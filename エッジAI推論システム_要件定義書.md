# 組み込みエッジAI推論システム 要件定義書・開発計画

> **履歴資料（評価ボード段階）**
> 本文に残るSTM32U575QGI6、ESP32-H2、USB CDC等の将来構成は採用しない。
> 自作基板の現行要件と実装順は
> [`docs/自作基板_統合実装計画.md`](docs/自作基板_統合実装計画.md) を正とする。

作成日: 2026-07-03（改訂版）

## 改訂方針（この版で確定した事項）
- 通信は **Bluetooth（BLE）のみ** を使用する。Wi-Fiは使用しない。
- **セキュリティ保護（暗号化・署名検証等）は行わない。** 通信の完全性担保としてCRC等の誤り検出のみ実施する。
- 学習済みモデルのC++（Cコード）化は、**PC上でアプリケーションを手動操作せず、Pythonプログラムから自動実行**する。

---

## 1. プロジェクト概要

### 1.1 目的
STM32マイコン上でセンサーデータを取得し、PC側で学習した機械学習モデルをコンパイルしてBluetooth経由でSTM32へ転送し、エッジ側（STM32）でリアルタイムに推論・解析・判断を行うシステムを構築する。

### 1.2 開発方針（2段階アプローチ）

| フェーズ | 内容 |
|---|---|
| Phase 0: 評価 | 市販評価ボード B-U585I-IOT02A で全体アーキテクチャ・BLE通信・MLパイプラインを検証 |
| Phase 1: 実装 | 自作基板（STM32U575QGI6 + センサーモジュール + ESP32-H2-MINI）へ移植 |

評価ボードと自作基板でMCUが異なる（STM32U585 → STM32U575）ため、ペリフェラル差分の吸収が設計上の論点になりうるが、本プロジェクトではセキュリティペリフェラル（AES/PKA/OTFDEC）の差分は対象外のため、実質的な影響はない。

---

## 2. システム全体アーキテクチャ

```
[エッジ側: STM32]                              [PC側]
センサーデータ取得 (I2C/SPI)                    機械学習（Python）
        │                                         ↑│
        ▼                                         ││
  データ管理 ──(揮発→不揮発)                 Cコード生成（stm32ai CLI, Python経由）
   揮発 → 不揮発                                   │
        │↕                                         │
      通信 ⇄──────────── Bluetooth (BLE) ───────⇄  通信
        │↕
   ファームウェア（USB経由でPC接続、独自コマンドで更新）
```

**データフロー:**
1. センサーデータ取得（I2C/SPI）→ 揮発メモリ（SRAM）でバッファリング → 必要に応じ不揮発メモリに記録
2. 通信モジュール（ESP32-H2-MINI, BLE）経由でPCへセンサーデータを送信
3. PC側Pythonプログラムで機械学習モデルを学習
4. 同じPythonプログラム内で `stm32ai` CLIを呼び出し、モデルをCコードへ自動変換
5. 生成物（またはそこから抽出した重みバイナリ）をBLE経由でSTM32へ送信し、不揮発メモリに書き込み
6. STM32がファームウェア／推論エンジンとして実行

---

## 3. ハードウェア要件

### 3.1 評価環境: B-U585I-IOT02A（確定仕様）

| 項目 | 仕様 |
|---|---|
| MCU | STM32U585AII6Q（Arm Cortex-M33、TrustZone、Armv8-M） |
| Flash / SRAM | 2 MB Flash / 786 KB SRAM、UFBGA169パッケージ |
| 外部メモリ | 512Mbit QSPI Flash、64Mbit OctoSPI PSRAM、256Kbit I2C EEPROM |
| 無線通信 | Bluetooth Low Energy（ST製）※Wi-Fiモジュールも搭載されているが本プロジェクトでは未使用 |
| USB | USB FS（Sink/Source、最大2.5W） |
| 搭載センサー | マイク×2（デジタル）、湿温度センサー、3軸磁気センサー、3D加速度・ジャイロ、気圧センサー、ToF/ジェスチャーセンサー、照度センサー |
| デバッグ | オンボードSTLINK-V3E |

### 3.2 自作基板構成（計画）

| 部品 | 型番 | 役割 |
|---|---|---|
| コアMCU | STM32U575QGI6 | センサー取得・データ管理・推論実行 |
| 通信モジュール | ESP32-H2-MINI-1 | STM32とPC間のBLE通信仲介（Bluetooth Low Energy 5 + IEEE 802.15.4対応。Wi-Fi非搭載だが本プロジェクトでは使用しないため問題なし） |
| 不揮発メモリ | AT25QF128A（NORフラッシュ, OCTOSPI Quad I/O） | モデル・ファームウェアの永続化 |

### 3.3 対象外とする事項
- Wi-Fi通信（3.1のB-U585I-IOT02Aが備えるWi-Fiモジュールも本プロジェクトでは使用しない）
- 暗号化・署名検証などのセキュリティ機能（STM32U575/U585のAES/PKA/OTFDEC搭載有無は考慮不要）

---

## 4. 通信要件定義（Bluetooth一本化）

### 4.1 STM32 ⇄ 通信モジュール間（USB、独自コマンドプロトコル）

| 設計項目 | 内容 |
|---|---|
| フレームフォーマット | 開始バイト＋コマンドID＋長さ＋ペイロード＋CRC（誤り検出）＋終端バイト |
| コマンド体系 | センサーデータ送信、モデル転送（分割転送）、ファームウェア更新、ステータス問い合わせ、ACK/NACK |
| フロー制御 | タイムアウト・リトライ回数・再送要求 |
| 転送単位 | USB FSバルク転送でのペイロードサイズ上限に応じた分割転送設計 |
| バージョニング | コマンドプロトコル自体のバージョン管理 |

### 4.2 通信モジュール ⇄ PC間（Bluetooth Low Energy）

| 設計項目 | 内容 |
|---|---|
| プロファイル | GATTサービス／キャラクタリスティック設計（センサーデータ用・モデル転送用・制御コマンド用を分離） |
| MTUサイズ | 最大化交渉（BLE 5対応であれば最大247バイト程度まで拡張可能）し転送効率を上げる |
| スループット対策 | 実効数十〜数百kbps程度を前提に、モデルは分割転送＋チャンクごとのACKを必須設計とする |
| PC側実装 | Pythonの`bleak`など、OS非依存のBLEライブラリを利用（8章で詳述） |
| 完全性検証 | CRCによるチャンク単位の誤り検出（暗号署名は行わない） |

---

## 5. データ管理（メモリ管理）要件

| 項目 | 内容 |
|---|---|
| 揮発メモリ（SRAM） | センサーデータの一時バッファ（リングバッファ／ダブルバッファ方式を検討） |
| 不揮発メモリ（外部Flash等） | 学習済みモデル、ファームウェアイメージ、設定値・ログの永続化 |
| メモリ管理方式 | ウェアレベリング、書き込み中の電源断対策（トランザクション的な書き込み） |
| 領域分離 | モデル格納領域とファームウェア格納領域を分離し、モデルのみの更新でファームウェアを壊さない設計（A/B領域方式を検討） |

---

## 6. 機械学習パイプライン・モデルバイナリ化要件

学習済みモデルのバイナリ化（Cコード生成）は独自実装せず、STマイクロエレクトロニクス公式ツールチェーンを利用する。

- STM32Cube AI StudioはX-CUBE-AIの後継として提供される最新の公式ツール（無償）
- X-CUBE-AI／STEdgeAI-Coreは、Keras・TensorFlow Lite・ONNX（PyTorch含む）形式の学習済みモデルを、STM32向けに最適化されたCコードへ自動変換する
- 重要: **これらはGUIアプリだけでなくコマンドラインインターフェース（CLI）としても提供されており、Pythonから完全に自動実行できる**（8章で詳述）

### 6.1 推奨パイプライン（音声分類版）
1. **データ収集**: STM32のMDF/ADF経由でMP23DB01HPマイクからPCM音声データを取得し、BLE経由でPCへ送信、ラベル付き音声データセットを蓄積（16kHz, モノラル, 1秒クリップ単位を基本とする。11.8節(2)参照）
2. **特徴量抽出**: PC側でlog-melスペクトログラムまたはMFCCを計算（学習・推論の前処理を一致させる必要があるため、STM32側でも同一アルゴリズムを実装する）
3. **学習**: Python（TensorFlow/Kerasなど）で音声分類モデル（軽量CNN/DS-CNN等、TinyML向け構成）を学習し、`.h5`または`.tflite`形式でエクスポート
4. **モデル変換（自動化）**: 同じPythonプロセス内から`stm32ai`（`stedgeai`）CLIをサブプロセスとして呼び出し、Cコードを自動生成
5. **STM32プロジェクトへの統合**: 生成されたCコードをSTM32CubeIDE等のビルド環境に組み込み、MDF/ADFで取得した音声フレームに対し特徴量抽出→推論を行うファームウェアをビルド
6. **配布**: 生成物（またはそこから抽出した重みパラメータのみ）を独自USBコマンド／BLE経由でSTM32へ転送し、不揮発メモリに書き込み

モデル全体（構造＋重み）を丸ごと転送するか、重みパラメータのみを転送してSTM32側の固定推論エンジンに読み込ませるかは設計上の分岐点であり、後者の方がOTA更新は軽量になりやすい。

---

## 7. ファームウェア更新（OTA）要件

| 項目 | 内容 |
|---|---|
| 更新経路 | 通信モジュール（BLE）経由でファームウェア／モデルイメージを受信し、不揮発メモリに書き込み |
| 更新の粒度 | ファームウェア全体更新 と モデルのみ更新（差分更新）を区別する設計を推奨 |
| 整合性検証 | CRC等による転送誤り検出のみ（暗号署名検証などのセキュリティ機能は対象外） |
| フェイルセーフ | 書き込み途中の電源断・通信断に対するロールバック機構（デュアルバンク方式などSTM32のフラッシュ機能を活用） |

---

## 8. ソフトウェア／プログラム作成計画

要件定義を実装に落とし込むための、PC側・STM32側それぞれのプログラム構成計画。

### 8.1 全体構成

```
project/
├── pc_side/                     # PC側（Python）
│   ├── ble_comm/                 # BLE通信モジュール
│   │   ├── ble_client.py         # bleak等を用いたBLE接続・送受信
│   │   └── protocol.py           # 独自コマンドプロトコルのエンコード/デコード（4章のフレーム仕様）
│   ├── data_collection/          # 音声データ収集
│   │   └── logger.py             # 受信した音声クリップをラベル付きデータセットとして保存
│   ├── ml_pipeline/               # 機械学習パイプライン
│   │   ├── audio_features.py      # log-melスペクトログラム/MFCC計算（STM32側と同一アルゴリズム）
│   │   ├── train.py               # 音声分類モデル学習（軽量CNN/DS-CNN、TensorFlow/Keras等）
│   │   ├── convert.py             # stm32ai CLIを呼び出すラッパー（6.1節手順4）
│   │   └── pack_weights.py        # 生成物から重みバイナリを抽出しBLE転送用に整形
│   └── main.py                    # 収集〜学習〜変換〜転送を一気通貫で実行するエントリポイント
│
├── stm32_firmware/               # STM32側（C/C++, STM32CubeIDEプロジェクト, ThreadX/USBX使用）
│                                  # ※ ディレクトリ構成の詳細は12章（X-CUBE-IOTA1を参考に再検討したMiddlewares構成）を参照
│
└── esp32_firmware/                # ESP32-H2-MINI側（ESP-IDF, BLE中継)
    ├── ble_gatt/                  # GATTサービス実装（4.2節、11.4節）
    └── usb_device_bridge/          # STM32側USB Hostに対するUSB Device（CDC-ACM）としての中継実装
```

### 8.2 PC側プログラム作成計画（Python）

| ステップ | 内容 | 主要ライブラリ |
|---|---|---|
| 1. BLE通信実装 | ESP32-H2とのBLE接続・GATT読み書き | `bleak`（クロスプラットフォーム） |
| 2. 独自プロトコル実装 | 4.1節のフレームフォーマットのエンコード/デコード、CRC計算 | 標準`struct`, `zlib.crc32`等 |
| 3. データ収集スクリプト | STM32から送られるセンサーデータを受信しデータセット化 | `pandas`, `numpy` |
| 4. 学習スクリプト | モデル学習、`.tflite`等でエクスポート | `tensorflow` |
| 5. モデル変換ラッパー | `subprocess`で`stm32ai generate`を呼び出し、Cコードを自動生成 | `subprocess`（標準ライブラリ） |
| 6. 転送用データ整形 | 生成物または重みバイナリをBLEチャンクサイズに分割 | 独自実装 |
| 7. 統合パイプライン | 1〜6を一つのコマンドで実行できるエントリポイント（`main.py`）に統合 | — |

`ml_pipeline/convert.py` の実装イメージ（6章の技術検証内容を反映）:

```python
import subprocess
from pathlib import Path

def convert_model_to_c(model_path: str, output_dir: str = "stm32ai_output") -> Path:
    """学習済みモデルをSTM32向けCコードへ変換する（GUI操作不要）"""
    subprocess.run(
        [
            "stm32ai", "generate",
            "-m", model_path,
            "--allocate-inputs",
            "--allocate-outputs",
            "-o", output_dir,
        ],
        check=True,
    )
    return Path(output_dir)
```

### 8.3 STM32側ファームウェア作成計画（C/C++, STM32CubeIDE）

12章で改訂したDrivers/Middlewares/Projects構成を前提とする。

| ステップ | 内容 |
|---|---|
| 1. 音声フロントエンド実装 | MDF/ADFペリフェラル経由でMP23DB01HPマイクからPCMデータ取得（`BSP_AUDIO_IN_Init`ベース、`Drivers/BSP/`）、log-melスペクトログラム/MFCC特徴量抽出（`Projects/.../Core/audio_frontend/`、11.8節(2)） |
| 2. メモリ管理実装 | SRAMリングバッファ、外部Flashへの書き込み・領域分離（`Core/memory_mgmt/`、5章） |
| 3. ThreadX/USBX導入 | `Middlewares/ST/threadx`, `Middlewares/ST/usbx`をSTM32CubeMXで有効化し、USBX Host CDC-ACMクラスを構成（11.1節） |
| 4. 独自コマンドプロトコル実装 | `Middlewares/protocol/`にRTOS非依存のフレーム送受信・CRCチェックを実装（4.1節、11.2節） |
| 5. 推論エンジン組み込み | `stm32ai generate`が出力したCコードを`X-CUBE-AI/`に配置し、`Core/inference/`から推論APIを呼び出す |
| 6. OTA機構実装 | BLE経由で受信したモデル/ファームウェアの不揮発メモリ書き込み、フェイルセーフ（`Core/ota/`、7章） |

### 8.4 ESP32-H2-MINI側ファームウェア作成計画（ESP-IDF）

| ステップ | 内容 |
|---|---|
| 1. BLE GATTサーバー実装 | センサーデータ用・モデル転送用・制御コマンド用のキャラクタリスティックを分離定義 |
| 2. STM32とのブリッジ実装 | USB/UART経由でSTM32の独自コマンドプロトコルとBLEパケットを相互変換 |
| 3. スループット検証 | 実機でのBLE転送速度計測、チャンクサイズの最適化 |

---

## 9. 開発マイルストーン

| フェーズ | 内容 | 主な成果物 |
|---|---|---|
| M1 | B-U585I-IOT02Aでのセンサーデータ取得・SRAM/Flashへの記録確認 | 基本ファームウェア |
| M2 | USB独自コマンドプロトコルの設計・実装（PC⇄STM32、8.2/8.3節） | プロトコル仕様書、通信ドライバ |
| M3 | BLE経由でのPCとの無線データ収集パイプライン構築 | データロガーアプリ |
| M4 | Pythonからの`stm32ai` CLI自動呼び出し・実機推論の検証 | 動作するAI推論ファームウェア、`convert.py` |
| M5 | モデル無線配布・不揮発メモリへの書き込みフローの実装 | OTAモデル更新機構 |
| M6 | ファームウェア全体のOTA更新機構の実装 | OTAファームウェア更新機構 |
| M7 | 自作基板（STM32U575QGI6 + ESP32-H2-MINI）への移植 | 自作基板ファームウェア |

---

## 10. 未確定・要判断事項（優先度順）

1. モデル転送は「モデル全体（Cコード一式）」か「重みパラメータのみ」か（BLEスループットとフラッシュ書き込み容量の両面から検討）
2. 不揮発メモリの書き込み耐久性（Flashの書き換え回数制限）に対する運用方針
3. USB独自コマンドプロトコル（STM32⇄通信モジュール間）の詳細仕様（フレームフォーマット、CRC方式など）の確定
4. BLE GATTサービス設計（キャラクタリスティック分割、MTUサイズ）とモデル分割転送の具体的なチャンクサイズ
5. `stm32ai generate`の実行環境構築（PC側学習スクリプトと同一のPython環境にまとめるか、別プロセス/CIパイプラインにするか）
6. モデル変換後のCコードをSTM32ファームウェアへ組み込むビルドフローの自動化範囲（8.2〜8.3節の連携方法）

---

## 11. AI実装エージェント向け補足仕様（本版で追加）

8章までの内容だけでは、実装を担当するAI（コーディングエージェント）が随所で仕様を「推測」して埋める必要があり、そのままでは実行しきれない。以下、技術的に確定できる部分を補完する。**11.8節のみユーザー確認が必要**なため、それ以外は本版で解決済みとして扱ってよい。

### 11.1 STM32 ⇄ ESP32-H2間 USBインターフェースの役割定義

これまで「USB通信」とだけ記載されていたが、USB通信には必ずHost/Deviceの役割分担が必要であり、これが未定義だった。

- **STM32U575/U585はUSB OTG_FSペリフェラルを搭載し、Host/Deviceどちらの役割も担える**（STマイクロエレクトロニクスのUSBハードウェアガイドラインでUSB 2.0 FS host/deviceインターフェースとして案内されている）
- **ESP32-H2はUSB Serial/JTAGペリフェラル（Device専用）を搭載**しており、Hostにはなれない
- したがって役割は一意に決まる: **STM32 = USB Host、ESP32-H2 = USB Device（CDC-ACMクラスとして列挙）**
- STM32側の実装には、STM32CubeMX上で `USB_OTG_FS` を **Host モード**・**USBX（Azure RTOS）+ ThreadX Core** を有効化する必要がある（STM32CubeIDEのUSBX Host CDC-ACMクラスを使用）。これはベアメタル実装では完結しないため、**USBX/ThreadXミドルウェアの導入が事実上必須**になる点に注意。

### 11.2 通信プロトコル詳細仕様（4.1節の具体化）

```
フレームフォーマット（Little Endian固定）:
+--------+--------+--------+----------+-----------+--------+--------+
| SOF    | CMD_ID | SEQ_NO | LEN      | PAYLOAD   | CRC16  | EOF    |
| 1byte  | 1byte  | 1byte  | 2byte    | 0-1024byte| 2byte  | 1byte  |
+--------+--------+--------+----------+-----------+--------+--------+

SOF (Start of Frame) : 0xAA 固定
EOF (End of Frame)    : 0x55 固定
SEQ_NO                : 0-255循環。再送判定・重複排除に使用
LEN                   : PAYLOADのバイト長（0〜1024）
CRC16                 : CRC-16/CCITT-FALSE（多項式0x1021、初期値0xFFFF）、SOF〜PAYLOADまでを対象
```

コマンドID一覧（初期案）:

| CMD_ID | 名称 | 方向 | 内容 |
|---|---|---|---|
| 0x01 | SENSOR_DATA | STM32→PC | センサーデータ送信 |
| 0x02 | MODEL_CHUNK | PC→STM32 | モデルデータの分割送信（1チャンク） |
| 0x03 | MODEL_COMPLETE | PC→STM32 | モデル全チャンク送信完了通知 |
| 0x04 | FW_CHUNK | PC→STM32 | ファームウェアイメージの分割送信 |
| 0x05 | FW_COMPLETE | PC→STM32 | ファームウェア送信完了通知 |
| 0x06 | STATUS_REQ | PC→STM32 | ステータス問い合わせ |
| 0x07 | STATUS_RESP | STM32→PC | ステータス応答 |
| 0x7E | ACK | 双方向 | 正常受信応答（SEQ_NOを含む） |
| 0x7F | NACK | 双方向 | 異常応答（エラーコードを含む） |

通信パラメータのデフォルト値:
- ACKタイムアウト: 500ms
- 再送回数上限: 3回（超過でエラー終了しSTATUS_RESPでエラー通知）
- モデル/ファームウェア1チャンクの最大PAYLOADサイズ: 1024バイト

### 11.3 不揮発メモリ（外部NORフラッシュ）アドレスマップ（AT25QF128A, 16MB想定）

| 領域 | アドレス範囲（例） | 用途 |
|---|---|---|
| ファームウェア領域A | 0x000000 - 0x1FFFFF (2MB) | 現行ファームウェア |
| ファームウェア領域B | 0x200000 - 0x3FFFFF (2MB) | OTA更新用（A/B方式、7章） |
| モデル領域A | 0x400000 - 0x5FFFFF (2MB) | 現行モデル |
| モデル領域B | 0x600000 - 0x7FFFFF (2MB) | OTA更新用 |
| センサーログ領域 | 0x800000 - 0xBFFFFF (4MB) | リングバッファとして使用 |
| 設定・メタデータ領域 | 0xC00000 - 0xC0FFFF (64KB) | バージョン情報、有効領域フラグ(A/B切替) |

※実際のセンサー種別・モデルサイズが確定した時点で容量配分は見直す。

### 11.4 BLE GATTプロファイル定義（4.2節の具体化）

| 項目 | UUID（暫定・独自定義でよい） |
|---|---|
| プライマリサービス | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`（Nordic UART Service方式を踏襲） |
| センサーデータ Characteristic（Notify） | `6E400002-...` |
| コマンド送信 Characteristic（Write） | `6E400003-...` |
| 応答受信 Characteristic（Notify） | `6E400004-...` |

MTUは接続確立後にBLE 5のExtended MTU Negotiationで最大247バイトまで拡張要求する。拡張に失敗した場合は既定の23バイト（ATT最小値）にフォールバックする。

### 11.5 開発環境・ツールチェーンのバージョン固定（推奨）

| ツール | バージョン目安 | 備考 |
|---|---|---|
| STM32CubeIDE | 1.15.x以降 | STM32U5対応版であること |
| STM32Cube_FW_U5 | 最新安定版 | HAL/USBX/ThreadXを含む |
| X-CUBE-AI / STEdgeAI-Core | 最新安定版（`stm32ai`/`stedgeai` CLI同梱） | 8.2節のconvert.pyが依存 |
| ESP-IDF | v5.x系（ESP32-H2対応版） | BLE + USB Serial/JTAGを使用 |
| Python | 3.10以降 | PC側全般 |
| 主要pipパッケージ | `tensorflow`, `bleak`, `numpy`, `pandas` | `requirements.txt`として固定推奨 |

### 11.6 マイルストーン受け入れ基準（9章の具体化）

| フェーズ | 完了の定義（Definition of Done） |
|---|---|
| M1 | MDF/ADF経由でマイクからPCMデータを取得し、1秒分のクリップをUART等でPCへダンプして波形が正しく確認できる |
| M2 | 11.2節のプロトコルでPC⇄STM32間にダミーデータを送受信し、CRCエラー時に自動再送されることを確認 |
| M3 | STM32→ESP32→PC(BLE)の経路で音声クリップが欠損なく受信・WAV/CSV保存できる |
| M4 | `convert.py`実行で`.tflite`から生成されたCコードがビルドでき、STM32上で録音した音声に対し推論結果（クラスラベル）が得られる |
| M5 | PC側から送信したモデルがモデル領域Bに書き込まれ、再起動後に領域切替で反映される |
| M6 | ファームウェア全体のOTA更新後、旧バージョンへのロールバックが電源断シミュレーションでも成功する |
| M7 | 自作基板上でM1〜M6と同等の動作が再現できる |

### 11.7 実行方式の前提（明記が必要だった暗黙の決定事項）

- **RTOS**: STM32側はUSBX/ThreadX（11.1節）を使用するため、実質的にThreadXベースの実装となる。センサー取得・推論・通信を素朴なsuperloopで書くのではなく、ThreadXのタスク（スレッド）分割を前提とする
- **ビルドシステム**: STM32CubeIDE標準のプロジェクト管理（Makefile自動生成）を使用し、CI等での自動ビルドが必要になった場合は別途`arm-none-eabi-gcc`＋Makefile化を検討

### 11.8 確定：推論対象タスク仕様（音声分類）

タスクを「音声分類（マイクで音を識別）」に確定。それに伴い、以下を具体化する。

#### (1) 入力センサー・ハードウェアインターフェース
- B-U585I-IOT02Aには**MP23DB01HPTR デジタルMEMSマイクが2基**搭載されており、<cite index="60-1">これらのマイクはMCUのADF/MDFインターフェースに接続されている</cite>
- STM32U5シリーズはPDM（Pulse Density Modulation）出力のマイクをMDF（Multi-function Digital Filter）／ADF（Analog Digital Filter）ペリフェラルでPCMデータに変換する。STM公式BSPの<cite index="62-1">オーディオ入力処理では、マイク1にはADF1_Filter0、マイク2（またはMDF側）にはMDF1_Filter0が使用されている</cite>
- STM32CubeU5のBSPサンプル（`b_u585i_iot02a_audio.c`、`BSP_AUDIO_IN_Init`）をベースに実装することを推奨。独自実装よりも公式BSP関数の再利用を優先する
- 自作基板（STM32U575QGI6）でも同系統のPDMマイク＋MDF/ADF構成を踏襲する想定（マイク型番は自作基板側で別途選定）

#### (2) 音声フロントエンド仕様（ベースライン・要最終確認）
| 項目 | 既定値 | 備考 |
|---|---|---|
| サンプリングレート | 16 kHz, モノラル（マイク1chのみ使用） | 音声分類タスクの一般的な標準値 |
| 推論ウィンドウ長 | 1秒（16,000サンプル） | キーワードスポッティング等でよく使われる長さ |
| ウィンドウのスライド幅 | 500ms（50%オーバーラップ） | 連続監視を想定 |
| 特徴量抽出 | log-melスペクトログラムまたはMFCC（40 mel bins、フレーム長25ms、ホップ長10ms） | X-CUBE-AIのAudio系サンプルで一般的な設定 |
| 推論頻度 | 500msごとに1回（スライド幅と同期） | 11.6節の受け入れ基準にも反映 |

#### (3) 未確定のまま残る項目（技術仕様ではなく用途に依存するため要ユーザー判断）
- **具体的な分類クラス（何の音を識別するか）**: 例）特定のキーワード／環境音の種類／機械の異常音など、用途によってラベル設計が大きく変わる
- **クラス数**: 上記が決まらないと確定できない
- **背景音（unknown/背景）クラスの要否**: 常時稼働する分類器では「該当なし」を明示的な1クラスとして持つのが一般的

→ この3点は、実際にデータ収集を始める前（8.2節の`train.py`実装前）までに確定すればよく、現時点でプロジェクト全体の設計をブロックするものではない。学習パイプラインの雛形（クラス数を可変長にした汎用実装）は先行して作成可能。

---

## 12. ミドルウェア構想の再検討（X-CUBE-IOTA1を参考に）

STMicroelectronics公式の [X-CUBE-IOTA1](https://github.com/STMicroelectronics/x-cube-iota1) を調査した。これはB-U585I-IOT02A向けにIOTA分散台帳技術を実装するためのSTM32Cube拡張パッケージで、<cite index="20-1">WiFi管理、暗号化・ハッシュ・メッセージ認証・電子署名（sodium/mbedCrypto）、Azure RTOS ThreadXとNetXDuo、Tangleと通信するためのIOTA Client APIといったミドルウェアライブラリと、STSAFEセキュアエレメントによるハードウェアルートオブトラストを備えている</cite>。また<cite index="20-1">モーションセンサーおよび環境センサーにアクセスするアプリケーションを構築するための完全なドライバも提供している</cite>。

**注意**: このリポジトリは2024年2月15日付でアーカイブ済み（開発終了）であり、直接の依存先としては使わない。あくまで「STがB-U585I-IOT02A向けにどのような層構造でミドルウェアを組んでいるか」という設計パターンの参考として扱う。

### 12.1 X-CUBE-IOTA1から採用する要素

| 要素 | 採用理由 |
|---|---|
| **Drivers / Middlewares / Projects の3層構成** | STM32Cube拡張パッケージの標準的なディレクトリ構成。公式BSP（audio, MEMSセンサー等）をDriversにそのまま取り込める |
| **Azure RTOS ThreadX** | 11.1節・11.7節で既に「USBX Host使用にはThreadXが前提」と結論済み。X-CUBE-IOTA1もThreadXを採用しており、STM32U5×ThreadXの組み合わせが公式にサポートされた構成であることの裏付けになる |
| **Middlewares層を「RTOS依存部」と「プロトコル/ロジック部」に分離する発想** | X-CUBE-IOTA1ではIOTA Client APIやL2SecプロトコルがThreadX/NetXDuoの上位に独立したモジュールとして実装されている。同様に、本プロジェクトの独自USBコマンドプロトコル（4.1節）もThreadX/USBXに直接依存させず、独立した「protocolモジュール」として実装すべきという設計方針が裏付けられる |
| **公式BSPドライバの流用方針** | 11.8節で言及した`BSP_AUDIO_IN_Init`等の公式BSP関数をDrivers層にそのまま組み込む方針を維持 |

### 12.2 X-CUBE-IOTA1から採用しない要素（理由）

| 要素 | 採用しない理由 |
|---|---|
| STSAFE-A110セキュアエレメント、sodium/mbedCrypto（暗号化・署名） | 3.3節(B)・7章で確定済みの通り、本プロジェクトはセキュリティ保護を対象外としている |
| WiFi管理、NetXDuo（TCP/IPスタック） | 3.3節(A)で確定済みの通り、Wi-Fiは使用せずBluetooth（BLE）に一本化している |
| IOTA Client API、Tangle通信ロジック | 分散台帳技術は本プロジェクトの要件に含まれない |

### 12.3 改訂後のミドルウェア構成図（STM32側）

```
Projects/B-U585I-IOT02A/Applications/AudioClassification/   ← アプリケーション層
        │  audio_frontend / memory_mgmt / ota / inference（推論API呼び出し）
        ▼
Middlewares/
   ├── ST/threadx/     ← Azure RTOS ThreadX（USBX Hostの前提、11.1節）
   ├── ST/usbx/        ← USBX Host CDC-ACM（STM32⇄ESP32-H2間、4.1節）
   └── protocol/       ← 独自コマンドプロトコル（RTOSに依存しない独立モジュール、4.1/11.2節）
        ▼
Drivers/
   ├── BSP/B-U585I-IOT02A/   ← 公式BSP流用（BSP_AUDIO_IN_Init等、11.8節）
   ├── STM32U5xx_HAL_Driver/
   └── CMSIS/
```

### 12.4 stm32_firmwareディレクトリ構成の改訂（8.1節の更新）

8.1節で示した`stm32_firmware/`は簡略化された独自構成だったが、STM32Cube拡張パッケージの標準構成（X-CUBE-IOTA1と同様）に合わせて以下のように改訂する。STM32CubeIDEでのプロジェクト生成・公式BSPのマージが容易になる。

```
stm32_firmware/
├── Drivers/
│   ├── BSP/B-U585I-IOT02A/          # 公式BSP（audio, MEMS I2Cセンサー等）をそのまま配置
│   ├── CMSIS/
│   └── STM32U5xx_HAL_Driver/
├── Middlewares/
│   ├── ST/
│   │   ├── threadx/                  # Azure RTOS ThreadX
│   │   └── usbx/                     # USBX Host CDC-ACMクラス
│   └── protocol/                     # 独自コマンドプロトコル実装（RTOS非依存）
│       ├── frame_codec.c/.h          # 11.2節のフレームエンコード/デコード、CRC16計算
│       └── command_handler.c/.h      # コマンドID別のディスパッチ処理
└── Projects/
    └── B-U585I-IOT02A/
        └── Applications/
            └── AudioClassification/
                ├── Core/
                │   ├── audio_frontend/    # MDF/ADF経由のPCM取得・特徴量抽出（11.8節）
                │   ├── memory_mgmt/       # 揮発/不揮発メモリ管理（5章、11.3節のアドレスマップ）
                │   ├── ota/               # モデル/ファームウェア書き込み・ロールバック（7章）
                │   └── inference/         # stm32ai generate 出力を統合する推論モジュール
                └── X-CUBE-AI/             # stm32ai generateの出力先（自動生成物）
```

## 13. 参考文献（STMicroelectronics公式GitHub）

[github.com/STMicroelectronics](https://github.com/STMicroelectronics) を調査し、本プロジェクトの各コンポーネントに直接対応する公式リポジトリを整理する。独自実装を最小限にし、可能な限りこれらの公式サンプル・ツールチェーンをベースにすることを推奨する。

### 13.1 音声分類・MLパイプライン関連（6章・8.2節に対応）

| リポジトリ | 内容 | 本プロジェクトでの位置付け |
|---|---|---|
| [`stm32ai-modelzoo`](https://github.com/STMicroelectronics/stm32ai-modelzoo) | STM32向け学習済みAIモデル集。<cite index="21-1">Hugging Face上のSTMicroelectronics Organizationにも各モデルのモデルカードが公開されている</cite> | **Audio Event Detection (AED)** カテゴリに音声分類向けの学習済みYamnetモデルが含まれる。ゼロから学習せず、これを起点に転移学習する選択肢がある |
| [`stm32ai-modelzoo-services`](https://github.com/STMicroelectronics/stm32ai-modelzoo-services) | <cite index="23-1">ユーザーデータセットでモデルを再学習・量子化・評価・ベンチマークするスクリプト、およびユーザーAIモデルから自動生成されたアプリケーションコード例を提供する</cite> | 8.2節で計画した`train.py`/`convert.py`を自作する代わりに、このリポジトリのAudio Event Detection用config（YAML）と`stm32ai_main.py`を使うことで、学習〜量子化〜STM32向けCコード生成〜（対応ボードなら）デプロイまでを一括自動化できる。**独自実装より優先して検討すべき** |
| Yamnetモデル（AEDカテゴリ内） | <cite index="20-1">Google製の音声分類モデルYamnetをマイコン向けに軽量化したもの（埋め込みベクトル256次元版）で、ESC-10やFSD50K等のデータセットで事前学習済み</cite> | 転移学習のベースモデル候補。<cite index="20-1">オリジナルのYamnetは25msウィンドウ・10msホップでスペクトログラムに変換し、125〜7500Hzに周波数をクリップする</cite>設定で、11.8節(2)で定義した特徴量抽出パラメータとほぼ整合する |
| [`STM32AI_Overall_Offer`](https://github.com/STMicroelectronics/STM32AI_Overall_Offer) | <cite index="26-1">STM32のAI関連GitHubリポジトリすべてへの入口となるリポジトリ</cite> | 上記以外にも音声・画像・センサー系のAI関連リポジトリを探す際の起点として活用 |

### 13.2 USB通信（Host CDC_ACM）関連（4.1節・11.1節に対応）

| リポジトリ | 内容 | 本プロジェクトでの位置付け |
|---|---|---|
| [`stm32-usbx-examples`](https://github.com/STMicroelectronics/stm32-usbx-examples) | <cite index="30-1">USBXミドルウェアを使用したSTM32製品向けのUSBサンプル一式で、USB DeviceとHost両方のアプリケーション開発方法を示す</cite> | **`Projects/NUCLEO-H723ZG/Applications/USBX/Ux_Host_CDC_ACM`** が11.1節で確定した「STM32=USB Host、ESP32-H2=USB Device（CDC-ACM）」構成の直接の参考実装になる。<cite index="34-1">USBX Host CDC_ACMのサンプルはCDCデバイスを正しく列挙するためのリクエストとデータ送受信APIを提供し、tx_application_define()内でUSBリソースを初期化した後、USB初期化スレッド・送信スレッド・受信スレッドの3つを異なる優先度で生成する</cite>構成になっており、Middlewares/protocol層（12章）の上に載せるスレッド設計の雛形として使える |
| [`STM32CubeU5`](https://github.com/STMicroelectronics/STM32CubeU5) | STM32U5シリーズのフルファームウェアパッケージ（HAL/LLドライバ、CMSIS、BSP、ミドルウェア、Projects） | **`Projects/NUCLEO-U575ZI-Q/Applications/USBX/Ux_Device_CDC_ACM`** は自作基板のMCU（STM32U575QGI6）と同じU575ファミリでのUSBX実績サンプル。Device視点だが、ThreadXの`tx_user.h`設定（Systickをタイムベースに使うためTIMへの切替が必須、等）はHost側実装でも共通の注意点として参照できる |

### 13.3 その他（12章の補足）

| リポジトリ | 内容 |
|---|---|
| [`x-cube-iota1`](https://github.com/STMicroelectronics/x-cube-iota1) | 12章で参照済み。Drivers/Middlewares/Projects構成とThreadX採用の参考（アーカイブ済み） |
| [`b-u585i-iot02a-bsp`](https://github.com/STMicroelectronics/b-u585i-iot02a-bsp) | 11.8節で参照済み。`BSP_AUDIO_IN_Init`等の公式BSP |

### 13.4 追加の設計上の気づき: USBXの「Standalone」動作モード

調査の過程で、<cite index="37-1">USBXはThreadXなしの「スタンドアロンモード」でも動作させられる例があり、実際にUSBXとThreadXのオーバーヘッドを避けたいという要望に対し、送受信をスレッドではなくシンプルな送受信ループに置き換えて動作させたという報告がある</cite>。STMicroelectronics公式でも[`x-cube-azrtos-h7`](https://github.com/STMicroelectronics/x-cube-azrtos-h7)に`Ux_Device_CDC_ACM_Standalone`という非ThreadX版サンプルが存在する（H7向け、Device側）。

11.1節・11.7節では「USBX利用にはThreadXが事実上必須」としていたが、**Standaloneモードが使えればThreadXの学習コスト・オーバーヘッドを回避できる可能性がある**。ただし現時点でU5×Host×Standaloneの組み合わせの公式サンプルは未確認のため、以下を今後の検討事項として追加する。

- STM32CubeMXでUSBX Host CDC_ACMをStandaloneモードで生成できるか確認する
- できない場合は、11.1節の方針（ThreadX前提）のまま進める

## 14. プロジェクト作成に必要な参考リンク・CubeMX/IDE設定項目

### 14.1 開発ツール・ダウンロードリンク

| ツール | リンク |
|---|---|
| STM32CubeMX | https://www.st.com/en/development-tools/stm32cubemx.html |
| STM32CubeIDE | https://www.st.com/en/development-tools/stm32cubeide.html |
| STM32Cube AI Studio（旧X-CUBE-AI後継、CLIは`stedgeai`） | https://www.st.com/en/development-tools/stedgeai-cubeai.html |
| X-CUBE-AI（STM32CubeMX組み込み版。UM2526にセットアップ手順あり） | https://www.st.com/en/embedded-software/x-cube-ai.html |
| STM32Cube.AI Developer Cloud（オンライン版、任意） | https://stm32ai-cs.st.com |
| STM32CubeU5（U575/U585共通ファームウェアパッケージ、HAL/BSP/ThreadX/USBX同梱） | https://github.com/STMicroelectronics/STM32CubeU5 |
| STM32CubeProgrammer（書き込みツール） | https://www.st.com/en/development-tools/stm32cubeprog.html |
| ESP-IDF（ESP32-H2用ツールチェーン） | https://docs.espressif.com/projects/esp-idf/en/latest/esp32h2/get-started/index.html |
| ESP-IDF BLE（NimBLEスタック）リファレンス | https://docs.espressif.com/projects/esp-idf/en/latest/esp32h2/api-reference/bluetooth/index.html |

### 14.2 ハードウェア資料リンク

| 資料 | リンク |
|---|---|
| B-U585I-IOT02A ユーザーマニュアル（UM2839） | https://www.st.com/resource/en/user_manual/um2839-discovery-kit-for-iot-node-with-stm32u5-series-stmicroelectronics.pdf |
| B-U585I-IOT02A 製品ページ | https://www.st.com/en/evaluation-tools/b-u585i-iot02a.html |
| STM32U575/U585 データシート・製品ページ | https://www.st.com/en/microcontrollers-microprocessors/stm32u575-585.html |
| MP23DB01HP（デジタルマイク）データシート | https://www.st.com/resource/en/datasheet/mp23db01hp.pdf |
| AN4879（USBハードウェア・PCBガイドライン、Host/Device対応表を含む） | https://www.st.com/resource/en/application_note/an4879-introduction-to-usb-hardware-and-pcb-guidelines-using-stm32-mcus-stmicroelectronics.pdf |
| ESP32-H2-MINI-1 データシート | https://www.espressif.com/sites/default/files/documentation/esp32-h2-mini-1_mini-1u_datasheet_en.pdf |
| ESP32-H2 技術リファレンスマニュアル | https://www.espressif.com/sites/default/files/documentation/esp32-h2_technical_reference_manual_en.pdf |
| b-u585i-iot02a-bsp（公式BSPソース） | https://github.com/STMicroelectronics/b-u585i-iot02a-bsp |

13章に記載したGitHubリポジトリ（stm32ai-modelzoo、stm32ai-modelzoo-services、stm32-usbx-examples、x-cube-iota1）も参照。

### 14.3 STM32CubeMX ピン設定項目

B-U585I-IOT02A評価ボードをCubeMXでボード選択（Board Selector）すると大半は自動アサインされるが、確認・明示すべき主要ピンは以下の通り。

| 機能 | ピン | 備考 |
|---|---|---|
| I2C2（MEMSセンサー共通バス） | PH4 (SCL), PH5 (SDA) | <cite index="10-1">HTS221・IIS2MDCTR・LPS22HH・ISM330DHCX・VL53L5CX・STSAFE-A110・VEML6030がすべてこのI2C2バスに接続されている</cite>。本プロジェクトでは主にマイク以外は未使用だが、バス自体は初期化しておくと将来センサー追加が容易 |
| MEMSセンサー有効化GPIO | PH1 | MEMSモジュール全体のイネーブル信号（GPIO出力） |
| デジタルマイク（MP23DB01HPTR×2） | MDF/ADFインターフェース経由（具体的ピンは公式BSPの`.ioc`設定を流用） | 独自にピンを再設計せず、`b-u585i-iot02a-bsp`のCubeMX設定をインポートして流用することを推奨（11.8節） |
| USB（PC⇄STM32、デバッグ/プログラミング用のSTLINK-V3E経由VCP） | オンボードSTLINK-V3E経由、USBはボード上のUSB Type-Cコネクタに接続済み | B-U585I-IOT02Aは評価用。自作基板では別途USBコネクタ設計が必要 |
| USB_OTG_FS（STM32⇄ESP32-H2間、Hostモード、11.1節） | 自作基板側で新規に配線（評価ボードのUSBポートとは別系統として設計） | CubeMXでUSB_OTG_FSを**Host**モードに設定し、USBX Hostミドルウェアを有効化（14.4節）。評価フェーズではB-U585I-IOT02Aの予備コネクタ（STMod+/Pmod/Arduino Uno）経由でESP32-H2評価モジュールと接続する配線を別途検討 |
| RTC用32kHz水晶振動子 | オンボード実装済み | ThreadXのタイムベースには使用しない（Systick/TIMを使用、14.4節） |

自作基板（STM32U575QGI6）では上記のうちUSB_OTG_FSとMDF/ADFマイク入力、外部NORフラッシュ（AT25QF128A, OCTOSPI）のピン配置を新規に設計する必要がある。

### 14.4 ミドルウェア設定項目（STM32CubeMX / STM32CubeIDE）

| ミドルウェア | 設定項目 | 推奨値・備考 |
|---|---|---|
| **ThreadX** | `TX_TIMER_TICKS_PER_SECOND`（`tx_user.h`） | 既定100 tick/secのままでよいか要検証。<cite index="31-1">ThreadXはSystickをタイムベースとして使うため、HALは別のタイマ（TIM）に時間基準を切り替える必要がある</cite> |
| ThreadX | ヒープメモリサイズ（リンカスクリプト） | USBX等の使用量に応じて確保（サンプルでは64KB程度の例あり）。11.3節のFlashマップとは別にRAM側の割り当てとして別途設計 |
| **USBX (Host)** | クラス | CDC_ACM Hostクラスを有効化（`ux_host_class_cdc_acm`） |
| USBX (Host) | スレッド構成 | USB初期化スレッド／送信スレッド／受信スレッドを別優先度で生成する構成を推奨（13.2節のUx_Host_CDC_ACMサンプル参照） |
| USBX (Host) | Standaloneモードの可否 | 13.4節の通り要検証。CubeMXの「USBX」設定画面でThreadXなし構成が選択可能か確認する |
| **X-CUBE-AI** | STM32CubeMXの「Software Packs」→「X-CUBE-AI」を追加 | Additional SoftwareからX-CUBE-AIを選択し、モデルファイル（`.tflite`等）をインポートしてNetwork設定を行う。GUIで完結させず、8.2節の`convert.py`（`stm32ai generate` CLI呼び出し）で同等の処理を自動化する方針と両立可能 |
| **MDF/ADF（オーディオ入力）** | クロック設定・フィルタ設定 | 公式BSPの`BSP_AUDIO_IN_Init`が内部で行うMDF/ADFの初期化パラメータをCubeMX上でも整合させる（11.8節） |
| **OCTOSPI（外部NORフラッシュ、自作基板）** | Quad I/Oモード、メモリマップドモード可否 | AT25QF128Aのデータシートに基づきCubeMXのOCTOSPI設定を行い、11.3節のアドレスマップに沿ってメモリ領域を定義 |

### 14.5 STM32CubeIDE プロジェクト設定項目

| 項目 | 内容 |
|---|---|
| ターゲットデバイス | 評価: STM32U585AII6Q（B-U585I-IOT02A）／自作基板: STM32U575QGI6 |
| リンカスクリプト | 11.3節のFlashメモリマップ（ファームウェアA/B領域、モデルA/B領域）に対応するセクション分割を`.ld`ファイルに追記。OTA用にリンカスクリプトを複数用意（現行イメージ用・更新後イメージ用）する構成を推奨 |
| 最適化レベル | Debugビルド: `-Og`、Releaseビルド: `-O2`（推論処理の実行速度が要件になるため、最終評価は`-O2`または`-O3`でのベンチマークを推奨） |
| デバッグインターフェース | オンボードSTLINK-V3E（SWD） |
| FPU設定 | STM32U5はCortex-M33＋FPU搭載のため`-mfpu=fpv5-sp-d16 -mfloat-abi=hard`を確認 |

### 14.6 ESP32-H2-MINI側（ESP-IDF, menuconfig）設定項目

| 項目 | 内容 |
|---|---|
| Bluetooth | `idf.py menuconfig` → Component config → Bluetooth を有効化し、NimBLEスタックを選択（軽量・省メモリ） |
| USB Serial/JTAG | STM32側とのUSB接続はESP32-H2内蔵の**USB Serial/JTAGコントローラ**（CDC-ACM）を使用。<cite index="57-1">USB_D+/USB_D-ピンはGPIO26～GPIO27（FSPICS4～FSPICS5と共用）にマルチプレクスされている</cite> |
| パーティションテーブル | OTA機構（7章）を見据え、`ota_0`/`ota_1`の2パーティション構成を検討（ただしOTA機構自体は主にSTM32側が担うため、ESP32-H2側は主に中継役に留める設計であれば単一パーティションでも可） |
| ログレベル | 開発中は`CONFIG_LOG_DEFAULT_LEVEL_DEBUG`、量産想定では`INFO`以下に調整 |

### 14.7 【将来的な拡張オプション】Wi-Fi・セキュリティ有効化のための設定項目

3.3節・7章で確定した通り、本プロジェクトの現行スコープでは**Wi-Fiは使用せず、セキュリティ保護も対象外**である。この結論自体は変更しないが、将来的に要件が変わった場合に備え、有効化に必要な設定項目を参考情報として付録的に記載する。

#### (A) Wi-Fiを有効化する場合
| 項目 | 内容 |
|---|---|
| ハードウェア | B-U585I-IOT02Aの場合はオンボードEMW3080 Wi-Fiモジュールが利用可能（<cite index="10-1">STM32U585AIIとはSPI等で接続されたMXCHIP製Wi-Fiモジュール</cite>）。自作基板では別途Wi-Fiモジュールの追加実装が必要（現行のESP32-H2はWi-Fi非搭載のため、モジュール変更が必要。3.3節参照） |
| ミドルウェア | Azure RTOS **NetXDuo**（TCP/IPスタック）をCubeMXで追加。X-CUBE-IOTA1（12章）でも採用されていた構成 |
| CubeMX設定 | 「Software Packs」からWi-Fi関連ミドルウェア（`mx_wifi`等、EMW3080用ドライバ）を追加し、SPI/UARTインターフェースを設定 |

#### (B) セキュリティ保護を有効化する場合
| 項目 | 内容 |
|---|---|
| 暗号化ペリフェラル | STM32U585のAES/PKA/OTFDEC（3.3節(B)で言及。STM32U575での搭載有無は要データシート確認） |
| セキュアエレメント | STSAFE-A110（B-U585I-IOT02Aにオンボード搭載、X-CUBE-IOTA1が参考実装を提供） |
| TrustZone | STM32U5シリーズはArm TrustZoneをネイティブサポート。CubeMXの「Trust Zone」タブでセキュア/非セキュア領域を分割定義 |
| 暗号化ライブラリ | mbedTLSまたはX-CUBE-IOTA1が採用しているsodium/mbedCryptoを追加ミドルウェアとして導入 |
| ファームウェア署名検証 | STM32CubeProgrammerのセキュアブート機能（TrustZone/RSS）と組み合わせてOTA更新時の署名検証を追加 |

有効化を判断する場合は、3.3節・7章・12章の該当箇所を合わせて更新すること。
- 2026-07-03: Wi-Fi不使用・Bluetooth一本化、セキュリティ要件を対象外に変更、モデル変換のPython自動化（stm32ai CLI）に関する要件を追記
- 2026-07-03: 上記修正内容を反映し全体を再構成。8章「ソフトウェア／プログラム作成計画」を新設
- 2026-07-03（本版）: 11章「AI実装エージェント向け補足仕様」を新設。USB Host/Device役割定義、通信プロトコルのバイトレベル仕様、Flashメモリマップ、BLE GATT定義、開発環境バージョン、マイルストーン受け入れ基準を追加。未解決事項として推論対象タスクの定義をユーザー確認事項として明記
- 2026-07-03（本版）: 推論対象タスクを「音声分類」に確定。オンボードMEMSマイク(MP23DB01HPTR×2, MDF/ADF接続)を用いた音声フロントエンド仕様、特徴量抽出方式、学習パイプライン、STM32実装ステップ、受け入れ基準を音声分類向けに具体化
- 2026-07-03（本版）: X-CUBE-IOTA1（STMicroelectronics公式、B-U585I-IOT02A向けIOTA DLT拡張パッケージ、現在はアーカイブ済み）を参考にミドルウェア構想を再検討。12章を新設し、Drivers/Middlewares/Projectsの3層構成とThreadX/USBXの採用根拠を整理。STSAFE・WiFi/NetXDuo・IOTA関連は本プロジェクトの要件外として不採用と明記。8.1節・8.3節をこの構成に合わせて更新
- 2026-07-03（本版）: STMicroelectronics公式GitHub組織を調査し、13章「参考文献」を新設。stm32ai-modelzoo/-services（音声分類・Yamnet転移学習の既存パイプライン）、stm32-usbx-examples（USBX Host CDC_ACMの直接参考実装）、STM32CubeU5（U575ファミリでのUSBX実績サンプル）を追加。USBXのStandaloneモード（ThreadX不要の可能性）を今後の検討事項として明記
- 2026-07-03（本版）: 14章「プロジェクト作成に必要な参考リンク・CubeMX/IDE設定項目」を新設。開発ツール・ハードウェア資料のダウンロードリンク一覧、STM32CubeMXのピン設定項目（I2C2=PH4/PH5等）、ThreadX/USBX/X-CUBE-AI/MDF/OCTOSPIのミドルウェア設定項目、STM32CubeIDEプロジェクト設定、ESP-IDF側menuconfig項目を追加。Wi-Fi・セキュリティは現行スコープ外の結論を維持しつつ、将来有効化する場合の設定項目（NetXDuo/mx_wifi、STSAFE-A110/TrustZone/mbedTLS等）を14.7節に付録として追加
