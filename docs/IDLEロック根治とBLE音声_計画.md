# 実装計画: IDLEロック根治・厳密状態遷移・BLE音声/プロトコル・不揮発ログ

> ブランチ: `refactor/layering` / 作成: 2026-07-16
> **この文書は自己完結した作業指示書である。** 軽量AIモデルが1ステップずつ実行できるよう、
> 対象ファイル(絶対パス)・変更内容・ビルド/検証手順・コミットまで各ステップに含める。
> **1ステップ=1コミット。実機検証PASS前に次へ進まない。**
>
> 前提: 通信リンクの状態遷移リファクタ(per-linkのLinkFsm、SRAM state_log、LINK_STANDBY、
> TIME_SYNC 等)は完了済み。詳細は [通信リンク状態遷移_設計.md](通信リンク状態遷移_設計.md)。
> 本書はその続きとして、新たに確定した根本原因(IDLEロック)と、BLE音声/プロトコル/
> 不揮発ログの実装計画をまとめる。

---

## Context(なぜこの変更をするか)

ユーザー報告: 「UARTテレメトリが起動時50Hzだが、すぐに約5Hzに落ちる。その間 赤LEDが
ゆっくり点滅(=IDLE状態)している。音声のストリーミング・録音・再生ができない。しかし
センサーデータはapp.pyに届いている」。

調査の結果、これは環境要因ではなく **`Secure/Core/Src/console.cpp` のRX不具合を起点とする
自己増強ループ** であると確定した(コードで全箇所検証済み):

1. `Console_GetChar(0)` は `timeout_ms==0` だと do/while が1回だけ回り、**1バイトしか
   返さない**([console.cpp:177-187](../Secure/Core/Src/console.cpp#L177))。`Service::poll()` は
   1周に1回しか呼ばない([comm_service.cpp:615](../Secure/Core/Src/comm_service.cpp#L615))ため、
   **受信は1バイト/ループが上限**になる。
2. **DMAリングにオーバーラン検出が無い**: 空判定は `if (head != rxTail)` だけ
   ([console.cpp:180](../Secure/Core/Src/console.cpp#L180))。循環DMA(1024B、`kRxBufSize`)が
   `rxTail` をちょうど1周して重なると `head==rxTail` が「空」に誤認され、**以後永久に
   受信不能**になる。
3. NonSecure `AppState_Tick` の `lastActivityMs` を更新する唯一の経路は `Trigger_Poll`
   (照度/音圧しきい値は既定0=無効)であり、その実体は `Comm_PollHostCommand()`。受信が
   死ぬと3秒でIDLEに入る。しかも `idle_allowed()` がIDLEの**入口・出口の両方**で使われて
   いる([app_state.c:69,91](../NonSecure/Core/Src/app_state.c#L69))ため、**IDLEが吸収状態**に
   なり、新しいバイトが届かない限り出られない。
4. `kLinkIdleMs=3000`([comm_service.cpp:636](../Secure/Core/Src/comm_service.cpp#L636))が
   `CFG_IDLE_TIMEOUT_MS=3000` と同値で共振する: IDLE入り → `uartLink_` も同時にIdleへ →
   TCP acceptの間引き間隔が60秒から5秒に戻る → `MX_WIFI_Socket_accept()` が約160-300ms
   ブロック → poll()が遅延しさらに受信バイトが溜まる。
5. 逃げ道のはずの `UART_FLAG_RXNE` ガード([comm_wifi.cpp:308](../Secure/Core/Src/comm_wifi.cpp#L308))
   は**死にコード**(コンソールRXはDMA駆動でRXNEが立たない)。既知欠陥として過去に
   文書化されていたが未修正だった(`git show 711abbd^:docs/refactoring/既知の問題_IDLE復帰の間欠的不安定性.md`
   で復元可能)。
6. **音声が使えない理由**: `setAudioStream(true)` はUARTで `'a'` を受信して初めて動く
   ([comm_service.cpp:513](../Secure/Core/Src/comm_service.cpp#L513))。RXが死んでいるためコマンドが
   届かない。BLE録音の開始コマンドも同様に届かない経路がある。`Sensors_Stop()` はToFのみで
   マイクには無関係。
7. **センサーデータだけ届く理由**: 送信(TX)は受信(RX)と独立しており、`AppState_Tick` が
   IDLE周期 `CFG_IDLE_TELEMETRY_MS=200`(=観測された5Hz)で送り続けるため。

この根治(Phase A)を最優先で行い、その後にユーザーが要求する4つの追加機能
(BLE音声の専有送信・状態の可視化と厳密な状態遷移・不揮発ログ・BLE全センサー送信)を
実装する。

### 設計判断: NS→Secure へ状態を渡す経路
候補として `Comm_SetTelemetryEnabled` の引数に状態を相乗りさせる案があったが**採用しない**
(隠れた罠になるため)。ガードレールは「既存7ゲートウェイのシグネチャ不変・**追加は可**」
なので、**追加のNSCゲートウェイ `Comm_SetDeviceState(uint32_t)` を新設**する
(`NonSecure/Core/Inc/comm_api.h`、`Secure/Core/Src/secure_nsc.c`、`Secure/Core/Inc/comm_backend.h`
に追加、既存7関数は不変)。

---

## 共通ビルド/フラッシュ/検証レシピ

```
CUBE = C:\ST\STM32CubeIDE_1.19.0\STM32CubeIDE\stm32cubeidec.exe
PROG = C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe
U585 workspace = $env:TEMP\claude\hws   (Secure: "B-U585I-IOT02A_Secure/Debug",
                                         NonSecure: "B-U585I-IOT02A_NonSecure/Debug")
```
- Secureのみの変更: `$CUBE ... -data $ws -cleanBuild "B-U585I-IOT02A_Secure/Debug"`。
- NonSecureを触るステップ: NonSecureも同様にビルド。
- フラッシュ: `$PROG -c port=SWD mode=UR -d <elf> -v -rst`(U585はSW通常位置)。
- **bleak(BLE)検証の前後は必ずUSB完全電源リセット**(ソフトリセット`-rst`だけではWB5MMGが
  復旧しない既知の癖)。
- **WiFiホットスポット未設定環境**(このPC)ではTCP関連の検証は推論のみ・後日
  ([検証環境メモ_WiFiホットスポット未設定.md](検証環境メモ_WiFiホットスポット未設定.md))。
- 現在の `NS_APP_VERSION`: [NonSecure/Core/Src/main.c](../NonSecure/Core/Src/main.c) を参照
  (このステップ群の実行前の値を確認してから+1していくこと)。

---

## Phase A — IDLEロック根治(最優先・全ての前提)

### Step A1: console.cpp のリング全排出 + DMAオーバーラン検出
- **対象**: `Secure/Core/Src/console.cpp` / `console.h`、`Secure/Core/Src/comm_service.cpp`
  (`poll()` 冒頭のUARTドレイン部)。
- **変更**:
  - 受信量を**距離**で管理する: `rxPending += (head - rxLastHead) % kRxBufSize` のように
    増分を追跡し、単純な等値比較 (`head != rxTail`) をやめる。
  - `rxPending > kRxBufSize` を検知したらオーバーラン: カウンタを増やし `rxTail = head` で
    再同期する(取りこぼした窓は諦めて先頭から再開)。
  - 「空」判定を `rxPending == 0` にする(周回によって偽装されない)。
  - `Console_ReadBlock(uint8_t *dst, size_t maxLen)` と `Console_GetRxOverrunCount()` を追加。
    既存の `Console_GetChar()` は同じ会計の上で動作させ、呼び出し元(`comm_wifi.cpp`等)を
    壊さない。
  - `Service::poll()` では `Console_GetChar` を1回呼ぶだけでなく、**1回のpollで最大256B
    程度をブロック単位で `processRxByte` ループに流す**ようにし、`uartLink_` の更新は
    ブロック単位で1回でよい。
  - `[PROF]` ログにオーバーラン件数を追加(診断用)。
- **検証**: `python pc_side/status_monitor/app.py` を実際にターミナルで起動し、UART接続して
  ログを観察する。50Hzが**60秒以上**維持され5Hzに落ちないこと、LEDが緑(ACTIVE)のままである
  こと、`overrun=0` が続くこと。次に `a` を送って音声ストリーミングが開始することを確認
  (=RXが生きている直接証拠)。
- **コミット**: `fix(console): drain whole RX ring per poll and detect DMA lap overrun`

### Step A2: accept間引きと `uartLink_` 老化をNS idleタイムアウトから分離
- **対象**: `Secure/Core/Src/comm_service.cpp`、`Secure/Core/Src/comm_wifi.cpp`。
- **変更**: `kLinkIdleMs` を3000→10000にして3秒/3秒の共振を解消。**死んでいた
  `UART_FLAG_RXNE` ガードを、Step A1の会計を使う `Console_RxPending()` 相当の判定に
  置き換えて修復**する。`!comm_wifi::NetUp()` 時のaccept間引きは維持。Secureのみの変更。
- **検証**: COM9で `[PROF]` を見ながら、無通信からのIDLE入り→再度コマンド送信での復帰が
  安定すること。
- **コミット**: `fix(comm): stop accept poll and uartLink aging from resonating with the NS idle timeout`

### Step A3: IDLEを吸収状態でなくする
- **対象**: `NonSecure/Core/Src/app_state.c`。
- **変更**: `idle_allowed()` を「IDLE入場専用」の判定と、「`wake_requested()`(活動の
  **立ち上がりエッジ**で復帰)」に分割する。`NS_APP_VERSION` を+1。
- **検証**: 連続5回、意図的に無通信でIDLEへ落としてから最初のコマンド1発で確実に
  復帰すること。
- **コミット**: `fix(app): make IDLE exit on a fresh activity edge, not on the absence of a timeout`

---

## Phase B — BLE音声がリンクを専有する

### Step B1: 音声送信中はセンサー送信を止める + 送信速度を上げる
- **対象**: `Secure/Core/Src/comm_ble.cpp`(`IsRecTxActive()` を新規公開。`PumpRecTx()` の
  `constexpr uint32_t kChunksPerPump = 2U;` を2→8に引き上げ。コメント
  `/* ~200 ms/notify at 9600 baud */` は現在115200なので陳腐、実測値に更新)、
  `Secure/Core/Src/comm_service.cpp`(`SendStatus` を呼ぶブロックを `!comm_ble::IsRecTxActive()`
  でゲートする=「音声送信中はセンサーテレメトリを止める」)。
- **注意**: `PumpRecTx()` は `Service::poll()` から毎ループ呼ばれる。1回のpollで送る
  チャンク数を増やしすぎるとpoll全体がブロックし他の応答が遅れるので、実機でテレメトリ・
  OTA応答が乱れない範囲に収める(8は目安、詰まるなら4程度に戻す)。
- **検証**: **bleak電源リセット必要**。3秒録音の転送時間を前後比較(目標: 数秒程度に短縮)。
  `REC_END` の `total_samples` とPC側デコードサンプル数の差が縮小(理想は一致)。
- **コミット**: `perf(ble): give the audio transfer the notify link to itself and pump 8 chunks per call`

### Step B2(任意・follow-up): チャンクロス対策
- **背景**: BLE notifyはWrite Without Response相当で、高速送信時にPC側が取りこぼすことが
  ある。`REC_END` にCRC16があるので、PC側で欠落seqを検出して記録し、ユーザーに再録音を
  促す(実装コスト小)か、再送要求コマンドを新設する(中コスト)。
- **検証**: 3秒録音でサンプル数がほぼ一致(許容±1%以内)、WAV再生で音声が途切れないこと。
- **コミット**: `feat(rec): handle BLE chunk loss (<選んだ方式>)`

---

## Phase C — 状態の可視化 → 厳密な状態遷移

### Step C1: デバイス状態をMiniStatusに載せ、app.pyに表示(挙動変更前の可観測性)
- **対象**: 新規NSCゲートウェイ `Comm_SetDeviceState(uint32_t)`(`NonSecure/Core/Inc/comm_api.h`、
  `Secure/Core/Src/secure_nsc.c`、`Secure/Core/Inc/comm_backend.h`、`Secure/Core/Src/comm_service.cpp`。
  既存7ゲートウェイは不変)。NonSecureの `AppState_Tick` が状態変化時にこれを呼ぶ。
  `Secure/Core/Src/comm_ble.cpp` の `SendStatus()`(`telemetry.hpp` の `MiniStatus.flags`。
  現状bit0=ble/bit1=wifi/bit2=tof_okのみ使用)で **bit3-4に状態値をOR**する(MiniStatusは
  39Bのまま、サイズ不変)。`pc_side/status_monitor/protocol.py` で `device_state` を復号、
  `pc_side/status_monitor/app.py` に状態表示ラベルを追加(`power_var` と同じ並びに)。
  `NS_APP_VERSION` を+1。
- **検証**: app.py接続中、IDLE⇔ACTIVEで表示ラベルが実際のLED状態と一致すること。
- **コミット**: `feat(proto): carry the device state in MiniStatus.flags bits 3-4 and show it in the app`

### Step C2: 厳密なフラグ駆動の状態遷移
- **対象**: `NonSecure/Core/Src/app_state.c`、`Secure/Core/Inc/frame_codec.h` と
  `pc_side/status_monitor/protocol.py` に `FRAME_CMD_ENTER_COMM`(空きIDから、例0x0F)/
  `FRAME_CMD_STOP_COMM`(例0x10)を**同一の値で追加**、`Service::handleFrame` に
  `FRAME_CMD_LINK_STANDBY` と並べて実装、`pc_side/status_monitor/app.py` に通信開始/停止
  ボタン(`_sync_time`/`_send_standby` と同じ送信パターン)。state_logに `DeviceActive`/
  `DeviceIdle` イベントをpush(Step C1のゲートウェイ経由でSecure側から)。`NS_APP_VERSION` を+1。
- **変更内容**: 時間ベースの自動復帰を**明示フラグ**に置き換える。IDLEからの離脱は
  **明示的なUARTコマンド(ENTER_COMM)受信のみ**で行う。フラグが立たなければIDLEを
  維持し続ける(照度/音圧しきい値0=無効は「フラグを立てない」という既存仕様のまま)。
- **検証**: 無操作(keep-aliveのみ送信、コマンドは送らない)で**60秒以上待機状態を
  維持**すること(新セマンティクスの核心)。その後 `ENTER_COMM` 送信で確実にACTIVEへ
  移行し、`STOP_COMM` または無コマンドの継続でIDLEへ戻ることを確認。
- **注意**: **この変更はボードを意図せずIDLEに閉じ込め得る**。必ずgitで前段階に戻せる
  状態を保ち、実機で異常が出たら安全にロールバックすること。
- **コミット**: `feat(app): strict flag-driven state machine entered and left by explicit UART commands`

---

## Phase D — 不揮発ログ

### Step D1: 空きOSPI NORに状態ログを永続化
- **対象**: 新規 `Secure/Core/Inc/nvm_log.hpp` + `Secure/Core/Src/nvm_log.cpp`、
  `Secure/Core/Src/state_log.cpp`(既存SRAMリングとの連携)。
- **領域**: **NORアドレス 0x600000以降**を使用する(OTAのstaging 0x200000-0x400000・
  backup 0x400000-0x600000から最大距離を取り、信頼ルートに触れない)。
- **方式**: 13B(`{tick_ms, wall_ms, event, ret_val}`)のレコードを4KBサブセクタに追記し、
  **サブセクタが満杯になった時だけ消去**(約315件毎)してローテートし、drop-oldestに
  する(既存SRAMリングのindex-remap方式を踏襲)。ヘッドポインタは `TAMP->BKP1R`
  (BKP0RはBootGuardが使用中につき使わない)。既存 `state_log` に**欠けている `Reset()`
  を追加**する。
  **消去処理はホットパス(poll()の毎回)で行わない**こと — ブロッキングな `Erase_Block` を
  そこに置くと、Phase Aで解消した停止クラスの問題を再発させる。専用のタイミング
  (例: リンクIdle時、または明示的なログ書き込みリクエスト時)でのみ行う。
- **API**: `BSP_OSPI_NOR_Init/Read/Write/Erase_Block`(`Drivers/BSP/B-U585I-IOT02A/b_u585i_iot02a_ospi.h`)。
- **検証**: 状態遷移を数回起こしてから電源リセットし、ログが失われず残っていることを確認。
- **コミット**: `feat(log): persist the state log to free OSPI NOR with drop-oldest subsector rotation`

### Step D2: app.pyからログ取得・保存・リセット
- **対象**: `Secure/Core/Inc/frame_codec.h` / `pc_side/status_monitor/protocol.py` に
  `FRAME_CMD_LOG_REQ`(例0x11)/`FRAME_CMD_LOG_RESP`(例0x12)/`FRAME_CMD_LOG_RESET`
  (例0x13)を同一追加。`Secure/Core/Src/comm_service.cpp` の `FRAME_CMD_STATUS_REQ` →
  `FRAME_CMD_STATUS_RESP` の実装パターンを流用し `sendResponse` で応答、大きいログは
  `FRAME_CMD_FW_CHUNK` のu32オフセット方式で分割送信。NACKエラーコードは7以降を使う。
- **重要**: `pc_side/status_monitor/app.py` の `_poll_queue` には現状 **ACK/NACK/STATUS_RESP
  系の受信分岐が一切無い**(`decode_status` が該当コマンドに `None` を返し黙って捨てられる)。
  `decode_status` の呼び出しより前に、`CMD_LOG_RESP` を明示的に処理する分岐を**必ず追加**
  すること。取得したログはボタン操作で `.txt` として保存する(`RECORD_DIR` と
  `_save_wav` と同じ保存パターンを踏襲)。「ログリセット」ボタンも追加する。
- **検証**: ログ取得ボタン押下でボードから最新ログが降ってきて `.txt` に保存されること、
  リセットボタンでボード側ログがクリアされ次回取得が空であること。
- **コミット**: `feat(ui): fetch, save and reset the board's non-volatile state log from the app`

---

## Phase E — BLEで全センサーデータを送る(最大の作業・最後)

### Step E1: フラグメント多重notify送信 + PC側再組立
- **対象**: `Secure/Core/Src/comm_ble.cpp`、`Secure/Core/Inc/frame_codec.h` /
  `pc_side/status_monitor/protocol.py` に `FRAME_CMD_STATUS_FRAG`(例0x14)、
  `pc_side/status_monitor/app.py` に再組立ロジック。
- **設計**: 既存 `MiniStatus`(39B、`FRAME_CMD_STATUS_MINI`)は**一切変更しない**。別経路として
  `PumpRecTx()` と同じ**生TLV方式**(`Frame_Encode` を使わない)を採用する。理由:
  真の制約はBLE GATT特性長(64B)ではなく**160BのATヘックス文字列バッファ**であり、
  本プロジェクトは過去に47Bペイロードで `str_received` オーバーフローを踏んでいる
  (`docs/通信リンク状態遷移_設計.md` 参照)。ヘッダは
  `[cmd u8][seq u16][frag_idx u8][frag_total u8][slice]`(payload ≤ 58B程度に保守的に)。
  FullStatus相当(165B、`wave[32]` や RAM/heap/flash等を含む)を約3フラグメントに分割し、
  約1Hzで送信する。Step B1の `IsRecTxActive()` で音声送信中は停止する。
  **WB5MMGの再フラッシュは不要**(真のATT MTU拡大は行わない、この方式で完結)。
- **検証**: **bleak電源リセット必要**。BLEの全データタブとUARTセッションの値を
  フィールドごとに突き合わせて一致を確認。
- **コミット**: `feat(ble): fragment the full sensor set across notifies with PC-side reassembly`

---

## ガードレール(全Step共通)

- 触れない: 7つの `Comm_*`([comm_api.h](../NonSecure/Core/Inc/comm_api.h))/7つの
  `CommBridge_*`([comm_backend.h](../Secure/Core/Inc/comm_backend.h))の**既存**シグネチャ・
  意味(追加関数は可)。`FullStatus_t`(165B)/`MiniStatus`(39B)/`frame_codec` のSOF/CRC
  レイアウト。OTA信頼ルート(`handleFrame` のFW_* cases、boot_guard、NORのstaging/backup領域)。
- 新しいコマンドIDは `frame_codec.h` と `protocol.py` に**同一コミットで同一値**を追加する。
  不一致は無言の通信断になる(`_poll_queue` は未知コマンドをログ無しで捨てる)。
- NonSecureを触ったStepは `NS_APP_VERSION` を+1(A3・C1・C2)。A2・B1・D1・D2・E1は
  Secure/PCのみで据え置き。
- 各Step: Debug構成・CubeIDEヘッドレスビルド・SWD書込・上記の検証を確認してからコミット。

## 実行順序の根拠

Step A1を飛ばしたり順序を変えたりしないこと — 以降のすべてのステップはRXが正常に
生きていることを前提にしている。Phase A(RX根治)→ Phase B(音声専有、最小変更で高価値)
→ Phase C(可視化してから挙動変更、という安全な順序)→ Phase D(ログはCで作る状態遷移の
記録先として自然な位置)→ Phase E(最大かつ最も後回しにしてよい作業)の順。

## 検証環境の制約(必ず考慮)

- **WiFiホットスポット未設定**(IP=0.0.0.0)。TCP関連の検証は推論のみとし、
  ホットスポットを立てられる環境で後日まとめて確認する
  ([検証環境メモ_WiFiホットスポット未設定.md](検証環境メモ_WiFiホットスポット未設定.md))。
- **bleak(BLE)検証は毎回USB完全電源リセットが必要**(ソフトリセット`-rst`だけでは
  WB5MMGが復旧しない既知の癖)。
- 本計画にWB5MMGの再フラッシュは含まれない。

## 本計画スコープ外の既知follow-up

- **LC3/LE Audio導入の実現性調査**(旧 `docs/BLE高速化とLC3調査_計画.md` Phase C、未着手):
  現行のSTM32WB5MMG環境(STM32CubeWB v1.18.0)にはLC3コーデックもLE Audioプロファイル
  (BAP/ASCS)も含まれておらず、全く別のプロトコルスタック(CIS/BIG Isochronous channels)が
  必要で非常に大規模。調査観点: (1) コプロファームのCIS/BIGサポート有無、(2) LC3ライブラリの
  入手性・ライセンス、(3) 自前実装の規模見積もり、(4) 代替案としてU585側でLC3ソフト
  エンコード→現行GATT notify経路で送る方式の実現性、(5) STM32WBA系への移行要否。
  成果物は `docs/LC3導入_実現性調査.md`(調査のみ、実装はしない)。

## Critical Files

- `Secure/Core/Src/console.cpp`(Step A1 = 最重要ファイル)
- `Secure/Core/Src/comm_service.cpp`(poll/リンク状態/handleFrame)
- `Secure/Core/Src/comm_wifi.cpp`(accept間引き)
- `Secure/Core/Src/comm_ble.cpp`(音声送信・フラグメント送信)
- `NonSecure/Core/Src/app_state.c`(IDLE/厳密状態遷移)
- `pc_side/status_monitor/app.py`(状態表示・ログ取得UI・`_poll_queue`分岐)
- `Secure/Core/Src/state_log.cpp` →(新規)`Secure/Core/Src/nvm_log.cpp`
- `Secure/Core/Inc/frame_codec.h` / `pc_side/status_monitor/protocol.py`(コマンドID、
  常に同一値で両方に追加)
