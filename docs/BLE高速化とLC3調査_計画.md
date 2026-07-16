# 実装計画: BLE通信の高速化(UART4/LPUART1ボーレート引き上げ) + LC3導入の調査

> ブランチ: `refactor/layering` / 作成: 2026-07-15
> **この文書は自己完結した作業指示書である。** 軽量AIモデルが1ステップずつ実行できるよう、
> 対象ファイル(絶対パス)・変更内容・ビルド/検証手順・コミットまで各ステップに含める。
> Phase A(UART高速化) → Phase B(ペーシング最適化) → Phase C(LC3調査) の順。
> 1ステップ=1コミット、実機検証PASS前に次へ進まない。

> **進捗(2026-07-16 更新)**
> - **Phase A = 完了**。ただし実機検証の結論として制御UARTは **115200 で確定**
>   (9600の12倍)。460800は初回ATがまれに化け、921600/1Mbpsはリンク不成立だった
>   (詳細は自動メモリ [[ble-uart-max-baud]] / [[wb-build-tree-notify-fix]])。テレメトリは
>   **10Hz** 化済み。`comm_ble::Init()` に AT bring-up リトライを実装。関連コミット:
>   c6a89bf(115200)/ eb43a88(460800実験)/ 910747d(115200確定+リトライ)/ d59ce03(10Hz)/
>   2544ed2(BLE keep-alive で ACTIVE 維持)。
> - **Phase B = 一部未了**。B-1(`kChunksPerPump` 引き上げ)・B-2(チャンクロス対策)は未着手。
>   なお PC→U585 の fe41 write 経路(録音の REC_START/STOP が通る道)は
>   [[wb-build-tree-notify-fix]] の修正で疎通を回復済み。
> - **Phase C(LC3調査)= 未着手**。
> - 以降の作業は WiFi ホットスポットを立てられる環境で実機検証すること
>   ([検証環境メモ_WiFiホットスポット未設定.md](検証環境メモ_WiFiホットスポット未設定.md))。

---

## Context(なぜこの変更をするか)

BLE通信に2つの性能問題がある:

1. **通常テレメトリが1Hz**: `Secure/Core/Inc/app_config.h` の `CFG_TLM_BLE_PERIOD_MS=1000` で
   1秒に1回しかMiniStatus(39B)を送っていない。
2. **録音チャンク転送が遅い**: 3秒の録音(約47000サンプル)の転送に約70秒かかり、一部チャンクロスも発生。

**両方の根本原因は同一**: U585↔WB5MMG間のUART(U585側UART4 / WB5MMG側LPUART1)が
**9600baud固定**で、BLE notify 1回のATコマンド送信(`AT+BLE_NOTIF_VAL=1,2,0x<hex>`、
64Bペイロードで約130文字)に約200msかかること。BLEの無線速度ではなく、モジュール制御用
UARTチャネルがボトルネック。

### 調査で確定した事実(実装の土台)

- **U585側UART4はハード制約なし**: PCLK1=160MHz駆動(`RCC_UART4CLKSOURCE_PCLK1`)。
  115200/230400/921600いずれもBRR計算に余裕。GPIO(PC10/PC11)は現在`GPIO_SPEED_FREQ_LOW`
  だが、921600等の高速化時のみ`HIGH`以上への引き上げを推奨。**有効なボーレートは
  `CFG_BLE_BAUDRATE`のみ**(`main.c`の`MX_UART4_Init`は115200をハードコードしているが、
  `comm_ble.cpp`の`Init()`/`stm32wb_at_ll_Init()`が起動時に`CFG_BLE_BAUDRATE`で再Initして
  上書きするため、main.c側の値は「死んだ値」)。
- **WB5MMG側LPUART1もハード制約なし**: PCLK1=32MHz駆動(`RCC_LPUART1CLKSOURCE_PCLK1`、
  LSEではない)。理論上限は約10Mbps。TXはDMA駆動、GPIO(PA2/PA3)は既に`VERY_HIGH`設定済み。
  9600は単なるST公式リファレンスのデフォルト値。**変更には`main.c:353`の
  `hlpuart1.Init.BaudRate=9600`を変えてWB5MMGファーム再ビルド・再書き込みが必須**。
- **ボーレート依存の固定タイムアウトは`kChunksPerPump`のみ**: `comm_ble.cpp`の`PumpRecTx()`の
  `kChunksPerPump=2U`にコメント`/* ~200 ms/notify at 9600 baud */`があり、ボーレートを上げたら
  この値も増やさないとスループット向上を活かせない。他のタイムアウト
  (`CFG_BLE_REPLY_TIMEOUT_MS=1500`、`HAL_UART_Transmit`の1000ms等)は絶対時間なので変更不要
  (高速化すれば余裕が増えるだけ)。
- **⚠️ 最重要リスク**: ボーレートは**U585側とWB5MMG側で必ず一致**していないと通信が全く
  成立しない。片方だけ変えると`BLE=NG`になりBLE機能が完全に死ぬ。両側を同じ値にして、
  WB5MMG側→U585側の順(または同時)に書き込む。

### LC3について(Phase Cで調査のみ、実装は本計画のスコープ外)

調査の結果、**現行のSTM32WB5MMG環境(STM32CubeWB v1.18.0)にはLC3コーデックも
LE Audioプロファイル(BAP/ASCS)も含まれていない**。現行のP2P GATT+ATコマンド方式とは
全く別のプロトコルスタック(CIS/BIG Isochronous channels + BAP + LC3)が必要で、非常に大規模。
かつ現在のボトルネックはコーデック圧縮率ではなくUART 9600baudなので、UART高速化を先に
やる方が費用対効果が桁違いに高い。よって本計画ではUART高速化(Phase A/B)を実装し、
LC3は実現性調査(Phase C)にとどめる。

---

## Phase A — UART4/LPUART1 ボーレート引き上げ(1Hz問題+録音遅延の根治)

**⚠️ 全ステップ共通の鉄則**: U585側とWB5MMG側のボーレートは常に一致させる。不一致の状態を
作らない。各ボーレート変更後は必ず実機で`BLE=OK`とnotify受信を確認してから次に進む。

### Step A-1: 現状のベースライン記録(変更なし)
- **目的**: 変更前のBLE疎通とnotifyレートを記録し、比較基準を作る。
- **手順**: USB完全電源リセット後、`pc_side/status_monitor`でbleak接続し、5秒間のMiniStatus
  受信数(現状1Hz≒5フレーム)とCRCエラー0を記録。UARTログで`[TLM] radio: BLE=OK`を確認。
- **検証(PASS)**: BLE=OK、5秒で約5フレーム、CRC0。
- コミット: なし(記録のみ)。

### Step A-2: 両側のボーレートを115200に変更してビルド
- **目的**: まず控えめな12倍(9600→115200)で疎通を確認する。
- **対象ファイル**:
  1. `Secure/Core/Inc/app_config.h`: `CFG_BLE_BAUDRATE` を `9600U` → `115200U`。
  2. `d:/.../B-U585I-IOT02A/ble_module_fw_patch/main.c`: `hlpuart1.Init.BaudRate = 9600;`(353行目)
     → `115200`。
  3. WB5MMGビルドツリー側 `C:/Users/takut/wb/STM32CubeWB/Projects/P-NUCLEO-WB55.Nucleo/
     Applications/BLE/BLE_AT_Server/Core/Src/main.c` の同じ行(353) → `115200`
     (ble_module_fw_patch/main.c と同一内容に保つ)。
  4. (整合性のため任意) `Secure/Core/Src/main.c` の `MX_UART4_Init()` の
     `huart4.Init.BaudRate = 115200;` はそのままで可(既に115200、かつ実際には
     CFG_BLE_BAUDRATEで上書きされる)。CFG_BLE_BAUDRATEと違う値にはしないこと。
- **ビルド**:
  - WB5MMG: CubeIDEヘッドレスで `BLE_AT_Server/Debug` をビルド(ワークスペース `$env:TEMP\claude\hws2`)。
  - U585: `B-U585I-IOT02A_Secure/Debug` をビルド(ワークスペース `$env:TEMP\claude\hws`)。
  - 期待値: 両方 "0 errors"。
- コミット: まだしない(A-3の実機PASS後にまとめてコミット)。

### Step A-3: WB5MMG→U585の順で書き込み、疎通確認
- **目的**: ボーレート不一致期間を最小化しつつ両側を新ボーレートに揃える。
- **手順**:
  1. SW4=OFF/SW5=ON に切替(ユーザー操作。要確認)。
  2. `STM32_Programmer_CLI -c port=SWD mode=UR -d <BLE_AT_Server.elf> -v -rst` でWB5MMGへ書込。
     elfパス: `C:/Users/takut/wb/.../BLE_AT_Server/STM32CubeIDE/Debug/BLE_AT_Server.elf`。
  3. SW4=ON/SW5=OFF に戻す(ユーザー操作。要確認)。
  4. `STM32_Programmer_CLI ... -d <B-U585I-IOT02A_Secure.elf> -v -rst` でU585へ書込。
  5. USB完全電源リセット(WB5MMG間欠初期化不安定の切り分け。ユーザー操作)。
- **検証(PASS)**: UARTログで `[TLM] radio: BLE=OK`、bleakでMiniStatus受信・CRC0。
  **ここでBLE=NGやnotify無しなら、ボーレート不一致か設定ミス** → 両側の値を再確認して
  やり直す(9600に戻せば必ず復旧する安全策も念頭に)。
- コミット(U585側): `perf(ble): raise BLE control UART to 115200 baud (was 9600)`
  - `Secure/Core/Inc/app_config.h`, `Secure/Core/Src/main.c`(触った場合),
    `ble_module_fw_patch/main.c` をステージ。NS不変なら`NS_APP_VERSION`据え置き。

### Step A-4: BLEテレメトリ周期を短縮(1Hz→数Hz)
- **目的**: 115200化で1回のnotifyが約17msに短縮されるので、テレメトリ周期を上げられる。
- **対象**: `Secure/Core/Inc/app_config.h` の `CFG_TLM_BLE_PERIOD_MS`。
  `1000U` → まず `200U`(5Hz)。コメントの「9600-baud...blocks ~200ms」も新ボーレートに更新。
- **ビルド/書込**: U585 Secure のみ(WB5MMGは無関係)。
- **検証(PASS)**: bleakで5秒間に約25フレーム(5Hz)、CRC0。取りこぼし/切り詰めが無いこと
  (フレーム長が47Bであること)。もし詰まる/不安定なら周期を`500U`(2Hz)に緩める。
- コミット: `perf(ble): raise BLE telemetry cadence to 5 Hz (was 1 Hz)`

### Step A-5(任意): さらなる高速化(230400/460800)を試す
- **目的**: 115200で安定動作を確認できたら、余裕があれば更に上げて録音転送を短縮。
- **手順**: A-2〜A-3と同じ要領で `CFG_BLE_BAUDRATE` と WB側 `hlpuart1` を
  230400(または460800)に。**必ず両側同時**。GPIO速度(U585側UART4のPC10/PC11)を
  `GPIO_SPEED_FREQ_LOW`→`HIGH`以上に上げる必要が出るかは実機の安定性を見て判断
  (`Secure/Core/Src/stm32u5xx_hal_msp.c` のUART4 MspInit、GPIO_InitStruct.Speed)。
- **検証(PASS)**: BLE=OK、テレメトリ・録音転送が安定(CRC0、チャンクロス無し)。
  不安定なら1段階戻す。
- コミット: `perf(ble): raise BLE control UART to 230400 baud`(採用時のみ)

---

## Phase B — 録音チャンク転送のペーシング最適化

### Step B-1: kChunksPerPump をボーレートに合わせて引き上げ
- **対象**: `Secure/Core/Src/comm_ble.cpp` の `PumpRecTx()`。
  `constexpr uint32_t kChunksPerPump = 2U;` を新ボーレートに応じて増やす
  (115200なら12倍相当だが、まず`8U`程度から実機で詰まらない上限を探る)。コメント
  `/* ~200 ms/notify at 9600 baud */` も実測値に更新。
- **注意**: `PumpRecTx()`は`Service::poll()`から毎ループ呼ばれる。1回のpollで送るチャンク数を
  増やしすぎるとpoll全体がブロックしテレメトリ/OTA応答が遅れるので、実機で
  テレメトリ受信が乱れない範囲に収める。
- **検証(PASS)**: 3秒録音の転送時間が計測上短縮(目標: 115200で数秒〜10秒台)、
  `REC_END`の`total_samples`とPC側デコードサンプル数の差が縮小(理想は一致)。
- コミット: `perf(rec): increase recording chunk pump rate for higher baud`

### Step B-2(任意): チャンクロス対策(信頼性向上)
- **背景**: BLE notifyはWrite Without Response相当で、高速送信時にPC側が取りこぼす。
  現状`total_samples`とデコード数が一致しないことがある。
- **選択肢(実機で軽い方を採用)**:
  a) 送信間隔を少し空ける(poll跨ぎの自然なペーシングに任せ、kChunksPerPumpを欲張らない)。
  b) `REC_END`にCRC16があるので、PC側で欠落seqを検出し再送要求(新コマンド`REC_RESEND`)を
     出す簡易再送(実装コスト中)。
  c) チャンクにseqが既にあるので、PC側で欠落を検出したら記録し、ユーザーに再録音を促す
     (実装コスト小、暫定)。
- **検証(PASS)**: 3秒録音でサンプル数がほぼ一致(許容±1%以内)、WAV再生で音声が途切れない。
- コミット: `feat(rec): handle BLE chunk loss (<選んだ方式>)`

---

## Phase C — LC3導入の実現性調査(実装はしない、調査レポートのみ)

### Step C-1: LE Audio対応の可否を調査しレポート化
- **目的**: LC3/LE Audioを現行または近い構成で実現できるか結論を出す。実装はしない。
- **調査項目**:
  1. STM32WB5MMGのコプロファーム(`stm32wb5x_BLE_Stack_full_extended_fw.bin`)がCIS/BIG
     (Isochronous channels)をHCIレベルでサポートするか(Release_Notes精査)。
  2. LC3コーデックライブラリをST(またはBluetooth SIG参照実装)から入手できるか、
     U585(Cortex-M33)で動くか。ライセンス条件。
  3. LE Audio上位プロファイル(BAP/ASCS/PACS)を`BLE_AT_Server`ベースで自前実装する規模見積もり。
  4. **代替案**: LE Audioスタックを使わず、U585側でLC3ソフトエンコード→現行のGATT notify
     (REC_CHUNK経路)でADPCMの代わりにLC3フレームを送る方式の実現性
     (音質/CPU負荷/実装コストをADPCMと比較)。
  5. 本格LE Audioが必要なら、STM32WBA系など対応ハードへの移行が必要か。
- **成果物**: `docs/LC3導入_実現性調査.md`(新規)に結論(推奨/非推奨とその理由、代替案、
  概算工数)を記載。
- コミット: `docs: LC3/LE Audio導入の実現性調査レポートを追加`

---

## ガードレール(全Step共通)

1. **ボーレートは常にU585側とWB5MMG側で一致**。不一致状態を作らない。変更時は両側同時、
   検証は必ずWB書込→U585書込→USB電源リセット→BLE=OK確認の順。
2. `FullStatus_t` 165B / `MiniStatus` 39B / frame_codec の SOF/EOF/CRC16 は不変。
3. 既存 `Comm_*` 7ゲートウェイと `CommBridge_*` はシグネチャ凍結。
4. OTA信頼ルート(handleFrame/ota::Manager/NOR定数)に触れない。
5. NSを触ったコミットは `NonSecure/Core/Src/main.c` の `NS_APP_VERSION` を +1
   (本計画はSecure/PC/WBのみの変更予定なので通常は据え置き)。
6. 検証は必ずDebug構成・CubeIDEヘッドレスビルド・`STM32_Programmer_CLI`(SWD)書込み。
7. 各BLE検証の前にUSB完全電源リセット(WB5MMG間欠初期化不安定の切り分け)。
8. **安全策**: ボーレート変更でBLEが死んだら、両側を9600に戻して再書き込みすれば必ず復旧する。

## 検証方法(エンドツーエンド)

- **テレメトリ**: `pc_side/status_monitor`(またはbleak直結スクリプト)でMiniStatusを購読し、
  fps(周期)とCRCエラー数を計測。Phase A完了時に1Hz→5Hz、CRC0を確認。
- **録音転送**: bleakで`CMD_REC_START`→3秒→`CMD_REC_STOP`を送り、`REC_CHUNK`/`REC_END`到着まで
  の所要時間、受信チャンク数、`total_samples`とデコードサンプル数の一致を計測。Phase B完了時に
  9600時の約70秒から大幅短縮を確認。
- **回帰**: UART/TCP経由の通常テレメトリ・音声ストリーミング・OTAが影響を受けていないこと
  (BLE UARTのボーレートは他系統と独立なので影響は無いはずだが、念のため50Hzテレメトリと
  OTA往復を1回確認)。

## Critical Files

- `Secure/Core/Inc/app_config.h`(`CFG_BLE_BAUDRATE`, `CFG_TLM_BLE_PERIOD_MS`)
- `Secure/Core/Src/comm_ble.cpp`(`PumpRecTx()`の`kChunksPerPump`、UART再Init)
- `ble_module_fw_patch/main.c` および WB側 `.../BLE_AT_Server/Core/Src/main.c`
  (`hlpuart1.Init.BaudRate`、353行目)
- `Secure/Core/Src/stm32u5xx_hal_msp.c`(UART4 GPIO速度、高速化時のみ)
- 新規: `docs/LC3導入_実現性調査.md`(Phase C)
- 参照(WBビルド手順): `ble_module_fw_patch/README.md`
