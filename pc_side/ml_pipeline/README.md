# ML→ファームウェア自動パイプライン

音声分類モデルの「学習 → STエッジAIコンパイラ(stedgeai CLI)でC++コード化 → ファームウェアに組込 → ビルド → 基板書き込み」を1コマンドで行う。

## 使い方

```bash
cd pc_side/ml_pipeline

# フル実行（学習→変換→統合→ビルド→ST-LINK書き込み）
python pipeline.py

# 学習をスキップして既存モデルを使う
python pipeline.py --skip-train

# ビルドまで（ボード無しでOK）
python pipeline.py --steps train,generate,integrate,build

# 書き込みをOTA(通信)経由にする（UARTまたはWi-Fi）
python pipeline.py --ota COM9
python pipeline.py --ota 192.168.4.1:5000
```

## 各ステップ

| ステップ | 内容 | 所要時間 |
|---|---|---|
| train | `train.py`: 1D-CNN学習→`build/model.tflite`(23KB)。`dataset/<クラス名>/*.wav`(16kHzモノラル)があればそれを使用、無ければ合成3クラス(silence/tone/noise) | ~1分 |
| generate | `stedgeai generate --target stm32u5` → `build/stm32ai/audio_net*.c/h`（重み16.5KB/RAM16KB/25万MACC） | ~7分 |
| integrate | 生成コード＋`ai_labels.h`を `Secure/Core/AI/` へコピー | 数秒 |
| build | CubeIDEヘッドレスビルド→`build/firmware.bin` | ~2分 |
| flash / ota | ST-LINK書き込み or OTAステージング | ~30秒 |

## モデル仕様（変更する場合はtrain.pyを編集）

- 入力: **2048サンプル（128ms @16kHz、float32正規化）** — FWのマイクDMAバッファそのまま。窓長を変える場合はFW側バッファ(telemetry.cppのkAudioSamples)も要変更
- 出力: クラス確率（softmax）。クラス数は`dataset/`のフォルダ数で自動決定
- 独自データで学習: `dataset/{クラス名}/*.wav` を置いて `python pipeline.py`。ステータスモニタの録音機能（recordings/*.wav）がそのまま学習データに使える

## ボード上での実行

| キー | 動作 |
|---|---|
| `i` | 1回推論して結果表示（例: `[AI] tone  silence=1% tone=97% noise=2%  (12ms)`） |
| `I` | 2秒毎の自動推論ON/OFF |

## FW側の構成

- `Secure/Core/AI/` — stedgeai生成コード（パイプラインが上書き。手編集禁止）
- `Secure/Core/Src/ai_app.cpp` — 推論ラッパ（初期化・マイクバッファ→入力・実行・結果）
- `Middlewares/ST/AI/` — X-CUBE-AIランタイム（ヘッダ+`NetworkRuntime1020_CM33_GCC.a`、CM33/GCC用）

## 既知の注意点

- stedgeai generateは約7分かかる＋終了時に統計許可を聞くのでパイプラインは`n`を自動入力
- `firmware.bin`は約1MBになる（NSCセクションが0x0C0FE000にあるためobjcopyがギャップを埋める）。ST-LINK書き込みはELF使用なので影響なし。OTA転送時間だけ増える（改善候補: セクション分割）
- stedgeaiのパス: `D:/App/x-cube-ai-windows-v10.2.0/stedgeai-windows-10.2.0/Utilities/windows/stedgeai.exe`（移動したらpipeline.py冒頭の定数を更新）
