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

---

## Phase D: 完了・実機検証済み（2026-07-12、設計を途中で見直し）

### 当初計画と見直し

当初は音声キャプチャ(MIC2/MDF1)もAI推論も両方NonSecureへ移す計画だったが、**音声キャプチャのNonSecure化はDMA転送が動かず断念**（初期化`s_audioOk=1`は成功するが、DMAが`s_audioBuf`に一切書き込まず全ゼロ。MDF1のPLL3クロックまたはGPDMAトリガーがTrustZone文脈で機能しない。GPDMA1_Ch0のチャネル属性をNSEC化しても解決せず）。

**見直し後の構成**: 音声キャプチャ(MDF1+PLL3+GPDMA、既にSecureで安定動作)はSecureに残し、`Comm_GetAudioBuffer` NSCゲートウェイでNonSecureが生の音声窓を取得。NonSecureがRMS/peak/waveformを計算してテレメトリに含める。AI推論(Phase D-2)も同じゲートウェイで音声を取得する予定。

### 実装内容

- `Secure/Core/Src/main.c`: 一度NSEC化したMDF1/MICピン/GPDMA1_Ch0を全てSecureに戻した
- `Secure/Core/Src/telemetry.cpp`: `initAudio()`(BSP_AUDIO_IN_Init/Record+reselectAudioPll3+DMA属性SEC設定)と`collect()`の音声RMS/peak/wave計算を復活
- `Secure_nsclib/secure_nsc.h` / `secure_nsc.c`: `Comm_GetAudioBuffer(int16_t* dst, uint32_t maxSamples)`ゲートウェイ追加（`cmse_check_address_range`で検証後、Secure audioBufをコピー）
- `NonSecure/Core/Src/ns_audio.c`: `Comm_GetAudioBuffer`でSecureから音声窓を取得→RMS/peak/wave計算に書き換え（BSPオーディオドライバ依存を除去）

### ビルド構造で判明した最重要事実（今後HAL/BSPソースを足す度に必ず踏む）

**NonSecure/Secureの`Drivers/STM32U5xx_HAL_Driver`は各HALソース`.c`を1つずつ`.project`の`<linkedResources>`に`<link>`で明示登録する構造**（CubeMXが`stm32u5xx_hal_conf.h`の有効モジュールから生成）。新HALモジュール(MDF等)を使うには①`hal_conf.h`で`#define HAL_XXX_MODULE_ENABLED` ②`.project`に`<link>`エントリ手動追加 ③`stm32cubeidec.exe ... -importAll "<repo>"`で再インポート、が必要。①だけだと`undefined reference to HAL_XXX_* / Unknown destination type (ARM/Thumb)`エラー。ヘッドレスビルド専用ワークスペースは`<scratchpad>/hws`でGUIワークスペースとは別物なので、GUIリフレッシュはhwsに反映されない。CLI完結には手動`.project`/`.cproject`編集+`-importAll`が正攻法。

### 実機検証結果

- 無音時: audio_rms=202, audio_peak=659（環境音、妥当なベースライン）
- 発音時: max_rms=6569（無音時の約30倍）, max_peak=32763（int16フルスケール近く）→ マイクが正しく音を拾い、NonSecureがゲートウェイ経由で取得・計算していることを実証
- 他センサー(温度37℃等)も並行して正常動作

### 次フェーズへの申し送り

- Phase D-2(AI推論のNonSecure化)は未着手。C++サポート追加とCubeAIライブラリ(`NetworkRuntime1020_CM33_GCC.a`)のNonSecureリンクが必要で、`.project`のlinked resources問題やC++ツール設定を考えると大掛かり。`Comm_GetAudioBuffer`ゲートウェイは既にあるのでAI側は音声取得済み

---

## Phase E: 完了・実機検証済み（2026-07-12）

### 実装内容

- `NonSecure/Core/Inc/app_config.h`（新規）: `CFG_IDLE_TIMEOUT_MS=3000`（テスト値）、`CFG_IDLE_LED_BLINK_MS=1000`、`CFG_ACTIVE_HB_MS=250`、`CFG_ACTIVE_TELEMETRY_MS=20`
- `NonSecure/Core/Src/main.c`: メインループをACTIVE/IDLEステートマシンに書き換え
  - **ACTIVE**: センサー読み+50Hzテレメトリ送信+緑PH7ハートビート点滅
  - **IDLE遷移**: `CFG_IDLE_TIMEOUT_MS`(3秒)無通信で `Sensors_Stop()`(ToF停止)+`Audio_Stop()`+`Comm_SetTelemetryEnabled(0)`+緑消灯、赤PH6を1秒間隔スロー点滅
  - **ACTIVE復帰**: `Comm_PollHostCommand()`が非ゼロ(=任意の受信バイト=ホスト活動)を返したら即 `Sensors_Resume()`+`Audio_Resume()`+`Comm_SetTelemetryEnabled(1)`
  - LED極性を統一(SET=消灯/RESET=点灯、両LED共通)、`led_show_version`を`led_green/red_off/toggle`に置換
- `pc_side/status_monitor/app.py`: 低消費電力対応
  - `_update_rate`(1Hz)で「常時ACTIVE維持」ON時に keep-alive バイト`\x00`を毎秒送信(ボードは任意の受信バイトを活動とみなすため、これでACTIVE維持)
  - トップバーに「常時ACTIVE維持」チェックボックス+「電源: ACTIVE/IDLE(低消費電力)」表示を追加(フレームレート>0でACTIVE、0でIDLE判定)

### 実機検証結果

- 起動後、無通信で約3秒→IDLE移行、赤LEDが1秒間隔でスロー点滅、テレメトリ停止(v2SOF=0)を確認
- IDLE中に1バイト送信→即ACTIVE復帰、テレメトリが55fpsで再開、全センサー(温度41.58℃)+音声(rms=360)復活を確認
- keep-alive相当(1秒ごとに1バイト送信)でACTIVE維持(50fps継続)を確認 → PC側app.pyの設計が実機で機能

### 設計上の重要点

- **ウェイク検知は純ポーリング**: Secure通信スタックはIDLE中もリスナー稼働(UART RX DMA/pollTcp/UART4はNonSecureモードに非依存)。バイト着信でSecure受信リング/nsActivityフラグが立ち、NonSecureの次の`Comm_PollHostCommand`が非ゼロを返す。割り込みをNonSecureへ入れる必要なし
- **PC側keep-aliveの`\x00`**: フレームSOF(0xAA)ではないので`FRAME_FEED_PLAIN`扱い→活動フラグは立つがコマンドとしては無害。nsCmdRingは16バイト循環で溢れても安全

Phase F（回帰確認）で既存バグを発見しました。 OTAの実ファーム更新（139KBのv15をUART経由で適用）で、ステージング（NOR書き込み+CRC検証）は成功するものの、FW_APPLY（NOR→Bank2コピー）後の検証でNACK(VERIFY)エラーになりBank2が消去状態のまま書き込まれませんでした。ボードはSWD直接書き込みで復旧済みです（v15正常動作、50Hzテレメトリ確認）。

このバグはTrustZoneリファクタリングとは無関係です。ota.cppのapplyToNonSecureは今回一切変更しておらず、Phase 2で動作確認したのは4〜5KBの小さいイメージでした。今回139KB（AI推論コードを含むため約30倍）で初めて露呈した、大きいイメージのBank2適用が失敗する既存バグです。原因候補はICACHE不整合、OCTOSPIメモリマップとNOR読み戻しの干渉、書き込みループの問題などです。

---

## バグ修正フェーズ（2026-07-13）：ユーザー報告の7項目対応

ユーザーから以下の課題が報告され、順に対応した：
(1)PhaseFの書き込みバグ (2)appでのストリーミング/録音不可 (3)低消費モードでもToFのLED起動 (4)プログラム内容と保存場所・メモリ管理の曖昧さ (5)UARTコマンドのmarkdown明示 (6)LED状態と実行状態の対応付け (7)プログラムファイルの肥大化

### (2) 音声ストリーミング/録音不可 — 修正・実機検証済み

**根本原因**: TrustZoneリファクタでNS駆動モード(`nsDriven=true`)になった結果、`'a'`/`'s'`(音声ストリーム開始/停止)コマンドが完全に処理されなくなっていた。`processRxByte()`のPLAIN分岐は、NS駆動時に受信バイトを`nsCmdPush()`でキューに積むだけで、旧来の`setAudioStream()`呼び出し経路に到達しなかった。TCP側(`pollTcp`)の`'a'`/`'s'`ハンドラも、`processRxByte`が`nsDriven`時に常に-1を返すため到達不能なデッドコードだった。app.pyは接続方式を問わず`'a'`/`'s'`を送るため、UART・TCP両方でストリーミング開始が機能していなかった。

**修正**: `telemetry.cpp`の`processRxByte()`のPLAIN分岐(NS駆動時)で、`nsCmdPush`の前に`'a'`/`'A'`→`setAudioStream(true)`、`'s'`/`'S'`→`setAudioStream(false)`を直接ハンドルするようにした。app.py側は`self.streaming`の初期化漏れ(getattr防御に依存)を明示初期化に修正。

**実機検証**: `'a'`送信後に音声フレーム(CMD_AUDIO)が98フレーム/3秒届き、`[TLM] audio streaming ON`ログを確認。`'s'`で停止を確認。

### (3) 低消費モードでもToFのLED起動 — 修正・実機検証済み

**根本原因**: `Sensors_Stop()`が`BSP_RANGING_SENSOR_Stop(0)`を呼ぶだけで、これはI2Cで測距を止めるコマンドに過ぎず、VL53L5CXモジュール自体は通電されたまま(モジュール上のアクティビティLEDも消えない)。VL53L5CXのLPn(PH1)ピンはBSPの`vl53l5cx_i2c_recover()`が初期化時に一度HIGHにするだけで、以降LOWにする処理がコードのどこにも無かった。

**修正**: `sensors.c`の`Sensors_Stop()`でLPn(PH1)を`GPIO_PIN_RESET`(LOW)にしてハードウェアシャットダウン。`Sensors_Resume()`でLPnをHIGHに戻し(LPn LOW→HIGHはXSHUT解除=センサー再起動なので)、`BSP_RANGING_SENSOR_Init`+プロファイル設定+Startのフル再初期化を実行(`tofInitAndStart()`ヘルパーに共通化)。

**実機検証**: SWDでGPIOH IDRを読み、ACTIVE中はPH1=HIGH(`0x72`)、3秒無通信でIDLE移行後はPH1=LOW(`0xB0`/`0xF0`交互=赤LED点滅)を確認。keep-aliveでACTIVE復帰後、150フレーム全てで`tof_ok=True`(LPn解除後の再初期化成功)を確認。

### (1) PhaseF書き込みバグ(OTA実ファーム更新失敗) — 原因3点を特定、2点修正・1点部分修正

大きいイメージのBank2適用が失敗する問題を、実機トレースで段階的に原因を3つ特定した：

**原因A: バックアップ処理が遅すぎてタイムアウト（修正済み・効果実証）**
`backupCurrentBank2()`が現行Bank2イメージをNORスロットBへ退避する際、消去ループが`kBackupMetaOffset`(0x1F0000≒1.94MB、31ブロック)まで全域を消していた。実際に書き込むのは`kNsFlashSize`(1MB、16ブロック)分だけなので、中間15ブロックの無駄な消去でPCツールの60秒×4リトライ(最大約8分)を使い切り、FW_APPLYが一度もACKを返せなかった。**修正: 消去ループ範囲を`kBackupMetaOffset`→`kNsFlashSize`に変更**。実機で**バックアップが約5.7秒で完了**することを確認(修正前は数分でタイムアウト)。

**原因B: ICACHE不整合（修正済み）**
Bank2消去+書き込み後のCRC検証(`crc16Update`で`kNsFlashBase`をポインタ読み)が、ICACHEにキャッシュされた消去前の古いライン(0xFF等)を読んで誤ったCRCを返しVERIFY失敗しうる。**修正: `applyToNonSecure()`と`restoreFromBackup()`の書き込みループ直後、CRC検証前に`HAL_ICACHE_Invalidate()`を追加**。

**原因C: Bank2消去中の割り込み起因HardFault（部分修正）**
NonSecureアプリ実行中にOTA FW_APPLYを受けると、Bank2消去でNonSecureの割り込みベクタ/ハンドラ(`SCB_NS->VTOR`はBank2を指す)が消え、消去〜書き込み間にSecure/NonSecure割り込みが発火すると消去済み領域を実行してHardFault。小さいイメージは書き込みが速く割り込みが発火する前に完了していたため露呈しなかった。**修正: `applyToNonSecure()`の消去直前に`__disable_irq()`、書き込み完了後に`__enable_irq()`(CRC検証・ACK送信はSysTick/UART割り込みが必要なため)。FW_APPLYハンドラのジャンプ直前に`HAL_ICACHE_Invalidate()`を追加(消去したてのBank2をICACHE経由で古い内容として実行するのを防ぐ)。**

**実機検証状況**:
- ✅ **1回目のホットOTA適用(v16稼働中→再適用→ジャンプ)は成功**: バックアップ5.7秒完了、Bank2書き込み・CRC検証・ジャンプ後にICSR=正常(Thread mode)、NSAP version=16を確認。書き込み内容はSWD読み戻しとMD5完全一致で、**書き込み自体は元々完璧だった**(問題は書き込み中/ジャンプ時のフォルトとタイムアウト)。
- ⚠️ **未解決: 連続2回目以降のホットOTA適用でジャンプ後HardFaultが残る**。v16が既に稼働している状態で立て続けにOTA→適用→ジャンプすると、2回目のジャンプ後にHardFault(ICSR VECTACTIVE=3)。アンダーリセット(`mode=UR -rst`)すれば正常起動に復帰する(Bank2イメージ自体は無傷)。通常のOTAフロー(Secureローダー状態→初回適用→初回ジャンプ)は成功するので実用上の主要ケースは動作するが、堅牢性のため**「OTA適用後は直接ジャンプせず`NVIC_SystemReset()`で再起動する」設計への変更を検討すべき**(業界標準のOTA適用パターン。どの状態からでもクリーンに起動できる)。

### (6) LED状態と実行状態の対応付け

→ **[UARTコマンド・LED状態リファレンス.md](UARTコマンド・LED状態リファレンス.md)** に独立ドキュメント化した（コマンド一覧・LED表・プロトコル仕様・SWDデバッグ手順）。

### (4) プログラム内容と保存場所・メモリ管理の曖昧さ

→ **[アーキテクチャ・メモリマップ.md](アーキテクチャ・メモリマップ.md)** に独立ドキュメント化した（Secure/NonSecureの分割理由、フラッシュ/RAM/NORのメモリマップ、ソースファイルの配置と役割、NSCゲートウェイ一覧、ペリフェラル割り当て、ビルド構造の注意点）。

---

## 追加バグ修正（2026-07-13、ユーザー報告「ACTIVE復帰後もToFがOFFのまま」から芋づる式に2件）

### バグ①: IDLE→ACTIVE復帰でToFが復帰しない（実機検証PASS）

**症状**: 復帰後 `tof_ok=True` なのに `tof_mm` が**1ミリも変化しない**（1674固定）。他のセンサー（照度など）は変動しているので、ToFだけ死んでいた。

**根本原因の連鎖**:
1. `Sensors_Resume()` が `BSP_RANGING_SENSOR_Init(0)` でフル再初期化を試みる
2. しかし `VL53L5CX_Init()` は **`IsInitialized` フラグが立っていると `VL53L5CX_ERROR` を返して何もしない**（ドライバの仕様）
3. → `s_tofOk = 0` になる
4. → `Sensors_Refresh()` の `if (s_tofOk && ...)` が偽になり、**ToF読み取り分岐ごとスキップ**
5. → 呼び出し側の `static FullStatus_t st` に**IDLE移行前の古い値が残り続ける** → 「動いているように見えて実は固定値」

**試した修正と、なぜダメだったか**:
- `Sensors_Stop()` で `BSP_RANGING_SENSOR_DeInit(0)` して `IsInitialized` をクリア → **ボードがUARTごと沈黙してIDLEから二度と戻らなくなった**。`BSP_RANGING_SENSOR_DeInit` → `BSP_I2C2_DeInit` → `HAL_GPIO_DeInit(PH4/PH5)` + I2C2クロック無効化 と、システムが立っているバス状態を崩しにいくため。

**採用した修正**: LPnピンのハードシャットダウンをやめ、**VL53L5CX自身の `SetPowerMode(SLEEP/WAKEUP)`** を使う。これは純粋なI2Cレジスタ操作なので、GPIO・クロック・ドライバ状態のどれも壊さない。
- `Sensors_Stop()`: `Stop()` + `SetPowerMode(SLEEP)` + `s_tofOk = 0`
- `Sensors_Resume()`: `SetPowerMode(WAKEUP)` + `Start()`（再Init不要）

**ついでに直した2つ**:
- `Sensors_Refresh()` で `s_tofOk == 0` のとき **明示的に `tof_ok=0, tof_mm=0` を書く**（古い値が残って「生きているように見える」のを防ぐ）
- 全センサーのスケジュールtickを `+= PERIOD` から **`= now + PERIOD` に変更**。IDLE中は `Refresh()` が呼ばれないので、累積方式だと期限が際限なく遅れ、復帰直後に毎ループ発火してセンサーを叩き続けていた。

**実機検証**: 復帰後の `tof_mm` が 1679→1680→1681→1683→1682 と変動することを確認（修正前は固定値）。

### バグ②: 一度IDLEに入ると二度とACTIVEに戻れない（実機検証PASS、最重要）

**症状**: IDLEに入るとUARTが完全に沈黙し、keep-aliveを何バイト送っても復帰しない。赤LEDは点滅し続ける（＝NonSecureのループ自体は回っている）。「IDLEに一度も入らずACTIVEを維持し続ける」なら150フレーム受信できるので、**IDLEに入った瞬間に受信経路が死ぬ**という切り分けができた。

**根本原因**: `Service::poll()` が `telemetryEnabled` に関係なく毎回 `pollTcp()` を呼んでいた。
- `pollTcp()` はTCPクライアント未接続時、1Hzで `MX_WIFI_Socket_accept()` を呼び、これが**数百ms〜10秒ブロックする**
- `pollTcp()` 内には「コンソールキーが来ていたらacceptより優先」というガードがあるが、これは `__HAL_UART_GET_FLAG(&huart1, UART_FLAG_RXNE)` を見ている。**コンソールRXはGPDMA循環DMA駆動なのでRXNEフラグは立たない**（DMAが即クリアする）→ ガードは機能していなかった
- **IDLE中はNonSecureループにセンサー処理がなく超高速で回る**ため、`poll()` がこのブロッキングacceptに突っ込み続け、ウェイクのための `Console_GetChar()` がドレインされない

**修正**: `poll()` で **`telemetryEnabled` が偽（＝IDLE中）なら `pollTcp()` をスキップ**する。IDLE中はTCPで配信すべきものが無いので実害はなく、コンソール/BLEのウェイク経路は生きたまま。

**実機検証**: keep-alive停止→IDLE移行→keep-alive再開で381フレーム受信、正常復帰を確認（app.pyの実transport層でも検証）。

---

### バグ③: 連続ホットOTA適用でジャンプ後HardFault（実機検証PASS）

**症状**: NonSecureアプリが稼働中に2回目のOTA適用をすると、ジャンプ直後にHardFault（ICSR VECTACTIVE=3）。1回目（Secureローダー状態から）は成功する。

**原因**: FW_APPLY成功後に `Secure_JumpToNonSecure()` で新イメージへ**直接ジャンプ**していた。クリーンなローダー状態からの初回ジャンプなら成立するが、NonSecureアプリが既に走っている状態では `SCB_NS->VTOR`・`MSP_NS`・NSが立ち上げたペリフェラル・ICACHEがいずれも**前のイメージの状態**を保持したままで、ジャンプ直後にフォルトする。

**修正**: **`NVIC_SystemReset()` で再起動する**。Stage-0が最初から走り直し、Bank2を再検証し、全ペリフェラルを初期化し直して、コールドブートと全く同じ経路で新イメージに入る。この種のMCUでOTAを締める標準的なやり方でもある。BootGuardのカウンタは適用直後に`ConfirmBoot`でクリアし、再起動後のStage-0が新イメージ用に新しい試行カウントを開始する。

**実機検証**: 同一イメージを**連続2回ホット適用**し、両方とも `Bank2 programmed and verified; NonSecure application launched` で成功。再起動後もテレメトリ206フレーム/4秒、ToF値が変動、全センサー正常。

---

## 未対応・残タスク

- **プログラムファイルの肥大化**: `telemetry.cpp` が1414行（通信3系統+OTA glue+MCU情報+音声が同居）。通信部を `comm_service.cpp` へ分割するリファクタが有効。Phase F（Secureデッドコード削除）と併せて実施予定。