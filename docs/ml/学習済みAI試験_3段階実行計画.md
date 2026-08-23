# 学習済みAI試験 3段階実行計画

- 対象: B-U585I-IOT02A / STM32U585
- 目的: ST公式学習済みモデルを用い、再学習せずに話し声検出を最短で実証する
- ブランチ: `feature/ml-voice-classification-test`
- 作成日: 2026-08-23

## 1. 3段階の定義

| 段階 | 実行場所 | 基板 | STM32コンパイル環境 | 到達点 |
|---|---|---:|---:|---|
| 第1段階 | このチャット環境 | 不要 | 不要 | 調査、仕様固定、モデル取得、PC推論、検証コードを完成 |
| 第2段階 | ユーザーPC | 不要 | 必要 | ST解析、C生成、公式applicationと現行firmwareをbuild |
| 第3段階 | ユーザーPC＋B-U585I-IOT02A | 必要 | 必要 | 書込み、マイク推論、現行firmware統合、連続試験、OTA |

作業をこの順に直列化する。基板が届くまでに第2段階まで完了させ、実機作業では「書込みと物理I/Oの検証」だけを残す。

## 2. 作業可能条件による分類

### 2.1 基板もSTM32コンパイル環境も不要

| 作業 | 状態 | 必要物 |
|---|---|---|
| 既存モデルの出自調査 | 完了 | GitHub閲覧 |
| ST公式モデルの選定 | 完了 | GitHub閲覧 |
| モデルcommit、LFS SHA-256、size、license固定 | 完了 | GitHub閲覧 |
| 入出力・クラス・前処理仕様固定 | 完了 | 公式YAML/README |
| モデル完全性検証スクリプト作成 | 完了 | Python標準ライブラリ |
| Git LFSポインタ誤使用の自動検出 | 完了 | Python標準ライブラリ |
| デプロイYAMLテンプレート作成 | 完了 | テキストエディタ |
| モデル実体の取得 | 完了 | Git＋Git LFS＋インターネット |
| TFLite入出力の実測 | 完了 | TensorFlow 2.18.0 |
| 保存WAVによるPC推論 | 完了 | 公式前処理＋合成音声／非音声＋ST付属bus.wav |

### 2.2 基板は不要だがSTM32コンパイル／ST環境が必要

| 作業 | 必要環境 | 出力 |
|---|---|---|
| ST Edge AI `analyze` | Developer CloudまたはST Edge AI Core | operator、Flash/RAM見積り |
| B-U585I-IOT02A `benchmark` | Developer Cloud、myST認証 | ボード基準の推論時間・メモリ |
| STM32用Cコード生成 | Developer CloudまたはST Edge AI Core | model C/H、runtime、report |
| 公式AED application build | CubeIDE/GCC＋Model Zoo Services | ELF、bin、map、build log |
| 現行TrustZone project build | CubeIDE/GCC | Secure/NonSecure ELF・bin・map |

Developer Cloudの`benchmark`は物理基板なしで実行できる。ローカルでELF/binを作るにはCubeIDEまたは互換GCC toolchainが必要である。

### 2.3 基板がなければ実行できない

- ST-LINK接続確認とoption bytesの読出し
- firmware書込みとboot確認
- onboardデジタルマイクからの16 kHz PCM取得
- 実音によるSpeech/non-Speech判定
- UARTログと実測推論時間の取得
- DMAと推論の競合、HardFault、長時間安定性の確認
- 現行TrustZone Secure Gateway経由の音声取得確認
- OTA転送、再起動、rollback、電源断試験

静的な`analyze`やmap解析では、マイク配線、MDF1クロック、DMA coherency、実環境のSNR、実時間deadlineは証明できないため、第3段階が必須である。

## 3. 第1段階 — このチャットで実行する作業

### 3.1 実施済み

次のファイルを作成した。

```text
pc_side/ml_pretrained_test/
├── model_manifest.json
├── run_pc_inference.py
├── verify_model.py
├── test_verify_model.py
└── deployment_fsd50k_speech_u5.yaml
```

#### `model_manifest.json`

次を機械可読形式で固定した。

- ST Model Zoo commit
- モデルGit LFS SHA-256とsize
- Apache-2.0 license
- input/output shapeとdtype
- 6クラスの順序
- voice/non_voice写像
- 16 kHz mel-spectrogram前処理値

#### `verify_model.py`

次の機能を実装した。

1. モデルが存在するか検査
2. Git LFSポインタをモデル実体として誤使用していないか検査
3. sizeとSHA-256をmanifestと照合
4. オプションでTFLite input/output shape、dtype、quantizationを照合
5. 結果をJSON出力

完全性検査はPython標準ライブラリのみで動く。`--inspect-io`だけはTensorFlowまたは`tflite-runtime`を必要とする。

#### `deployment_fsd50k_speech_u5.yaml`

ST Model Zoo Servicesの公式U5 deployment設定を基に、FSD50K YamNet-256 Unknownありモデル用に次を固定した。

- `operation_mode: deployment`
- 5既知クラス＋garbage class
- 16 kHz、64 mel、96 frames
- B-U585I-IOT02A / STM32U5 / GCC
- `unknown_class_threshold: 0.0`
- Developer Cloud使用

### 3.2 この環境での実行結果（2026-08-23更新）

| 確認 | 結果 |
|---|---|
| Python | 3.12.13 |
| `verify_model.py` syntax check | PASS |
| 完全性検証unit test | 4/4 PASS |
| YAML parse・manifest整合性検査 | PASS |
| TensorFlow | 2.18.0を一時venvへ導入 |
| モデル実体 | 184,240 bytes、SHA-256一致、PASS |
| TFLite I/O | `[1,64,96,1]` int8 → `[1,6]` float32、PASS |
| PC推論 | 音声=Speech 98.62%、tone/noise=other 99.61%、bus=other 85.97% |
| ARM GCC | 未導入 |
| CubeIDE | 未導入 |
| CubeProgrammer | 未導入 |
| ST Edge AI | 未導入 |
| VS Codeローカル環境 | 未接続。現在はLinux chat sandboxで実行 |

モデル取得、I/O inspection、PC推論まで完了した。STコンパイルとfirmware buildはtoolchain/myST環境がなく、実機試験は基板未接続のため未実施とする。

### 3.3 第1段階の完了条件

- [x] 使用モデルと由来を確定
- [x] 不明な由来情報を不明と記録
- [x] hash/size/入出力/前処理/ラベルをmanifest化
- [x] モデル検証スクリプトを実装
- [x] 検証スクリプトのunit testを実行
- [x] U5 deployment設定テンプレートを作成
- [x] 第2・第3段階の作業境界を定義

## 4. 第2段階 — 基板なしでPC上で実行する作業

第2段階を2Aと2Bに分ける。2AはSTM32コンパイル環境なしで実行でき、2BはST/CubeIDE環境を必要とする。

### 第2A段階: モデル取得・PC検証

#### Step 2A-1: 固定commitを取得

```powershell
git lfs install
git clone https://github.com/STMicroelectronics/stm32ai-modelzoo.git
git -C stm32ai-modelzoo checkout 1423c78953a830903485135febe1dd98ff31aed8
git -C stm32ai-modelzoo lfs pull

git clone https://github.com/STMicroelectronics/stm32ai-modelzoo-services.git
git -C stm32ai-modelzoo-services checkout 0f6210ed5156126b782e1c43249063a477484b20
git -C stm32ai-modelzoo-services submodule update --init --recursive
```

リポジトリを同じ親ディレクトリに置き、パスに空白を含めない。

#### Step 2A-2: モデル完全性を検証

```powershell
$MODEL = "stm32ai-modelzoo/audio_event_detection/yamnet/ST_pretrainedmodel_public_dataset/fsd50k/yamnet_e256_64x96_tl/with_unknown_class/yamnet_e256_64x96_tl_int8.tflite"

python B-U585I-IOT02A/pc_side/ml_pretrained_test/verify_model.py $MODEL
```

合格出力:

```json
{
  "integrity": {
    "size_bytes": 184240,
    "sha256": "cd75689f072fac00d2a0fca063faec0ae0070a78d128d7f8d60c0f7ff88cd48d"
  },
  "status": "PASS"
}
```

FAILの場合は以降へ進まない。特に150 bytes前後のファイルはGit LFSポインタである。

#### Step 2A-3: Python環境を作成

固定したServices commitの`requirements.txt`を用いる。

```powershell
cd stm32ai-modelzoo-services
py -3.12 -m venv st_zoo
.\st_zoo\Scripts\Activate.ps1
python -m pip install --upgrade pip
python -m pip install -r requirements.txt
```

#### Step 2A-4: TFLite I/Oを実測

```powershell
python ..\B-U585I-IOT02A\pc_side\ml_pretrained_test\verify_model.py `
  $MODEL --inspect-io `
  --json-out test_results\model_verification.json
```

合格条件:

- input `[1,64,96,1]` / `int8`
- output `[1,6]` / `float32`
- integrity PASS

出力数が6でない場合、Unknownの位置やクラス順を推測しない。モデルYAMLと生成される`ai_model_config.h`を照合する。

#### Step 2A-5: 保存音声でPC推論

公式Model Zoo Servicesのprediction機能または同じ前処理コードを使い、次の16 kHz mono WAVを各3本以上実行する。

- speech: 通常の会話
- non-speech: 無音、ノック、空調または音楽

確認目的は精度評価ではなく、入力に応じてSpeech scoreが変わることと、ラベル順を確定することである。raw PCMをTFLiteへ直接入力しない。

第2A完了条件:

- [x] Git LFSモデル実体のhash/sizeが一致
- [x] TFLite input/output実測がmanifestと一致
- [x] 保存音声でSpeech scoreの変化を確認
- [x] 実行環境の主要パッケージ版を保存

### 第2B段階: ST変換・firmware build

必要環境:

- mySTアカウント
- ST Edge AI Developer CloudまたはST Edge AI Core 4.0.0
- STM32CubeIDE 1.17.0
- Model Zoo Servicesが要求するGCC/toolchain

#### Step 2B-1: deployment設定を配置

```powershell
Copy-Item `
  B-U585I-IOT02A\pc_side\ml_pretrained_test\deployment_fsd50k_speech_u5.yaml `
  stm32ai-modelzoo-services\audio_event_detection\configs\deployment_fsd50k_speech_u5.yaml

$env:STM32_MODEL_PATH = (Resolve-Path $MODEL)
```

YAML内の`path_to_cubeIDE`をPCの実パスへ変更する。Cloud利用時も、Servicesのconfig validationが要求する項目は削除しない。

#### Step 2B-2: analyze/benchmark

最初に`operation_mode`を`benchmarking`へ変更してB-U585I-IOT02Aの結果を保存し、その後`deployment`へ戻す。

暫定合格条件:

| 項目 | 上限 |
|---|---:|
| unsupported operator | 0 |
| total RAM | 140 KiB |
| total Flash | 210 KiB |
| inference | 400 ms |

同一アーキテクチャのST公開値はRAM 110.56 kB、Flash 167.1 kB、279.99 msである。ただしこれはESC-10版なので、FSD50K版の実測値で上書きする。

#### Step 2B-3: 公式applicationを生成

```powershell
cd stm32ai-modelzoo-services\audio_event_detection
python stm32ai_main.py `
  --config-path configs `
  --config-name deployment_fsd50k_speech_u5
```

モデルC/H、mel LUT、`ai_model_config.h`、runtime、生成reportを保存する。Core 4.0生成物と現行Core 2.2 runtimeを混在させない。

#### Step 2B-4: 基板を接続せずbuild

生成されたSTM32U5 applicationをCubeIDE/GCCでbuildし、次を保存する。

- ELF
- bin
- map
- build log
- 生成report
- model hashとST Edge AI Core版

合格条件:

- compile/link error 0
- linker region overflow 0
- 生成model input/outputがmanifestと一致
- map上のRAM/FlashがStep 2B-2の見積りと説明可能な範囲

この段階で基板が届けば、未解決のcompile errorを実機デバッグへ持ち込まず、先に第2Bを完了する。

## 5. 第3段階 — 基板を用いた実装

### Step 3-1: 現行状態を保存

1. 現行Secure/NonSecure binとmapを保存する。
2. STM32CubeProgrammerでoption bytesを読んで保存する。
3. 現行firmwareのboot、sensor、Wi-Fi、audio streamを確認する。
4. 復旧書込み手順を確認する。

公式サンプル実行のためだけにRDP、TZEN、TrustZone設定を変更しない。設定変更が必要なら公式application単独flashを中止し、Step 3-3の現行Secure統合へ進む。

### Step 3-2: 公式applicationをST-LINK実行

1. 第2Bで作ったfirmwareを書き込む。
2. UARTを公式設定で開く。初期候補は115200 baud / 8-N-1。
3. 無音、会話、ノック、グラス音、音楽・空調を各10回入力する。
4. 全6 score、top-1、前処理時間、推論時間を保存する。
5. 100回連続推論する。

PoC合格条件:

- 会話10回中8回以上で`Speech`
- 非会話条件の8割以上で`Speech`以外
- 前処理＋推論が約0.975秒の入力patch時間未満
- reset、HardFault、AI runtime errorが0

これは公開accuracy 73.9%のモデルに対する動作確認であり、製品精度保証ではない。

### Step 3-3: 現行TrustZone Secure側へ統合

1. 旧Core 2.2モデルとruntimeをbuild targetから外す。
2. 第2BのCore 4.0生成物、対応runtime、公式mel前処理/LUTを追加する。
3. MDF1/GPDMA音声を止めず、約0.975秒のsnapshotを作る。
4. 固定mel KAT、保存PCM KAT、マイク単発、連続推論の順に確認する。
5. 6クラスをmanifestどおり二値化する。

合格条件:

- PC/STM32のKAT top-1が全件一致
- 公式applicationとの二値判定一致率80%以上
- 1,000回でDMA停止、HardFault、AI errorが0
- map上でFlash/RAM overlapが0

### Step 3-4: リアルタイム化

初期値:

- patch: 96 frames、約0.975秒
- 更新周期: 500 ms
- voice開始: 直近3回中2回がSpeech
- voice終了: 直近3回中2回がSpeech以外

処理時間が周期を超えた場合、推論要求を蓄積せず古い窓を破棄する。音声DMAを推論完了待ちにしない。

### Step 3-5: NonSecure移設とOTA

1. AI、前処理、判定をNonSecureへ移す。
2. 音声取得はSecureに残し、Secure Gatewayからsnapshotを渡す。
3. Secure版と同じKAT・実音・1,000回試験を再実行する。
4. AIを含むNonSecure firmware全体を既存OTAで更新する。
5. AI初期化とKATをboot confirm条件へ加える。
6. 正常、hash不一致、転送中断、書込中電源断、10回連続OTAを試験する。

現行OTAはNonSecure更新経路なので、AIがSecureにある間はAI更新を完了したとは判定しない。モデル単体OTAは初期目標から除外する。

## 6. 次にユーザーPCで行う最小作業

基板到着前に、まず第2Aの次の3コマンドを完了する。

```powershell
git lfs pull
python pc_side/ml_pretrained_test/verify_model.py <MODEL_PATH>
python pc_side/ml_pretrained_test/verify_model.py <MODEL_PATH> --inspect-io
```

返してほしい結果:

1. `verify_model.py`のJSON出力
2. `python --version`
3. `pip freeze`またはTensorFlow/TFLite runtimeの版

これがPASSすれば、基板なしで第2BのST変換・buildへ進める。

## 7. 参考資料

- STMicroelectronics, [YamNet README](https://github.com/STMicroelectronics/stm32ai-modelzoo/blob/1423c78953a830903485135febe1dd98ff31aed8/audio_event_detection/yamnet/README.md)
- STMicroelectronics, [採用モデル](https://github.com/STMicroelectronics/stm32ai-modelzoo/blob/1423c78953a830903485135febe1dd98ff31aed8/audio_event_detection/yamnet/ST_pretrainedmodel_public_dataset/fsd50k/yamnet_e256_64x96_tl/with_unknown_class/yamnet_e256_64x96_tl_int8.tflite)
- STMicroelectronics, [採用モデル設定](https://github.com/STMicroelectronics/stm32ai-modelzoo/blob/1423c78953a830903485135febe1dd98ff31aed8/audio_event_detection/yamnet/ST_pretrainedmodel_public_dataset/fsd50k/yamnet_e256_64x96_tl/with_unknown_class/yamnet_e256_64x96_tl_config.yaml)
- STMicroelectronics, [Audio Event Detection deployment guide](https://github.com/STMicroelectronics/stm32ai-modelzoo-services/blob/0f6210ed5156126b782e1c43249063a477484b20/audio_event_detection/docs/README_DEPLOYMENT.md)
- STMicroelectronics, [U5 deployment example](https://github.com/STMicroelectronics/stm32ai-modelzoo-services/blob/0f6210ed5156126b782e1c43249063a477484b20/audio_event_detection/config_file_examples/deployment_u5_config.yaml)
- 対象リポジトリ内 `docs/ml/ST公式学習済み音声モデル_最小導入・実機試験計画.md`
- 対象リポジトリ内 `OTAテスト手順_2026-07-10.md`
- 対象リポジトリ内 `TrustZone リファクタリング計画.md`
