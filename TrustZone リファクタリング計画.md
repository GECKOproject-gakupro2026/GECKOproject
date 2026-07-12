# TrustZone リファクタリング計画: NonSecure をアプリケーション層へ

## Context（背景・目的）

現在このプロジェクト（B-U585I-IOT02A エッジAIシステム）は、全センサー読み取り・全通信（Wi-Fi/BLE/UART）・音声/AI推論・OTA機構のすべてが **Secure 側**（`Secure/Core/Src/telemetry.cpp` 約1300行の巨大クラス `telemetry::Service`）に集中している。NonSecure 側（Bank2、OTA更新対象）は LED 点滅だけの最小デモアプリ（約200行）にとどまっている。

ユーザーの要求は「新ファームで、一定時間ホスト要求がなければセンサー/通信を止めメモリ解放して待機電力を下げる（テスト用に3秒閾値、低消費モード中は赤LEDをゆっくり点滅）」。この機能は本来「アプリケーション層」に属するべきロジックであり、これを機にレイヤー分離を TrustZone 境界に一致させる。

**確定した切り分け（ユーザー明示指示）:**

- **Secure に残す（OTA存続・通信安定性の信頼ルート。誤更新でOTA経路や通信が壊れると復旧不能になるため）**
  - OTA機構全体: フレーム復号（`handleFrame`）、NORステージング（`ota::Manager`）、Bank2書き込み（FW_APPLY）、BootGuard ロールバック、Stage-0 ローダー
  - 通信スタック3系統: Wi-Fi(EMW3080/SPI2)、Bluetooth(WB5MMG/UART4)、UART/VCP(USART1) の送受信・接続管理・フレーム処理（+ 関連 IRQ/DMA）

- **NonSecure に移す（頻繁に変更するアプリケーション層。OTA更新対象）**
  - センサーライブラリ + スケジューリング: ToF(VL53L5CX/I2C2)、環境(温湿度気圧/I2C1)、照度(VEML3235/I2C1)、モーション(加速度/ジャイロ/磁気/I2C1)。BSPドライバ + コンポーネントドライバ + I2C1/I2C2 HAL
  - 音声キャプチャ（MDF1マイク + DMA）と AI推論（X-CUBE-AI ランタイム + `ai_app.cpp` + `Core/AI/*`、モデル約256KB）
  - **新規**: 低消費電力ロジック（3秒無通信で IDLE、赤LEDスロー点滅）
  - LED制御（PH6赤/PH7緑、GPIO属性は既にNonSecure開放済み）

**データフロー:** NonSecure がセンサーを読み `FullStatus_t` POD構造体を埋め、NSCゲートウェイ `Comm_SendTelemetry()` で Secure 通信スタックに渡して送信。ホスト受信コマンドは Secure が受け、NonSecure が `Comm_PollHostCommand()` で毎ループ引き取る（= ホスト活動検知 = IDLEタイマーのリセット）。OTAフレームバイトは100% Secure内に留まり NonSecure には出さない（信頼ルート保証）。

---

## 設計詳細

### 1. NSCゲートウェイAPI（Secure→NonSecure公開）

全て `Secure/Core/Src/secure_nsc.c` に `CMSE_NS_ENTRY` で実装、`Secure_nsclib/secure_nsc.h` で宣言、`extern "C"` ラッパー経由で Secure 通信コードにブリッジ。**CMSE制約: スカラーとPOD構造体ポインタのみ。NonSecureから来たポインタは `cmse_check_address_range(ptr, len, CMSE_NONSECURE|CMSE_MPU_READ/READWRITE)` で検証してから Secure ローカルにコピーして使う。**

| シグネチャ | ブリッジ先 | 越境POD | 検証 |
|---|---|---|---|
| `int Comm_SendTelemetry(const FullStatus_t *st)` | `sendUart/sendTcp/sendBle` | `FullStatus_t`(165B) | `cmse_check_address_range` 後にSecureローカルへコピー。0=受理 |
| `int Comm_PollHostCommand(uint8_t *out)` | Secure受信リングを drain | `uint8_t` out | 書込可検証。1=バイトあり(=活動)、0=なし |
| `void Comm_SetTelemetryEnabled(uint32_t on)` | BLE/TCPプッシュ抑制トグル | スカラー | 不要 |
| `uint32_t Comm_GetLinkStatus(void)` | bit0=ble_alive/bit1=wifi_alive/bit2=ble_conn/bit3=tcp_client | スカラー戻り | 不要 |
| `void Secure_ConfirmBoot(void)` | **既存** boot_guard.cpp | - | - |

**共有ヘッダ新規作成 `Secure_nsclib/comm_dto.h`**: `FullStatus` の C-POD 双子 `FullStatus_t`（同一パックレイアウト、両側で `_Static_assert(sizeof==165)`）。C++ の `telemetry::FullStatus` とバイト同一を静的アサートで保証。

**センサー/音声/AIはゲートウェイ不要**: I2C1/I2C2/MDF1 を NonSecure ペリフェラルにすればBSPドライバがハードを直接叩く。Secure に届く必要があるのは `Comm_*` のみ。LED も PH6/PH7 は開放済みで `HAL_GPIO_WritePin` 直叩き。

### 2. ペリフェラル/メモリ再分割

`Secure/Core/Src/main.c` の `MX_GTZC_S_Init`（401-548行）を手編集。

**Secure→NonSecureへ移す:**
- `GTZC_PERIPH_I2C1`(425行) → NonSecure（環境/モーション/照度/EEPROM）
- `GTZC_PERIPH_I2C2`(429行) → NonSecure（ToF）
- `GTZC_PERIPH_MDF1`(441行) → NonSecure（MIC2キャプチャ）
- I2C1/I2C2 の SCL/SDA GPIOピン → `GPIO_PIN_NSEC`（PH6/PH7と同じ機構）
- MDF1マイクデータGPIO(CCK/SDIN) → NSEC
- 音声GPDMAチャネル（`haudio_mdf[1]`, telemetry.cpp:406-408のSEC設定を削除）→ NonSecure側で設定。**console RX(GPDMA1_Channel1)と物理チャネルが衝突しないこと必須**

**Secureのまま（触らない）:** SPI2(Wi-Fi)、USART1(+GPDMA1_Ch1 RX)、UART4(BLE)、OCTOSPI*(NOR/OTA)、全通信IRQ、ADF1（MIC1使わない）。ADC12/ADC4はMCU情報の所属をPhase Fで決定。

**共有RAM:** `FullStatus_t` は NonSecure RAM に置き、Secure が検証後に読む（新規共有領域不要）。受信コマンドリングは Secure RAM に置く。既存の SRAM3 NS carve-out（`NS_RUNNING_VERSION_ADDR=0x200BFFF0`, MPCBB main.c:540）が双方向共有の前例。

**.ioc は再生成しない**（手編集運用）。GTZC/GPIO/DMA変更は全て手編集。`.ioc`ドリフトはヘッダコメントで明記。

### 3. ファイル移動

**移動（Secure→NonSecure、Phase Fで Secure から削除）:**
- `Secure/Core/AI/*`（audio_net系）→ `NonSecure/Core/AI/`
- `Secure/Core/Src/ai_app.cpp` + `ai_app.hpp` → NonSecure
- `telemetry.cpp` を**分割**（wholesale移動ではない）:
  - センサー/音声部（`initSensors`420-480、`initAudio`379-418、`refreshSlowSensors`657-709、`collect`のセンサー/音声部711-768、音声バッファ/BSPコールバック、`Telemetry_GetAudioBuffer`）→ NonSecure新規 `sensors.cpp` + `app_service.cpp`
  - 通信部（`wifiModuleInit/tcpServerInit/sendTcp/pollTcp/sendUart/sendBle/initRadio/handleFrame/processRxByte`ほか mx_wifi/BLE/UART glue）→ **Secure に残す**、`comm_service.cpp` にリネーム

**複製（両側必要）:**
- HAL ドライバ: NonSecure が HAL I2C/MDF/DMA/I2C_Ex（+MCU統計移すならADC）を新たにコンパイル。Secure は SPI/UART/GPDMA/FLASH/OSPI を維持
- BSP `b_u585i_iot02a_bus.c`（I2Cバス層）→ NonSecure へ移動

**NonSecure `.cproject` 変更**（includepaths 45-54/132-141、Debug/Release両方）:
- 追加: BSP各コンポーネント（hts221/lps22hh/ism330dhcx/iis2mdc/veml3235/vl53l5cx/m24256）、`../Core/AI`、`../../Middlewares/ST/AI/Inc`、AI静的ライブラリの `-l/-L`
- 新規ソース `sensors.cpp/app_service.cpp/ai_app.cpp` + `Core/AI` を登録
- 既存 `secure_nsclib.o` リンク（65行）は維持（Comm_*ゲートウェイ呼び出し用）
- C++サポート（`-std`/libstdc++破棄ルール）を Secure `.cproject` にミラー

### 4. 低消費電力ステートマシン（全てNonSecure）

新規 `NonSecure/Core/Inc/app_config.h`:
```c
#define CFG_IDLE_TIMEOUT_MS      3000U   /* テスト値: 無通信でIDLE */
#define CFG_IDLE_LED_BLINK_MS    1000U   /* IDLE時 赤PH6 スロー点滅 */
#define CFG_ACTIVE_HB_MS          250U   /* ACTIVE時 緑PH7 ハートビート */
```

NonSecure `App_Main` ループ（現 main.c:145-151 のLEDデモを置換）:
- `lastHostActivityMs`: `Comm_PollHostCommand`==1 の度に `HAL_GetTick()` で更新
- `mode`: ACTIVE / IDLE

**遷移:** `HAL_GetTick()-lastHostActivityMs >= CFG_IDLE_TIMEOUT_MS` で ACTIVE→IDLE。次の `Comm_PollHostCommand()==1` で即 IDLE→ACTIVE。

- **ACTIVE:** センサー定期読み、`Comm_SendTelemetry`を50Hz、緑PH7ハートビート、赤PH6消灯
- **IDLE:** センサー読みスキップ、`BSP_RANGING_SENSOR_Stop(0)`でToF停止、`Comm_SendTelemetry`スキップ、任意で`Comm_SetTelemetryEnabled(0)`、赤PH6を1Hzスロー点滅、緑PH7消灯。低ループ間で WFI 可だが `Comm_PollHostCommand` は毎ループ継続

**ウェイク検知:** Secure通信スタックは IDLE 中もリスナー稼働（`pollTcp`/UART RX DMA/UART4 は NonSecure モードに依存しない）。バイト着信で Secure受信リングに入り、次の `Comm_PollHostCommand` が1を返す→ ACTIVE復帰。割り込みをNonSecureへ入れる必要なし（純ポーリングでCMSEコールバック複雑性を回避）。

**エッジケース:** 初回起動は `lastHostActivityMs=HAL_GetTick()` で ACTIVE開始（即IDLE防止）。OTAフレームバイト列（Secure内で消費、コマンドとして表面化しない）も活動として数えるため、Secure は**任意の**受信バイトで活動フラグを立て、`Comm_PollHostCommand` が活動をコマンドとは別に報告する。

### 5. 実装フェーズ（各々ビルド/実機検証可能）

- **Phase A — Comm_*ゲートウェイ（センサーはまだSecure）。GTZC変更なし。**
  `Comm_SendTelemetry/PollHostCommand/GetLinkStatus` + `comm_dto.h` 追加。NonSecure main がダミー `FullStatus_t`（固定カウンタ）を送信。
  *実機検証:* PCテレメトリにNonSecureのダミーカウンタ値が出る（構造体がCMSE越境しSecureが送信した証明）。文字送信で `Comm_PollHostCommand` が返す。SecureFaultなし。

- **Phase B — センサー1個を端から端まで移す（推奨: USERボタン=I2C不要でゲートウェイ経路を先に検証）。**
  ボタンGPIOをNSECにし、NonSecureで `st.button` を埋める。**I2Cフリップはまだ。**
  *実機検証:* ボタン押下でテレメトリの button フィールドが NonSecure 由来でトグル。

- **Phase C — 残りI2Cセンサー + ToF。最高リスクのI2C1/I2C2 GTZC再分割。**
  I2C1/I2C2 + GPIOピンをNSEC化。`initSensors/refreshSlowSensors/collect`センサー部 + BSP/コンポーネントドライバをNonSecureへ。**ToF I2Cリカバリ最初（telemetry.cpp:424）とVEML3235 SD_ALSパッチをNS側で再現。**
  *実機検証:* 全センサー値がNonSecure由来でライブ表示。ToF非ゼロ距離。GTZC不正アクセスフォルトなし。

- **Phase D — 音声 + AI。MDF1(+マイクGPIO+音声GPDMA)をNSEC化。**
  `initAudio`/音声バッファ/BSPコールバック/`Core/AI/*`/`ai_app.*` を移動。AI静的ライブラリをNS `.cproject`へ。
  *実機検証:* 音声RMS/peak/波形ライブ、`i`でNonSecure推論結果、音声ストリーム(`a`/`s`)動作。Bank2イメージが収まり起動する（§6サイズリスク）。

- **Phase E — 低消費電力 + LED。** `app_config.h`、ACTIVE/IDLE機械、ToF stop/start、LEDパターン。
  *実機検証:* 3秒無通信で赤PH6が1Hz点滅・テレメトリ停止・ToF停止。文字送信でACTIVE復帰・緑ハートビート・テレメトリ再開。

- **Phase F — Secure側デッドコード削除 + app_main縮小。**
  移動済みセンサー/音声/AIソースとinclude/ソース登録をSecureから削除。Secure `app_main.cpp` を通信+OTA+BootGuardのみに縮小。I2C1/I2C2/MDF1のSecure init削除。
  *実機検証:* 全回帰（各トランスポートでOTA更新、センサー/音声/AI/IDLE動作、BootGuardロールバック、通信影響なし）。Secureイメージ縮小を確認。

**リスク集中: Phase C(I2C)・Phase D(MDF/DMA)。それ以前はゲートウェイのみで低リスク。**

### 6. リスクと緩和

1. **CMSE制約（スカラー/POD限定）** → `comm_dto.h` C-POD双子 + 両側 `_Static_assert`。Phase AでPCパーサがバイト同一を見ることを検証
2. **NonSecureポインタ信頼** → 全ポインタゲートウェイで `cmse_check_address_range` 後にSecureローカルコピー。Phase Aで不正ポインタを渡して拒否を確認
3. **I2C/MDF GTZCフリップ（Phase C/D最高リスク）** → 1ペリフェラルずつフリップ、都度SWDでGTZCフォルト監視、NS bring-up実証までSecure initをフォールバックに残す。ToF-I2Cリカバリ順序/VEML3235 SD_ALS/MIC2-MDF1(MIC1不可)の既知ハマりをNS側再現
4. **mx_wifi非リエントラント(Secure専有)** → 設計上NonSecureは触らない、ハンドルを越境させない
5. **Bank2サイズ(AIモデル)** → NS FLASHは既に1MB(`ld:50`, `ota.cpp:23`)。Phase D後にNS `.map` で `.text+.rodata`<1MB確認。超過時は linkerリージョン + `ota.cpp kNsFlashSize`(106/116/218行連動) + NOR backupスロットを揃えて拡張。Phase DでサイズチェックとフルOTA往復テスト
6. **起動確認基準の拡張** → 現状「main到達」で `Secure_ConfirmBoot`。real機能を持つNonSecureでは弱い。**推奨: 初回 `Comm_SendTelemetry()==0` 後に確認呼び出し**（=Secureがテレメトリ1件受理）。センサー成否では gate しない（flaky sensorでロールバックループ回避）。タイムアウト付き（N秒以内に成功テレメトリなければ確認せず→BootGuardが不良OTAをロールバック）。Phase Eで決定/テスト
7. **DMAチャネル衝突(Phase D)** → `haudio_mdf[1]`のチャネル番号 vs GPDMA1_Ch1を監査、衝突なら音声チャネル再割当。Phase D後にconsole入力+音声同時で検証
8. **.iocドリフト** → 再生成しない、Secure main.cにハンドエディット済みペリフェラルセキュリティをコメントで明記

---

## 検証方法（全体）

各フェーズで「ヘッドレスビルド0エラー → SWD書込 → 実機観測」を徹底（このプロジェクトは「実機で観測して初めてPASS」の文化）:
- ビルド: `stm32cubeidec.exe --launcher.suppressErrors -nosplash -application org.eclipse.cdt.managedbuilder.core.headlessbuild -data <hws> -build "B-U585I-IOT02A_Secure/Debug"` および NonSecure
- 書込: `STM32_Programmer_CLI.exe -c port=SWD mode=UR -d <elf> -v`、通常リセットは `-c port=SWD mode=HOTPLUG -rst`
- テレメトリ確認: `pc_side/status_monitor` の app.py / ota_update.py --status、UART raw capture(utf-8化必須)
- SWD メモリ読み: `-r32 <addr> <n>`（Bank2バージョン `0x08100404` 等。ただしSecure領域はRDP保護で読めない）
- Wi-Fi: `pc_side/wifi_hotspot.ps1 -Action Start`（2.4GHz固定）、`find_board.ps1` でボードIP発見

## 主要変更ファイル

- `Secure/Core/Src/secure_nsc.c` / `Secure_nsclib/secure_nsc.h` — Comm_*ゲートウェイ + comm_dto.h
- `Secure_nsclib/comm_dto.h`（新規）— FullStatus_t POD双子
- `Secure/Core/Src/telemetry.cpp` — 分割（通信=Secure残留=comm_service.cpp、センサー/音声=NonSecureへ）
- `Secure/Core/Src/main.c`(401-548) — GTZC/GPIO/DMA再分割
- `NonSecure/Core/Src/main.c` + `NonSecure/.cproject` — 新アプリループ・IDLE機械・include/ソース追加
- `NonSecure/Core/Inc/app_config.h`（新規）— CFG_IDLE_TIMEOUT_MS ほか
- `NonSecure/Core/Src/sensors.cpp` / `app_service.cpp`（新規）— 移設したセンサー/音声/スケジューリング

---

## Phase A: 完了・実機検証済み（2026-07-12）

### 実装内容

- `Secure_nsclib/comm_dto.h`（新規）: `FullStatus`のC-POD双子`FullStatus_t`（165B、両側`_Static_assert`）
- `Secure_nsclib/secure_nsc.h` / `Secure/Core/Src/secure_nsc.c`: `Comm_Poll` / `Comm_SendTelemetry` / `Comm_PollHostCommand` / `Comm_SetTelemetryEnabled` / `Comm_GetLinkStatus` の5 NSCゲートウェイを追加。ポインタ引数は`cmse_check_address_range`で検証後、Secureローカルへコピーしてから使用
- `Secure/Core/Src/telemetry.cpp`: `Service::setNsDriven()` / `submitExternalStatus()` / `wifiTcpLinkBits()` を追加。`CommInit()`関数（Service初期化+ボーレート設定+NS駆動モード有効化）を新設。`poll()`にNS駆動モード時のコンソールRXポンプを追加
- `Secure/Core/Src/main.c`: 通信ペリフェラル初期化（OCTOSPI/SPI2/UART4/USART1等）とセンサー/音声初期化（I2C1/I2C2/ADF1、Phase C/Dで分離予定）を**Stage-0ジャンプ判定より前**に移動。ジャンプ前に`Comm_Init()`を呼び、Secure通信サービスをNS駆動モードで起動
- `NonSecure/Core/Src/main.c`: メインループを`Comm_Poll()`駆動+ダミー`FullStatus_t`送信+`Comm_PollHostCommand()`ポーリングに変更（v10）

### 発覚した重大バグと修正

1. **`Secure_JumpToNonSecure()`のNVIC/SysTick全消去がハングを引き起こした**（最重要）: 旧設計（NSは独立LEDデモ、Secureに戻らない前提）では`__disable_irq()` + `SysTick->CTRL=0` + 全NVIC ICER/ICPRクリアが正しかったが、新設計ではNonSecureが`Comm_*`ゲートウェイでSecureを呼び戻すため、Secure側の`HAL_GetTick()`（SysTick依存）とUART/BLE/Wi-Fi割り込みが死んだ状態になり、最初の`Comm_Poll()`呼び出しでSecure内のタイムアウト待ちが永久に返らずシステム全体がハングした（症状: 赤緑LED両方点灯のまま、UART出力ゼロ）。**修正: SysTick停止とNVIC全消去を削除**、TrustZoneの割り込みルーティング（NVIC ITNS）が既にSecure/NonSecureの向き先を決めているため全消去は元々不要だった
2. **UARTコンソール受信バイトの経路消失**: 旧設計では`App_Main()`の外側ループが`Console_GetChar()`→`processRxByte()`を毎回呼んでいたが、NS駆動モードでは`App_Main()`が実行されず、この経路が消えてOTAステータス照会等のフレームコマンドが一切応答しなくなった。**修正: `Service::poll()`内にNS駆動モード時のみ`Console_GetChar`ポンプを追加**（`Comm_Poll()`が全てのSecureサービス駆動の起点になるため）
3. **ボーレート初期化漏れ**: `App_Main()`が持っていた`CFG_CONSOLE_BAUDRATE`(921600)への再設定が`Comm_Init()`に引き継がれておらず、CubeMXデフォルト(115200)のままでPC側と噛み合わずログが一切見えなかった。**修正: `CommInit()`内にボーレート設定を移動**

### 実機検証結果

- UART: `ver=2, button=<NonSecureカウンタ>, uptime_ms=<NonSecure HAL_GetTick>`のフレームが50Hzで連続受信、CMSE越境が実証された
- LED: 緑PH7が250ms間隔で正常点滅（NonSecureメインループが正常に周回）
- OTAステータス照会（`ota_update.py --status`）: UART経由で正常応答（Secureのフレーム処理・OTA機構が引き続き機能することを確認）
- TCP経路は本セッションでは未再検証（ホットスポットが停止していたため）。UARTと共通コードパスなので設計上は動作するはずだが、Phase Bで要再確認

### 既知の制約・次フェーズへの申し送り

- センサー/音声はまだSecureにある（Phase C/Dで移行）。現状`Comm_Init()`前にI2C1/I2C2/ADF1初期化も行っている（Phase C/Dでこの部分がNonSecure側の初期化に置き換わる）
- `Secure_ConfirmBoot()`の呼び出しタイミング（起動即時 vs 初回テレメトリ受理後）はまだ見直していない。Phase Eで計画通り「初回`Comm_SendTelemetry()==0`後に確認」へ変更予定

---

## Phase B: 完了・実機検証済み（2026-07-12）

### 実装内容

- `Secure/Core/Src/main.c`: USERボタン(PC13)をGTZC GPIOでNSEC化（PH6/PH7と同じ`HAL_GPIO_ConfigPinAttributes`機構）
- `Secure/Core/Src/telemetry.cpp`: `Service::collect()`から`BSP_PB_GetState`呼び出しを削除、`Service::initSensors()`から`BSP_PB_Init`を削除（コメントで移行先を明記）
- `NonSecure/Core/Src/main.c`: `button_init()`/`button_read()`を追加（生GPIO、入力・プルダウン、既存BSP設定を再現）。`build_status()`で`st->button`に実測値を設定

### 実機検証結果

- USERボタン押下でテレメトリの`button`フィールドが0→1に変化することを確認（NonSecure由来の実測値がCMSE境界を越えて反映）

---

## Phase C: 完了・実機検証済み（2026-07-12）

### 実装内容

- `Secure/Core/Src/main.c`: GTZC I2C1/I2C2を`GTZC_TZSC_PERIPH_SEC`→`GTZC_TZSC_PERIPH_NSEC`に変更。I2C1(PB8/PB9)・I2C2(PH4/PH5)・**ToF LP/シャットダウンピン(PH1)**をNSEC化。`MX_I2C1_Init()`/`MX_I2C2_Init()`呼び出しをStage-0起動フローから削除（GTZCフリップ後Secureはこれらのペリフェラルに触れない）
- `Secure/Core/Src/telemetry.cpp`: `initSensors()`からToF/env/motion/light初期化を全削除（no-op化）。`refreshSlowSensors()`からenv/light/ToF部分を削除（MCU情報更新のみ残す）。`collect()`からモーションセンサー読み取りを削除
- `NonSecure/Core/Src/sensors.c`＋`sensors.h`（新規）: `Sensors_Init()`/`Sensors_Refresh()`/`Sensors_Stop()`/`Sensors_Resume()`をC言語で実装。BSPセンサードライバ（`BSP_ENV_SENSOR_*`/`BSP_MOTION_SENSOR_*`/`BSP_LIGHT_SENSOR_*`/`BSP_RANGING_SENSOR_*`）は内部で`BSP_I2C1_Init`/`BSP_I2C2_Init`を自動的に呼ぶ自己完結設計のため、NonSecure側で手動I2C初期化は不要と判明
- `NonSecure/Core/Inc/b_u585i_iot02a_conf.h`（新規）: SecureのCore/Incにあった同名ファイルをコピー（BSPドライバがビルド時に要求する設定ヘッダ）
- `NonSecure/.cproject`: BSPインクルードパス追加。**ユーザーがCubeIDE GUIでプロジェクトをリフレッシュし、`Drivers`ソースエントリに除外リスト（`excluding=`、Wi-Fi/BLE/EEPROM/OSPI/未使用コンポーネント等Secure専有ドライバを除外）を追加**して解決（ヘッドレスビルドのCDT自動ディスカバリだけでは新規`Drivers/BSP`ディレクトリのソースファイルが認識されなかった）

### 発覚した問題と修正

1. **CDTヘッドレスビルドがBSPドライバを認識しない**: `.cproject`の`sourceEntries`に`Drivers`が指定されていても、ヘッドレスビルド(`stm32cubeidec.exe ... -build`)だけでは新規に追加されたサブディレクトリ(`Drivers/BSP/*`)のソースファイルがコンパイル対象として自動収集されなかった（"Unknown destination type (ARM/Thumb)"のリンクエラーで発覚）。CubeIDE GUIでのプロジェクトリフレッシュが必要だった
2. **VL53L5CX(ToF)のLPピン(PH1)解放漏れ**: I2C1/I2C2バスとそのGPIO(PB8/PB9、PH4/PH5)をNSEC化しても、ToFセンサー固有の`vl53l5cx_i2c_recover()`（I2Cバスリカバリシーケンス）が使う独立したシャットダウン/LPピン(PH1)がSecureのまま残っていたため、環境/照度/モーションセンサーは正常動作する一方でToF初期化だけ失敗し続けた（`tof_ok=0`固定）。**教訓: センサーのI2Cバス本体だけでなく、専用の制御/リセットGPIOも個別に洗い出してNSEC化する必要がある**

### 実機検証結果

- 環境センサー: 温度41.0℃・湿度36.7%・気圧1006.9hPa（NonSecure由来、妥当な値）
- 照度センサー: 生値2761（NonSecure由来）
- モーションセンサー: 加速度Z軸+1013mg（基板水平置きで重力1Gと一致）
- ToFセンサー（PH1修正後）: `tof_ok=1`、距離1750mm（手をかざして測定、妥当な値）
- LED: 緑PH7が250ms間隔で正常点滅、NonSecureメインループが全センサー読み取りを含めて正常に周回
- ボタン押下でOTAローダーに留まるケース（`App_Main()`実行）は`CommInit()`を再利用し`setNsDriven(false)`に戻す設計。今回のセッションでは未検証（通常の自動起動経路のみ確認）
