# ST公式学習済み音声モデルによる最小AI実機試験計画

- 対象: B-U585I-IOT02A / STM32U585
- 目的: 再学習を行わず、既存のST公式学習済みモデルで「話し声か、それ以外か」を実機確認する
- 対象リポジトリ: [Fly0KUBOKI/B-U585I-IOT02A](https://github.com/Fly0KUBOKI/B-U585I-IOT02A)
- 作業ブランチ: `feature/ml-voice-classification-test`
- 調査日: 2026-08-23

## 1. 結論

最短経路は、ST公式の次のモデルと公式B-U585I-IOT02A用Audio Event Detectionアプリをそのまま組み合わせ、最初はST-LINKで書き込む方法である。

- モデル: `yamnet_e256_64x96_tl_int8.tflite`
- 学習データ: FSD50Kの部分集合
- 出力: `Speech`、`Gunshot_and_gunfire`、`Crying_and_sobbing`、`Knock`、`Glass`、および学習時に追加されたUnknown（garbage）クラス
- 二値化: `Speech`だけを`voice`、その他を`non_voice`
- 入力: 16 kHzモノラル音声から生成する`64 x 96 x 1`のmel-spectrogram
- 対象: STがB-U585I-IOT02Aを正式なデプロイ対象にしている

この試験では、データ収集、ラベル付け、学習、再量子化を行わない。公式アプリでモデル・前処理・マイク・推論を一体で確認した後にだけ、現行TrustZoneファームウェアへ移植する。

## 2. 調査結果

### 2.1 現行リポジトリ内モデルの出自

現行の`model.keras`、`model.tflite`、`Secure/Core/AI/audio_net*`は、commit `54107280ba5de39b162b8fd9dcf01ecb7cf39791`で追加された。

`pc_side/ml_pipeline/train.py`の既定動作では、実データセットがない場合に次の合成波形を作り、3クラス分類器を学習する。

| クラス | 生成内容 |
|---|---|
| `silence` | 小さいガウス雑音 |
| `tone` | 200～3000 Hzの正弦波と雑音 |
| `noise` | 大きいガウス雑音 |

確認できたモデル仕様は次のとおり。

| 項目 | 値 |
|---|---|
| sample rate | 16 kHz |
| input | float32、2048 samples（128 ms） |
| output | `silence` / `tone` / `noise` |
| weights | 16.48 KiB |
| activation RAM | 16.03 KiB |
| MACC | 254,864 |
| 生成ツール | ST Edge AI Core 2.2.0-20266 |
| model hash | `0x0749ea096dfb04ebb065e4858a427872` |

したがって、このモデルはST Model Zoo由来ではなく、このリポジトリ内のスクリプトで作られた配線・推論経路確認用モデルである。人声／非人声を学習していないため、今回の判定には使わない。

学習を実行した人物、Python/TensorFlowの厳密な版、合成データ以外を与えたかどうかを確定できる記録は見つからなかった。これらの出自は**不明**とする。実装判断には不要なので、これ以上の追跡を前提にしない。

### 2.2 採用するST公式モデル

採用候補はSTM32 Model Zooの次のGit LFSファイルである。

[FSD50K YamNet-256 int8（Unknownクラス付き）](https://github.com/STMicroelectronics/stm32ai-modelzoo/blob/1423c78953a830903485135febe1dd98ff31aed8/audio_event_detection/yamnet/ST_pretrainedmodel_public_dataset/fsd50k/yamnet_e256_64x96_tl/with_unknown_class/yamnet_e256_64x96_tl_int8.tflite)

再現性のため、次を固定する。

| 項目 | 値 |
|---|---|
| Model Zoo commit | `1423c78953a830903485135febe1dd98ff31aed8` |
| LFS SHA-256 | `cd75689f072fac00d2a0fca063faec0ae0070a78d128d7f8d60c0f7ff88cd48d` |
| LFS size | 184,240 bytes |
| ライセンス | Apache-2.0 |
| フレームワーク | TensorFlow Lite |
| 量子化 | int8 input / float output |
| 公開clip-level accuracy | 73.9% |

GitHub上の184,240 bytesはモデル実体のサイズである。通常のHTTP表示やGit LFS未導入のcheckoutでは、SHAとsizeだけを書いたポインターファイルになるため、必ずGit LFSで取得してSHA-256を検証する。

### 2.3 モデルの由来

STのREADMEは、元のGoogle YAMNetをAudioSet事前学習モデルとして示している。STはMCU向けにembeddingを1024から256へ縮小し、FSD50Kの次の5クラスへ転移学習している。

1. Speech
2. Gunshots and gunfire
3. Crying and sobbing
4. Knock
5. Glass

採用版は、FSD50Kのその他多数の音をまとめたUnknown（garbage）クラスも学習している。STの公開値ではUnknownなしが87.0%、ありは73.9%だが、STはUnknownありの方がデバイス上のオンライン動作を実用上改善すると説明している。任意の環境音を5クラスのどれかへ強制分類しないため、今回はこちらを採用する。

STがYamNet-256を再学習した正確な実行日時、使用GPU、全依存パッケージの完全なlock、学習run IDは公開ファイルから確定できなかったため**不明**とする。一方、モデル、学習設定YAML、データセット名、クラス、前処理、量子化方法、Git LFSハッシュ、ライセンスは確認できるため、今回の再現性には十分である。

### 2.4 入力前処理

モデルと一緒に公開された設定は次の値を指定している。

| 項目 | 値 |
|---|---:|
| sample rate | 16,000 Hz |
| FFT | 512 |
| window | Hann、400 samples（25 ms） |
| hop | 160 samples（10 ms） |
| mel bands | 64 |
| frequency range | 125～7500 Hz |
| patch | 96 frames |
| input shape | `(64, 96, 1)` |
| FFT center padding | 無効 |
| spectrum power | 1.0 |
| HTK mel | 有効 |
| dB変換 | 無効 |

この前処理はモデルの一部ではない。raw PCMを直接TFLite入力へ渡しても正しく動かない。STの公式STM32アプリが生成するmel filter LUTと前処理コードを使用し、独自実装は初回試験に持ち込まない。

### 2.5 B-U585I-IOT02A上の既知性能

STが同一YamNet-256アーキテクチャのESC-10版について公開しているB-U585I-IOT02A / 160 MHz / ST Edge AI Core 4.0.0の値は次のとおり。

| 項目 | 公開値 |
|---|---:|
| activation RAM | 109.57 kB |
| runtime RAM | 0.99 kB |
| total RAM | 110.56 kB |
| weights Flash | 135.91 kB |
| code Flash | 31.19 kB |
| total Flash | 167.1 kB |
| inference | 279.99 ms |

STはFSD50K版を「わずかに小さく高速」としているが、FSD50K版のB-U585I-IOT02A上の個別数値は掲載していない。したがって、上表は容量見積りにだけ使い、採用モデルの正確なFlash、RAM、処理時間はDeveloper Cloudの`analyze`/`benchmark`と最終mapファイルで測る。

### 2.6 他候補を採用しない理由

| 候補 | 不採用理由 |
|---|---|
| 現行3クラス1D-CNN | Speechを学習していない |
| ESC-10 YamNet-256 | 環境音10クラスで、汎用Speechクラスがない |
| ESC-10 MiniResNetV2 | より軽いが、汎用Speechクラスがない |
| FSD50K Unknownなし | 無関係音を既知5クラスへ強制しやすい |
| Google元YAMNet | 約3.2M parametersでU5には大きく、前処理custom layerも直接C変換できない |

## 3. 判定仕様

最初の仕様では「人間の声」を**話し声（Speech）**と定義する。

```text
voice     = Speech
non_voice = Gunshot_and_gunfire
          | Crying_and_sobbing
          | Knock
          | Glass
          | Unknown
```

泣き声も人声へ含めたい場合は、モデルを変えずに`Speech OR Crying_and_sobbing`を`voice`へ写像できる。ただし最初の試験中は定義を変更しない。歌声、ささやき、テレビ越しの会話に対する保証は公開5クラス仕様からは導けないため、観察項目として記録する。

## 4. 実装戦略

### Phase A: 公式アプリだけでAI動作を確認する

現行ファームウェアへ手を入れる前に、STの公式B-U585I-IOT02Aアプリ、公式前処理、公式モデルの組合せをビルドしてST-LINKで実行する。これが最小作業であり、モデル変換とTrustZone統合の問題を分離できる。

### Phase B: 現行Secureプロジェクトへ移植する

Phase A成功後、生成コードと公式前処理を現行のSecure側AI経路へ入れる。現行モデルはraw PCM 2048点、float32入力、3出力、Core 2.2であり、採用モデルはmel 64x96、int8入力、6出力、Core 4.0である。重みファイルだけを差し替えることはできない。

### Phase C: NonSecure化してOTAする

現行OTAはNonSecureイメージを更新する設計である。AIがSecure側にある間、モデルを含むAI全体を既存OTAだけで更新できない。ST-LINKでの実行成功後にAIをNonSecureへ移し、最後にNonSecureファームウェア全体のOTA試験を行う。

## 5. Step-by-step計画

### Step 0: 現行状態を保存する

1. `feature/ml-voice-classification-test`のHEADを記録する。
2. 現行Secure/NonSecureをクリーンビルドし、ELF、bin、map、起動ログを保存する。
3. STM32CubeProgrammerでoption bytesとTrustZone設定を読み出して記録する。
4. 現行の音声取得を開始し、無音と発声でRMS/peakが変わることを確認する。

完了条件:

- 現行ファームウェアへ戻せるbinと書込手順がある。
- マイク、クロック、MDF1、GPDMAの基準動作が確認できる。

禁止事項:

- 公式サンプルのためだけにRDP、TZEN、TrustZone option bytesを変更しない。
- 変更が要求された場合はPhase Aを中止し、Phase Bへ進む。

### Step 1: 公式ソースとモデルを固定取得する

```bash
git clone https://github.com/STMicroelectronics/stm32ai-modelzoo-services.git
cd stm32ai-modelzoo-services
git checkout 0f6210ed5156126b782e1c43249063a477484b20
git submodule update --init --recursive

cd ..
git lfs install
git clone https://github.com/STMicroelectronics/stm32ai-modelzoo.git
cd stm32ai-modelzoo
git checkout 1423c78953a830903485135febe1dd98ff31aed8
git lfs pull
```

モデル実体を検査する。

```bash
sha256sum audio_event_detection/yamnet/ST_pretrainedmodel_public_dataset/fsd50k/yamnet_e256_64x96_tl/with_unknown_class/yamnet_e256_64x96_tl_int8.tflite
wc -c audio_event_detection/yamnet/ST_pretrainedmodel_public_dataset/fsd50k/yamnet_e256_64x96_tl/with_unknown_class/yamnet_e256_64x96_tl_int8.tflite
```

完了条件:

- SHA-256が`cd75689f...cd48d`と完全一致する。
- sizeが184,240 bytesである。
- ファイル先頭が`version https://git-lfs...`ではない。

### Step 2: PC環境を作る

固定したModel Zoo ServicesのREADMEに従い、空白を含まないパスへPython 3.12.9環境を作る。

```bash
cd stm32ai-modelzoo-services
python -m venv st_zoo

# Windows
st_zoo\Scripts\activate

python -m pip install --upgrade pip
python -m pip install -r requirements.txt
```

使用版を`test_results/tool_versions.txt`へ記録する。

```bash
python --version
python -m pip freeze
```

追加で必要なもの:

- STM32CubeIDE 1.17.0（公式AEDガイドの基準版）
- STM32CubeProgrammer 2.18.0（公式アプリREADMEの基準版）
- mySTアカウントとST Edge AI Developer Cloud、またはローカルST Edge AI Core

生成Cコードとruntime libraryは必ず同じST Edge AI Core版を使う。今回の推奨は固定したServices releaseが対応するCore 4.0.0である。現行リポジトリのCore 2.2生成物や`NetworkRuntime1020_CM33_GCC.a`と混在させない。

### Step 3: デプロイYAMLを作る

`audio_event_detection/config_file_examples/deployment_u5_config.yaml`をコピーし、現在の公式ファイルを基準に次だけを置き換える。

```yaml
operation_mode: deployment

model:
  model_path: <absolute-or-config-relative-path>/yamnet_e256_64x96_tl_int8.tflite

dataset:
  dataset_name: fsd50k
  class_names: ['Speech', 'Gunshot_and_gunfire', 'Crying_and_sobbing', 'Knock', 'Glass']
  multi_label: False
  use_garbage_class: True
  expand_last_dim: True

preprocessing:
  target_rate: 16000

feature_extraction:
  patch_length: 96
  n_mels: 64
  n_fft: 512
  hop_length: 160
  window_length: 400
  window: hann
  center: False
  power: 1.0
  fmin: 125
  fmax: 7500
  norm: None
  htk: True
  to_db: False

tools:
  stedgeai:
    optimization: balanced
    on_cloud: True

deployment:
  c_project_path: ../application_code/sensing/STM32U5
  IDE: GCC
  hardware_setup:
    serie: STM32U5
    board: B-U585I-IOT02A
  unknown_class_threshold: 0.0
  build_conf: debug
```

注意:

- `use_garbage_class: True`と`unknown_class_threshold`によるOOD判定は排他的なので、thresholdは`0.0`のままにする。
- 完全なYAMLは、モデル付属の学習YAMLではなく、固定commitの最新`deployment_u5_config.yaml`を土台にする。
- datasetの音声パスはdeploymentだけなら不要である。
- クラス順、前処理値を変更しない。

### Step 4: PC上でモデルI/Oを検査する

学習は実行せず、TensorFlow Lite interpreterで次を確認し、JSONへ保存する。

- input shapeが`[1, 64, 96, 1]`
- input dtypeが`int8`
- output dtypeが`float32`
- output要素数が6であること
- input/outputのquantization parameters
- モデルSHA-256

出力数やdtypeが想定と異なる場合、ラベル順を推測せず作業を停止し、生成された`ai_model_config.h`とモデル設定を照合する。

### Step 5: ST解析とボードbenchmarkを行う

Developer Cloudで採用モデルに対して`analyze`とB-U585I-IOT02A向け`benchmark`を行う。Model Zoo Servicesの`benchmarking`モードを使ってもよい。

合格条件:

| 項目 | 合格基準 |
|---|---:|
| unsupported operator | 0 |
| total RAM | 140 KiB以下 |
| total Flash | 210 KiB以下 |
| inference | 400 ms以下 |

基準は同一アーキテクチャの公開値に余裕を加えたスモークテスト値であり、製品要求ではない。レポート、Core版、モデルhashを保存する。

### Step 6: 公式アプリを生成・ビルド・書き込む

`audio_event_detection`ディレクトリで、作成した設定を指定する。

```bash
python stm32ai_main.py \
  --config-path <config-directory> \
  --config-name deployment_fsd50k_speech_u5
```

この処理で次を一式生成・更新する。

- モデルCコード
- 対応するST Edge AI runtime
- `ai_model_config.h`
- mel filter LUTと前処理設定
- B-U585I-IOT02A用アプリ

自動flashに失敗した場合は、同じ生成プロジェクトをCubeIDEでビルドし、ST-LINKで書き込む。Secure/NonSecureのoption bytes変更を要求された場合は書込みを中止する。

UARTは公式アプリREADMEの設定を確認し、初期値として115200 baud / 8-N-1を使う。

### Step 7: 公式アプリで実機スモークテストする

ボード前方0.5～1 mから、各条件を10回ずつ約2秒与える。毎回、top-1ラベル、Speech score、全出力、前処理時間、推論時間をCSVへ保存する。

| 試験 | 入力 | 期待 |
|---|---|---|
| T1 | 静かな室内 | Speechになり続けない |
| T2 | 通常の話し声 | Speechが上昇し、top-1になる |
| T3 | 机または扉をノック | Knockまたはnon_voice |
| T4 | グラスを軽く鳴らす録音 | Glassまたはnon_voice |
| T5 | 音楽、空調、拍手 | Unknownまたはnon_voice |
| T6 | 100連続推論 | reset、HardFault、AI errorなし |

PoC合格条件:

- T2で10回中8回以上`voice`
- T1、T3～T5を合わせて10条件中8条件以上`non_voice`
- 前処理と推論の合計が次の約0.96～0.98秒patch取得時間未満
- 100回連続でruntime errorとresetが0

公開accuracyは73.9%であり、この10回試験は精度保証ではない。ここでは、マイク入力が正しく特徴量化され、分類結果が音に応じて変わることだけを確認する。

### Step 8: 現行TrustZoneファームウェアへ統合する

Phase Aが成功した場合だけ実施する。

1. 生成されたCore 4.0モデルコードと対応runtimeを`Secure/Core/AI`へ追加する。
2. 現行Core 2.2のモデルコードとruntimeを同じtargetから外す。
3. 公式アプリのmel前処理、LUT、`ai_model_config.h`を移植する。
4. 現行MDF1/GPDMAの16 kHz PCMから、公式実装と同じring buffer/patchを作る。
5. `Telemetry_GetAudioBuffer`または既存Secure内バッファからsnapshotを取り、DMA書込領域との競合を防ぐ。
6. 出力6要素を生成configの順序で読み、二値化する。
7. UART/TCPへmodel ID、各score、voice/non_voice、前処理時間、推論時間、errorを出す。

実装順:

```text
固定mel特徴KAT
  → 保存PCMの前処理KAT
  → マイク単発推論
  → 連続推論
```

合格条件:

- 固定特徴入力でPCとSTM32のtop-1が全件一致する。
- 保存PCMから作るmelの量子化値が公式PC処理と一致、または事前に定めた誤差内である。
- Phase Aと同じ音源で二値判定が8割以上一致する。
- mapファイルでFlash/RAMにoverlapがない。
- 1,000回連続推論でHardFault、DMA停止、AI runtime errorが0。

### Step 9: リアルタイム判定を最小追加する

最初は1 patchごとの即時表示でよい。連続表示が不安定な場合だけ次を追加する。

```text
voice開始: 直近3回のうち2回がSpeech
voice終了: 直近3回のうち2回がSpeech以外
推論周期: 実測処理時間より長くし、最初は500 ms
```

推論が周期に間に合わない場合はキューを積まず、古い窓を捨てて最新snapshotを使う。DMA音声取得を推論待ちにしない。

### Step 10: NonSecure移設後にOTAする

ST-LINKで安定動作した後だけ実施する。

1. モデル、runtime、前処理、判定をNonSecureへ移す。
2. 音声取得はSecureに残し、既存Secure GatewayからPCM snapshotを得る。
3. Secure版と同じKAT、実音試験、1,000回試験を行う。
4. AI一式を含むNonSecure firmware binを生成する。
5. 既存OTAでBank2 NonSecureへ配布する。
6. 書込前にload address、vector table、size、SHA-256を検査する。
7. 再起動後、AI初期化とKAT成功をboot confirm条件へ加える。
8. 正常更新、破損bin、転送中断、書込中電源断、10回連続更新を試験する。

AIをSecureに残したまま、既存NonSecure OTAでAIが更新されるとはみなさない。モデル単体OTAも、生成コード/runtime ABIを固定できるまで実装しない。

## 6. 成果物

```text
docs/ml/
└── ST公式学習済み音声モデル_最小導入・実機試験計画.md

test_results/ml_pretrained_smoke/<date>/
├── source_versions.txt
├── tool_versions.txt
├── model_manifest.json
├── stedgeai_analyze.*
├── stedgeai_benchmark.*
├── build.log
├── firmware.map
├── uart.csv
└── test_report.md
```

大きいモデル、生成中間物、認証情報、個人の音声はGitへcommitしない。モデルは取得元commit、LFS SHA-256、sizeで参照する。生成Cコードをcommitする場合は、model hash、Core版、生成コマンドを同じcommitへ含める。

## 7. 実施チェックリスト

- [ ] Step 0: 現行firmware、option bytes、音声取得を保存
- [ ] Step 1: 公式2リポジトリを固定commitで取得
- [ ] Step 1: LFSモデルのSHA-256とsizeを確認
- [ ] Step 2: Python/STM32/ST Edge AI環境を固定
- [ ] Step 3: FSD50K Unknownあり用deployment YAMLを作成
- [ ] Step 4: TFLite input/output/dtype/quantizationを確認
- [ ] Step 5: Cloud analyze/benchmarkに合格
- [ ] Step 6: 公式B-U585I-IOT02AアプリをST-LINK実行
- [ ] Step 7: Speech/non-Speechスモークテストに合格
- [ ] Step 8: 現行Secure firmwareへ統合しKAT/連続試験
- [ ] Step 9: 必要な場合だけ平滑化を追加
- [ ] Step 10: NonSecure移設後にOTA試験

## 8. 停止条件と次の判断

| 状況 | 判断 |
|---|---|
| モデルhash/size不一致 | LFS取得をやり直し、推論しない |
| 公式アプリがAI initに失敗 | Core版、生成コード、runtimeの組合せを修正 |
| 公式アプリでSpeech scoreが全く変わらない | PCM、sample rate、mel前処理を先に調査 |
| 公式アプリは成功し現行統合だけ失敗 | TrustZone、linker、DMA snapshotの差分を調査 |
| Flash/RAM不足 | mapを確認し、同時保持する旧runtime/旧modelを除去 |
| 精度だけ不足 | 統合成功とは分離して記録し、次段階で再学習を検討 |
| OTAだけ失敗 | AIモデルを変更せず既存OTA経路を単独調査 |

最初の完了点は**Step 7**である。ここで「公式学習済みAIがB-U585I-IOT02Aのマイク入力に対して動作する」ことを証明できる。Step 8以降は製品側ファームウェアへ統合する別作業として扱う。

## 9. 参考資料

- STMicroelectronics, [YamNet audio event detection README](https://github.com/STMicroelectronics/stm32ai-modelzoo/blob/1423c78953a830903485135febe1dd98ff31aed8/audio_event_detection/yamnet/README.md)
- STMicroelectronics, [採用モデル設定YAML](https://github.com/STMicroelectronics/stm32ai-modelzoo/blob/1423c78953a830903485135febe1dd98ff31aed8/audio_event_detection/yamnet/ST_pretrainedmodel_public_dataset/fsd50k/yamnet_e256_64x96_tl/with_unknown_class/yamnet_e256_64x96_tl_config.yaml)
- STMicroelectronics, [Audio Event Detectionライセンス一覧](https://github.com/STMicroelectronics/stm32ai-modelzoo/blob/1423c78953a830903485135febe1dd98ff31aed8/audio_event_detection/LICENSE.md)
- STMicroelectronics, [STM32 Model Zoo Services](https://github.com/STMicroelectronics/stm32ai-modelzoo-services/tree/0f6210ed5156126b782e1c43249063a477484b20)
- STMicroelectronics, [Audio Event Detection deployment guide](https://github.com/STMicroelectronics/stm32ai-modelzoo-services/blob/0f6210ed5156126b782e1c43249063a477484b20/audio_event_detection/docs/README_DEPLOYMENT.md)
- STMicroelectronics, [B-U585I-IOT02A AED application](https://github.com/STMicroelectronics/stm32ai-modelzoo-services/tree/0f6210ed5156126b782e1c43249063a477484b20/application_code/sensing/STM32U5)
- STMicroelectronics, [deployment_u5_config.yaml](https://github.com/STMicroelectronics/stm32ai-modelzoo-services/blob/0f6210ed5156126b782e1c43249063a477484b20/audio_event_detection/config_file_examples/deployment_u5_config.yaml)
- Google, [YAMNet](https://tfhub.dev/google/yamnet/1)
- FSD50K, [dataset repository](https://github.com/eduardofv/FSD50K)
- 対象リポジトリ内 `pc_side/ml_pipeline/train.py`
- 対象リポジトリ内 `pc_side/ml_pipeline/model_meta.json`
- 対象リポジトリ内 `pc_side/ml_pipeline/audio_net_generate_report.txt`
- 対象リポジトリ内 `Secure/Core/Src/ai_app.cpp`
- 対象リポジトリ内 `OTAテスト手順_2026-07-10.md`
- 対象リポジトリ内 `TrustZone リファクタリング計画.md`
