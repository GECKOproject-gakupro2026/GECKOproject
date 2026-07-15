# 実装計画: BLE受信修復・BLE音声録音(SRAM+ADPCM)・音声圧縮

> ブランチ: `refactor/layering` / 作成: 2026-07-15
> **この文書は自己完結した作業指示書である。** 軽量AIモデルが1ステップずつ実行できるよう、
> 対象ファイル(絶対パス)・変更内容・ビルド/検証手順・コミットまで各ステップに含める。
> Phase 0 → 1 → 2 の順で、1ステップ=1コミット、実機検証PASS前に次へ進まない。

---

## Context（なぜこの変更をするか）

ユーザーの要求は3点:

1. **BLE経由でPC(app.py)がデータを受信できない** → 受信を通す。
2. **BLE音声のリアルタイム送信は困難**（BLE notify 64B/1Hz制約）。代わりに「録音開始→停止までの音声を**STM32内蔵の揮発メモリ(SRAM)**に保存し、停止を受け取ったらそのデータを一括送信する」機構を作る。
3. **音声送信プロトコルの圧縮**。現状は生PCM(int16, 16kHz mono, 無圧縮)。損失の少ない圧縮法を調査・実装する → **IMA ADPCM 4bit**（1/4圧縮、音声用途で実績大、軽量AIでも確実に実装可）を採用。

### 確定した設計方針（ユーザー判断）

- **録音保存先 = STM32内蔵SRAM（揮発メモリ）の NonSecure `.bss`**。NOR flashには触れない。
  - NOR flash(MX25LM51245G 64MB)は**OTAファーム記録専用の構造**（0x200000-0x3FFFFF=ステージング、0x400000-0x5FFFFF=ロールバック用バックアップ）。録音には使わない。
  - **配置: NonSecure RAM(0x20040000, 512KB, リンカスクリプトで確認済み)** に録音リングを置く。理由: (1) NS RAM 512K に 96KB を置いても余裕、(2) Secure RAM 256K を圧迫しない、(3) 生PCM取得は既存 `Comm_GetAudioBuffer` NSCゲートウェイで完結、audio_capture 自体は Secure のまま不変。
  - **ADPCM 4bit圧縮で 8KB/秒** なので、`64KB確保→約8秒`、`128KB確保→約16秒`。本計画は既定 **96KB確保→約12秒**（`REC_BUF_BYTES=98304`）。バッファ満杯で**自動停止（上書きしない有限録音）→送信フェーズへ**。
  - 揮発なので電源断で消える（録音→停止→即送信のワンショット用途で許容）。
- **圧縮 = IMA ADPCM 4bit**。BLE録音送信に必須適用。UART/TCPストリーミングへの適用は任意(Phase 2-7, opt-in・後方互換維持)。

### 調査で確定した最重要事実（設計の土台）

1. **PC→board の BLE write 経路が既にファーム側に存在する。** `Secure/Core/Src/comm_ble.cpp:202` の `stm32wb_at_BLE_EVT_WRITE_cb()` が P2P write characteristic **fe41** の受信を処理（現状は緑LEDトグルのみ）。→ 録音の開始/停止制御はこのコールバックを拡張して送れる。**基板のボタン操作は不要**。
2. PC側 `BleTransport.write()` は未実装（基底 `transports.py:43` が `return False`）。fe41 への `write_gatt_char` を追加する必要がある。
3. BLE notify=**fe42**(svc_index=1, char_index=2)、write=**fe41**(char_index=1)。notify 1回の上限は `val_tab[64]`、write は `val_tab[245]`（`Drivers/BSP/Components/stm32wb_at/stm32wb_at_ble.h:207`付近）。
4. `Frame_Encode`/`Frame_DecoderFeed`(frame_codec.h) は UART/TCP/BLE 共通。**ただし BLE notify 64B制約に frame_codec の 8B overhead を足すと録音チャンクが window に収まらない** → 録音チャンクは軽量生TLVにする（後述）。
5. 音声は Secure の `audio_capture`(16kHz/16bit mono、`audioBuf[2048]` リング)。`comm_service.cpp:664-691` が `g_AudioEvents` の half/full 検出で 512サンプルずつ処理済み → 録音Pumpもこの方式を流用。
6. NSCゲートウェイの**追加は許可**（既存7関数は凍結）。追加は `secure_nsc.h`/`secure_nsc.c`/`comm_backend.h`/`comm_service.cpp` の4ファイルで対に行う。
7. NSを1バイトでも変更したコミットは `NonSecure/Core/Src/main.c` の `NS_APP_VERSION`(現在24) を +1。

---

## Phase 0 — 課題1: BLE受信を通す（切り分け優先・土台）

原因が計画段階で断定できないため、**まず実機ログで4分岐に確定してから対処**する。BLEモジュール(WB5MMG)は間欠的初期化失敗があり、**各BLE検証の前に必ずUSB完全電源リセット**でHW事象を切り分ける。

### Step 0-1: 実機ログで原因を確定（観測のみ・変更なし）
- **観測ログ**: `[BLE-RAW] AT...`(`comm_ble.cpp:66`)、`[TLM] radio: BLE=OK|NG`(`comm_service.cpp` の radio 行)、BLE接続時 `[TLM] BLE central connected`(`comm_ble.cpp:197`)、app.py BLEスキャンの `Seen:` に出るデバイス名。
- **判定 → 進むStep**:
  - RAW応答が全く無い or `BLE=NG` → bleLinkOk不成立 → **0-2**
  - `BLE=OK` だが app スキャンに `P2PSRV1` が出ない → 名前不一致 → **0-3**
  - 接続でき `connected` は出るがフレームが来ない → 送信ゲート/char_index → **0-4**
- コミット: なし（記録のみ）。

### Step 0-2: bleLinkOk 成立の確実化（Initタイミング修正）
- **対象**: `Secure\Core\Src\comm_ble.cpp` の `Init()`(`comm_ble.cpp:60-89`)。
- **変更**: (1) 固定 `HAL_Delay(500)`(79行) を「`bleLinkOk` を50ms間隔で最大1500msポーリング、成立し次第抜ける」ループに置換。(2) タイムアウトしても `stm32wb_at_client_Set(BLE_SVC,&svc)` を1回はフォールバック実行しアドバタイズ開始。(3) `printf("[BLE] Init linkOk=%d after %lums\r\n",...)` 追加。
- **ビルド**: CubeIDEヘッドレス Secure Debug（NS不変→NS_APP_VERSION据え置き）。
- **検証(PASS)**: 書込み後、連続リセット**10回中10回** `linkOk=1` かつ `BLE=OK`。
- コミット: `fix(ble): poll bleLinkOk with timeout to reliably start advertising`

### Step 0-3: 探索名の一致
- **対象**: `pc_side\status_monitor\app.py`(`app.py:339` 付近のBLEターゲット候補/既定値)。
- **変更**: 0-1で観測した実アドバタイズ名を候補先頭に追加し既定化（BleTransportは部分一致検索なので部分文字列で可）。
- **検証(PASS)**: BLE接続で app に `BLE connected, subscribing to notifications` が出る。
- コミット: `fix(pc): match BLE scan name to module advertised name`

### Step 0-4: 送信ゲート/char_index の確定
- **対象**: `Secure\Core\Src\comm_ble.cpp` の `SendStatus()`(`comm_ble.cpp:95-135`)。
- **変更**: `stm32wb_at_client_Set(BLE_NOTIF_VAL,&notif)` の戻り値 rc をログ(`printf("[BLE] notif rc=%d len=%u\r\n",...)`)。**まず char_index=2 のまま rc を観測**。rc≠0 が続く場合のみ char_index を 2→1 に変えA/B比較。
- **検証(PASS)**: `notif rc=0` が1Hz継続、app 受信レート **≥1 fps**、MINI(39B)がダッシュボードに反映。
- コミット: `fix(ble): verify notify char mapping and log SendStatus result`

### Step 0-5: PC側 MINI 受信サニティ（通常変更不要）
- `python -c "import protocol; print(protocol.MINI_SIZE_V2)"` → **39** を確認（firmware `sizeof(MiniStatus)` と一致）。不一致時のみ `protocol.py` を修正。

**Phase 0 完了ゲート**: BLE経由で app が **≥1fps** 受信しダッシュボードが更新。

---

## Phase 1 — 課題3: IMA ADPCM コーデック基盤

**方式**: ブロック方式 IMA ADPCM。各ブロック先頭に `[predictor int16 LE][step_index u8][pad u8]` の4Bヘッダ → ブロック独立デコードでパケットロス耐性。純粋関数（状態は引数で持ち回り、グローバル禁止＝再入可能）。Phase 0 と独立着手可（純粋関数で単独検証できる）。

### Step 1-1: C側 ADPCM コーデック（新規）
- **新規**: `Secure\Core\Inc\adpcm.h`, `Secure\Core\Src\adpcm.c`。
- **API**:
  - `typedef struct { int16_t predictor; int8_t step_index; } adpcm_state_t;`
  - `size_t adpcm_encode(adpcm_state_t*, const int16_t* pcm, size_t count, uint8_t* out);`（2サンプル→1バイト）
  - `size_t adpcm_decode(adpcm_state_t*, const uint8_t* in, size_t nbytes, int16_t* pcm);`
  - 標準 IMA step table(89要素)/index table(16要素) を内部 static const で持つ。
- **重要（両プロジェクトでビルド）**: 録音エンコードは NonSecure 側で行うため、`adpcm.c`/`adpcm.h` を **NonSecure プロジェクトのコンパイル対象にも追加**する（同一純粋Cソースを Secure/NonSecure 両方でビルド。TrustZone 境界越えの呼び出しは無し）。Secure 側は Phase 2-7(任意)の UART/TCP ADPCM ストリーミングで使う。
- **ビルド**: Secure Debug + NonSecure Debug（`Core/Src` 配下は自動収集）。**検証(PASS)**: 両ビルド0 error。
- コミット: `feat(audio): add IMA ADPCM block codec (C, built in both S/NS)`

### Step 1-2: Python側 ADPCM デコーダ（新規・Cとビット互換）
- **新規**: `pc_side\status_monitor\adpcm.py`。`decode_block(block: bytes) -> list[int]`（4Bヘッダ+nibble列を復号）。C と同一 step/index table。
- **検証**: `python -c "import adpcm"` 成功。
- コミット: `feat(pc): add IMA ADPCM decoder (Python)`

### Step 1-3: 往復自己テスト
- **新規**: `pc_side\status_monitor\test_adpcm.py`。1kHz正弦(16kHz, 4096サンプル)で（Python encode相当 or C出力を read して）decode し SNR と圧縮率を測定。
- **検証(PASS)**: 往復 **SNR ≥ 20 dB**、圧縮率(出力/入力) **≤ 0.27**。
- コミット: `test(audio): ADPCM round-trip SNR/compression check`

**Phase 1 完了ゲート**: Cビルド0 error、往復 SNR≥20dB・圧縮率≤0.27。

---

## Phase 2 — 課題2: BLE音声録音機構（内蔵SRAM保存→一括送信）

**制御・データ経路（確定）**:
```
[録音制御 PC→board]
app.py[開始/停止] → BleTransport.write(frame) → GATT write fe41(val_tab[245])
  → Secure stm32wb_at_BLE_EVT_WRITE_cb(comm_ble.cpp:202) が Frame_DecoderFeed で REC_START/STOP 抽出
  → Secure内フラグ bleRecCmd に保持 → NS が Comm_PollRecCmd() で引き取り → NS Rec_Start/Stop

[録音データ board→PC]（NS録音バッファ → Secure BLE notify → PC）
NS録音: Comm_GetAudioBuffer(既存) で生PCM窓 → NS adpcm_encode → NS .bss 録音リング(96KB)へ追記(満杯で自動停止)
NS送信: リングを 生TLV[0x0B][seq u16][adpcm ~58B] に刻み Comm_BleSendRaw() で Secure→notify(fe42)
        末尾 [0x0C][total_samples u32][crc16](REC_END)
PC: notify先頭が 0x0B/0x0C なら録音経路(adpcm.decode 連結→WAV保存)、他は従来 FrameParser
```

### Step 2-1: 新規コマンドID
- **対象**: `Secure\Core\Inc\frame_codec.h`(`:37`の FW_APPLY 直後), `pc_side\status_monitor\protocol.py`(`:26` 付近)。予約 0x7E/0x7F は不変。
- **追加**: `FRAME_CMD_REC_START=0x09`, `REC_STOP=0x0A`, `REC_CHUNK=0x0B`, `REC_END=0x0C`（Python側も同値）。
- **検証**: Secureビルド0 error、`python -c "import protocol"` 成功。
- コミット: `feat(proto): add recording command IDs`

### Step 2-2: NS録音バッファ + 状態機械（内蔵SRAM `.bss` + ADPCM）
- **目的**: NS 側に固定録音リングを確保、生PCM窓をADPCM圧縮して追記、満杯で自動停止。
- **新規**: `NonSecure\Core\Inc\recorder.h`, `NonSecure\Core\Src\recorder.c`。
- **骨格（recorder.h）**:
```c
#ifndef RECORDER_H
#define RECORDER_H
#include <stdint.h>
/* 内蔵SRAM録音: NS .bss に固定確保。ADPCM 4bit=8KB/s。
   REC_BUF_BYTES=98304(96KB)→約12秒。満杯で自動停止(上書きしない)。 */
#define REC_BUF_BYTES  98304U   /* 96KB≈12s。64KB=8s/128KB=16sも可 */
void     Rec_Start(void);   /* adpcm_state リセット, wpos=0, active=1 */
void     Rec_Stop(void);    /* active=0 */
int      Rec_Active(void);
void     Rec_Pump(void);    /* Comm_GetAudioBuffer→adpcm_encode→リング追記, 満杯でRec_Stop */
uint32_t Rec_UsedBytes(void);
uint32_t Rec_TotalSamples(void);
uint32_t Rec_Read(uint32_t off, uint8_t *dst, uint32_t len);
#endif
```
- **recorder.c 実装**: `static uint8_t recBuf[REC_BUF_BYTES];`（NS `.bss`）。`Rec_Pump()` は 512サンプル分の生PCMを `Comm_GetAudioBuffer(tmp,512)` で取得 → `adpcm_encode(&st,tmp,512,out)` → `recBuf` 末尾追記。`wpos+need > REC_BUF_BYTES` なら `Rec_Stop()`。総サンプル数を別カウンタで保持。
- **NS main改変**: `NonSecure\Core\Src\app_loop.c` の `App_Run()` ループ（`Comm_Poll()` 直後）に `if (Rec_Active()) Rec_Pump();` を追加、`#include "recorder.h"`。
- **NS変更あり → `NS_APP_VERSION`(`main.c`) を +1**。
- **ビルド**: NS Debug ビルド。
- **検証(PASS)**: Start→3秒→Stop で `Rec_TotalSamples()`≈48000(±10%)、`Rec_UsedBytes()`≈samples/4。満杯テスト: 長時間放置で約12秒で自動停止。VCP `[REC] stopped samples=NNNNN used=MMMM`。
- コミット: `feat(rec): NS SRAM recording ring + ADPCM (96KB ~12s, auto-stop on full)`

### Step 2-3: Secure→NS 録音制御フラグ（BLE write 受信の受け皿）
- **対象**: `Secure\Core\Src\comm_ble.cpp`（+ `comm_ble.hpp`）。
- **変更**: 無名namespaceに `volatile uint8_t bleRecCmd=0; /*0=none,1=start,2=stop*/` と `Frame_Decoder bleWriteDecoder;`。`stm32wb_at_BLE_EVT_WRITE_cb`(`:202`) で `val_tab` を `Frame_DecoderFeed`、`REC_START`→`bleRecCmd=1`、`REC_STOP`→`bleRecCmd=2`（既存LEDトグルは残す）。`comm_ble.hpp` に `uint8_t TakeRecCmd();`（読んで0クリア）を宣言/実装。
- **検証(PASS)**: PCから fe41 に REC_START/STOP write → VCP `[BLE] recCmd=1|2`。
- コミット: `feat(ble): parse REC_START/STOP on GATT write, expose TakeRecCmd()`

### Step 2-4: NSCゲートウェイ追加（制御取得 + 送信橋渡し）※既存7関数凍結
- **対象**（対で追加）: `Secure_nsclib\secure_nsc.h`, `Secure\Core\Src\secure_nsc.c`（CMSE_NS_ENTRY + cmse検証）, `Secure\Core\Inc\comm_backend.h`, `Secure\Core\Src\comm_service.cpp`（CommBridge_*）。
- **追加関数**:
  - `uint8_t Comm_PollRecCmd(void)` → `CommBridge_PollRecCmd` → `comm_ble::TakeRecCmd()`（0/1/2）。
  - `int Comm_BleSendRaw(const uint8_t *data, uint32_t len)` → `CommBridge_BleSendRaw`。**len≤64検証 + `cmse_check_address_range(...,CMSE_NONSECURE|CMSE_MPU_READ)` + Secureローカルコピー**後、`stm32wb_at_BLE_NOTIF_VAL_t` に詰め `stm32wb_at_client_Set(BLE_NOTIF_VAL,&notif)`(svc=1,char=2)。戻り 0=送信/-1=不正/-2=BLE未接続。
- **注記**: `Comm_BleSendRaw` は frame_codec を通さない生TLV送信専用（notify 64B制約対応）。既存 `SendStatus` の notify 低レベルAPIを使う。
- **検証**: ビルド0 error、CMSEエントリがマップ出現。
- コミット: `feat(nsc): add Comm_PollRecCmd + Comm_BleSendRaw gateways (existing 7 frozen)`

### Step 2-5: NS 録音制御ループ結線 + 送信
- **対象**: `NonSecure\Core\Src\app_loop.c`（送信ヘルパーは recorder.c に置いても可）。
- **変更**:
  1. `App_Run()` ループで毎回 `uint8_t rc=Comm_PollRecCmd(); if(rc==1)Rec_Start(); else if(rc==2){Rec_Stop(); rec_send_pending=1;}`。
  2. `rec_send_pending` 中、1ループで数チャンクだけ送る（BLE律速なので ~200ms/notify のペーシング）: `Rec_Read(off,buf+3,58)` → `buf[0]=0x0B; buf[1..2]=seq` → `Comm_BleSendRaw(buf,61)`。全送信後 `[0x0C][total_samples u32][crc16]` を1回送り `rec_send_pending=0`。
  3. UART/TCP 制御が要る場合は既存 `Comm_PollHostCommand`('a'/'s') 経路にREC追加可（BLE完結の本計画では任意）。
- **NS変更あり → `NS_APP_VERSION` を再度 +1**（Step 2-2 と別コミットなら都度 +1）。
- **ビルド**: NS Debug ビルド。
- **検証(PASS)**: BLEで録音開始→3秒→停止 → REC_CHUNK 連続到着、REC_END total_samples≈48000(±10%)。転送時間≈used/(58B×5Hz)。
- コミット: `feat(rec): wire BLE rec control + stream ADPCM ring over notify`

### Step 2-6: PC側 BLE write + 録音UI + 受信/WAV化
- **対象**: `pc_side\status_monitor\transports.py`, `protocol.py`, `app.py`。
- **変更**:
  1. `transports.py` `BleTransport`: write char UUID `0000fe41-8e22-4541-9d4c-21edae82ed19` 保持。`write()` はスレッドセーフキューに積み、`session()` async ループで `await client.write_gatt_char(WRITE_UUID,data)` を drain（bleak client は `_run` 内のため直接呼べない）。
  2. `app._poll_queue`(`app.py:491-522`): BLEの生バイト先頭が 0x0B→録音チャンク(seq+adpcm)蓄積、0x0C→REC_END で `adpcm.decode` 連結→`record_samples`→既存WAV書き出し(`app.py:464-477`)。他は従来 FrameParser。
  3. `_toggle_record`(`app.py:452`): BLE時は `write(protocol.build_frame(CMD_REC_START,0))`/STOP に。UART時の既存ストリーム録音('a'/'s')は維持。
- **検証(PASS)**: BLE録音開始→3秒→停止→`recordings/rec_*.wav` 生成、再生で音声、WAVサンプル数=REC_END total_samples 一致。
- コミット: `feat(pc): BLE GATT write + SRAM recording control + ADPCM WAV save`

### Step 2-7（任意）: UART/TCP ストリームにも ADPCM 適用（opt-in・後方互換）
- **方針**: 既存 `CMD_AUDIO=0x03` は**生PCMのまま残す**（`decode_audio` 互換維持）。新規 `FRAME_CMD_AUDIO_ADPCM=0x0D` を opt-in 追加。
- **対象**: `frame_codec.h`, `comm_service.cpp`(`:664-691`), `protocol.py`(`decode_audio_adpcm` 新設), `app.py`。
- **検証(PASS)**: 0x0D選択時、帯域が約1/4に低下し波形が目視一致（SNR≥20dB相当）。
- コミット: `feat(audio): optional ADPCM streaming (0x0D), PCM kept for compat`

**Phase 2 完了ゲート**: BLE録音→SRAM→一括送信→WAV保存が往復成功、サンプル数一致。

---

## ガードレール（全Step共通）

1. `FullStatus_t` 165B / `MiniStatus` 39B / frame_codec の SOF(0xAA)/EOF(0x55)/CRC16-CCITT は**不変**（static_assert維持）。
2. 既存 `Comm_*` 7ゲートウェイと `CommBridge_*` は**シグネチャ凍結**（新規追加のみ可: `Comm_PollRecCmd`, `Comm_BleSendRaw`）。NS→Sの関数ポインタ導入禁止。
3. OTA信頼ルートに触れない: `handleFrame`/`ota::Manager`/NOR定数 0x200000–0x5FFFFF は不可触。**録音は内蔵SRAM(NS `.bss`)のみ**（`BSP_OSPI_NOR_*`/NOR/OSPI を一切使わない）。
4. 機械的変更のみ: タイムアウト値/NVIC優先度/DMA属性/printf文字列/比較演算子を勝手に変えない。
5. NSを触ったコミットは `NonSecure/Core/Src/main.c` の `NS_APP_VERSION` を +1（Step 2-2, 2-5 で都度）。
6. 検証は必ず **Debug構成**・CubeIDEヘッドレスビルド・`STM32_Programmer_CLI`(SWD)書込み。
7. **各BLE検証の前に必ずUSB完全電源リセット**（WB5MMG間欠初期化不安定をHW事象として切り分け）。

## 軽量AIが踏みやすい罠（明示）

- **NOR不使用**: Phase 2 で `BSP_OSPI_NOR_*`/ota定数を絶対に呼ばない（録音は内蔵SRAMのみ）。
- **録音バッファ配置**: NonSecure `.bss`（NS RAM 512K）。Secure RAM(256K)は圧迫しない。audio_capture は Secure 不変（生PCMは既存 `Comm_GetAudioBuffer` で取得）。
- **BLE notify 64B制約**: 録音チャンクを frame_codec に通すと 8B overhead で溢れる → REC_CHUNK/END は**生TLV**、1回≤61B。UART/TCP と PC側で経路分岐が必須(Step 2-5/2-6)。
- **NS→Secure 送信の安全ルール**: `Comm_BleSendRaw` は `cmse_check_address_range` 検証 + Secureローカルコピー必須（既存ゲートウェイと同じ）。
- **char_index の個体依存**: fe42→char_index=2 はモジュール定義依存。まず 2 のまま rc を観測してからA/B(Step 0-4)。
- **BLE write のスレッド越え**: bleak client は asyncio ループ内 → `write()` はキュー経由でループに渡す(Step 2-6)。
- **満杯自動停止**: 上書きしない有限録音。満杯で `Rec_Stop()`→送信フェーズ。
- **録音Pumpと音声ストリームの競合**: 両方ONだと `Comm_GetAudioBuffer` の窓を取り合う。録音中はUART/TCPストリーミングを排他推奨(app側ガード or NS側優先)。
- **SRAM確保失敗**: `REC_BUF_BYTES=96KB` が NS `.bss` overflow するならリンカマップを見て 64KB に縮小（録音長は約8秒に減る）。

## Critical Files

- `NonSecure\Core\Src\app_loop.c`（録音Pump/制御ループ結線、新規 recorder.c/.h の呼び出し元）
- `Secure\Core\Src\comm_ble.cpp`（BLE Init/送信/write受信・notify生TLV送信）
- `Secure\Core\Src\secure_nsc.c`（新規 Comm_PollRecCmd / Comm_BleSendRaw ゲートウェイ）
- `pc_side\status_monitor\transports.py`, `app.py`, `protocol.py`（PC側BLE/録音/デコード）
- 新規: `Secure\Core\{Inc,Src}\adpcm.{h,c}`(S/NS両ビルド), `NonSecure\Core\{Inc,Src}\recorder.{h,c}`, `pc_side\status_monitor\adpcm.py`
- 参照: `NonSecure\Core\Src\main.c`(NS_APP_VERSION), `Secure\Core\Inc\frame_codec.h`, `Secure\Core\Src\comm_service.cpp`(CommBridge_*), `Secure_nsclib\secure_nsc.h`, `NonSecure\STM32U585AIIXQ_FLASH.ld`(RAM 512K)
