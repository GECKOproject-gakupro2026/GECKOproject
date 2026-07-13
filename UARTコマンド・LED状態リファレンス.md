# UARTコマンド・LED状態リファレンス

B-U585I-IOT02A エッジAIシステムの、ホスト↔ボード間コマンドとLED表示の対応表。

**接続**: ST-LINK VCP (COM9) / **921600 baud** / 8N1
（Wi-Fi TCP、BLEでも同じフレームプロトコルが使える。文字コマンドはUART/TCP共通）

---

## 1. 動作モードとLED表示

ボードは3つの状態を取る。両LEDとも **SET=消灯 / RESET=点灯**（LD6赤=PH6、LD7緑=PH7）。

| 状態 | 緑LED (PH7) | 赤LED (PH6) | ToF | テレメトリ | 何が動いているか |
|---|---|---|---|---|---|
| **ACTIVE** | 250ms周期で点滅（ハートビート） | 消灯 | 測距中 | 50Hz送信 | NonSecureアプリ（センサー読み+送信） |
| **IDLE**（低消費電力） | 消灯 | **1秒周期でゆっくり点滅** | SLEEP（停止） | 停止 | NonSecureアプリ（ウェイク待ちのみ） |
| **Secure OTAローダー** | Secure側が制御 | - | - | Secureが送信（センサー値=0） | Bank2に有効イメージがない、またはUSERボタン押下起動 |

**ACTIVE ⇄ IDLE の遷移**
- **ACTIVE → IDLE**: ホストから**3秒間**（`CFG_IDLE_TIMEOUT_MS`）何も受信しない
- **IDLE → ACTIVE**: ホストから**任意の1バイト**を受信（フレームでも生バイトでもよい）

**PCアプリ（`pc_side/status_monitor/app.py`）の「常時ACTIVE維持」**をONにすると、1秒ごとに`\x00`（NUL）を送ってACTIVEを維持する。OFFにすると3秒後にIDLEへ落ちる。

---

## 2. 文字コマンド（1文字送るだけ）

送信例: `python -c "import serial; serial.Serial('COM9',921600).write(b'a')"`

**重要**: 有効なコマンドは**ボードの状態によって変わる**。

### NonSecureアプリ稼働中（通常運用、ACTIVE/IDLEどちらでも）

| キー | 動作 |
|---|---|
| `a` / `A` | **音声ストリーミング開始**（16kHz/16bit/モノラルのPCMをCMD 0x03フレームで送出） |
| `s` / `S` | **音声ストリーミング停止** |
| その他の任意バイト | コマンドとしては無視。ただし**ホスト活動として扱われIDLEから復帰する**（`\x00`をkeep-aliveに使うのはこのため） |

`t`/`b`/`i`/`I` はこのモードでは**効かない**（NonSecureのコマンドキューに積まれるだけで、Secure側のハンドラは実行されない）。

### Secure OTAローダー稼働中（Bank2が空、またはUSERボタン押しながら起動）

| キー | 動作 |
|---|---|
| `t` / `T` | ハードウェアテストスイート実行（全16テスト。テレメトリは一時停止） |
| `a` / `A` | 音声ストリーミング開始 |
| `s` / `S` | 音声ストリーミング停止 |
| `b` / `B` | BLE ATブリッジモード |
| `i` | AI推論を1回実行 |
| `I` | AI推論の自動実行をトグル（2秒周期） |

### TCP接続時のみ（Wi-Fi、Secure OTAローダー稼働中）

| キー | 動作 |
|---|---|
| `p` / `P` | PONG応答（`[TCP] PONG uptime=...`） |
| `l` / `L` | 緑LEDトグル |

---

## 3. フレームプロトコル（要件定義 11.2節）

すべてのテレメトリとOTAはこのフレーム形式でやり取りする。

```
SOF(0xAA) | CMD(1) | SEQ(1) | LEN(2, LE) | PAYLOAD(LEN) | CRC16(2, LE) | EOF(0x55)
```
CRC-16/CCITT-FALSE を SOF〜PAYLOAD にかける。

### コマンド一覧

| CMD | 方向 | 名前 | ペイロード |
|---|---|---|---|
| `0x01` | board→PC | STATUS | 全ステータス 165バイト（v2、`FullStatus`） |
| `0x02` | board→PC | STATUS_MINI | 圧縮ステータス 39バイト（BLE通知用） |
| `0x03` | board→PC | AUDIO | PCM 512サンプル × int16（LE）= 1024バイト |
| `0x04` | PC→board | FW_CHUNK | offset(u32 LE) + データ（最大1008バイト） |
| `0x05` | PC→board | FW_COMPLETE | size(u32 LE) + crc16(u16 LE) → NORのステージングを検証 |
| `0x06` | PC→board | STATUS_REQ | なし（OTA状態を問い合わせ） |
| `0x07` | board→PC | STATUS_RESP | `<BIIHB`: state/received/expected/crc16/last_error |
| `0x08` | PC→board | FW_APPLY | なし（NOR→Bank2にコピーし、**システムリセットで新イメージを起動**） |
| `0x7E` | board→PC | ACK | `<BBI`: orig_cmd/orig_seq/arg |
| `0x7F` | board→PC | NACK | `<BBB`: orig_cmd/orig_seq/error |

### NACKエラーコード

| 値 | 意味 |
|---|---|
| 1 | BAD_OFFSET（オフセット不正） |
| 2 | ERASE（消去失敗） |
| 3 | WRITE（書き込み失敗） |
| 4 | VERIFY（書き込み後のCRC不一致） |
| 5 | TOO_LARGE（イメージが大きすぎる） |
| 6 | BAD_STATE（ステージング未完了などで適用不可） |

### OTA状態（STATUS_RESPの`state`）

| 値 | 意味 |
|---|---|
| 0 | Idle |
| 1 | Receiving（チャンク受信中） |
| 2 | Staged（NORに格納済み・CRC検証済み） |
| 3 | Error |

---

## 4. PCツール

すべて `pc_side/status_monitor/` にある。

```bash
# GUIモニタ（ダッシュボード/全データ/オーディオ録音再生の3タブ）
python app.py

# OTAファームウェア更新（ステージング + Bank2適用 + 自動起動）
python ota_update.py <firmware.bin> --port COM9 --apply
python ota_update.py <firmware.bin> --tcp 192.168.137.2:5000 --apply

# OTA状態の問い合わせのみ
python ota_update.py --status --port COM9
```

**注意**: ボードがIDLEだとテレメトリが止まっているので、状態を見る前に1バイト送ってACTIVEに戻すこと。

---

## 5. デバッグ（SWD経由）

```bash
CLI="<CubeIDE>/plugins/.../tools/bin/STM32_Programmer_CLI.exe"

# 書き込み
$CLI -c port=SWD mode=UR -d <elf> -v

# リセット（通常）
$CLI -c port=SWD mode=UR -rst

# NonSecureのバージョン確認（NSAP magicの次のワード）
$CLI -c port=SWD mode=HOTPLUG -r32 0x08100404 1

# コアのフォルト状態（ICSR の VECTACTIVE = 下位9bit）
#   0 = 正常（Thread mode） / 3 = HardFault
$CLI -c port=SWD mode=HOTPLUG -r32 0xE000ED04 1

# LED / ToF LPnピンの状態（GPIOH IDR）
#   bit7=PH7(緑) bit6=PH6(赤) bit1=PH1(ToF LPn)、値0=LOW
$CLI -c port=SWD mode=HOTPLUG -r32 0x42021C10 1
```

**GPIOH IDRの読み方の例**
- `0x72` = 緑消灯・赤消灯・LPn HIGH → ACTIVE
- `0xB0` / `0xF0` が交互 → 赤が点滅 = IDLE

---

## 6. ハマりどころ

- **IDLE中はUARTが完全に無音**（テレメトリ停止）。「応答がない」と判断する前に、まず1バイト送ってACTIVEに戻す。
- **音声ストリーミングの`a`/`s`は、NonSecureアプリ稼働中とOTAローダー中で通る経路が違う**（前者は`processRxByte`のPLAIN分岐、後者は`App_Main`のディスパッチ）。片方だけ直しても両方は動かない。
- **OTA適用（FW_APPLY）は、Bank2に既存イメージがあると先にNORへバックアップを取る**ため数秒かかる（約5.7秒）。Bank2が空なら1.4秒程度。
- **FW_APPLY成功後、ボードはシステムリセットして新イメージを起動する**（直接ジャンプではない）。ACKが返ってから再起動＋Wi-Fi/BLE初期化で**十数秒**かかるので、続けてコマンドを送る前に待つこと。
- ボードのバージョンはNonSecureの`NS_APP_VERSION`（`NonSecure/Core/Src/main.c`）で、SWDでは`0x08100404`から読める。
