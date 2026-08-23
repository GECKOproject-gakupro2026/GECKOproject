# ST公式学習済み音声AI 基板なし実行・評価結果

- 実行日: 2026-08-23
- 対象: B-U585I-IOT02A / STM32U585
- 対象ブランチ基準: `feature/ml-voice-classification-test` / `e70325ac7048deedbe905b758cc6c859f06cc6f9`
- 基板: 未接続
- 実行場所: ChatGPT WorkのLinux sandbox（Windows PC／VS Codeではない）

## 結論

基板とSTM32 toolchainを必要としない最小AI試験は完了し、**PASS**と評価する。

1. ST公式モデル実体を固定commitからGit LFSで取得した。
2. sizeとSHA-256が公開LFS情報に一致した。
3. TFLite input/output、dtype、量子化係数を実測した。
4. ST公式Model Zoo Servicesと同じ前処理で保存WAVを推論した。
5. 合成音声を`Speech`、tone/noise/ST付属bus.wavを`other`と判定した。

これはPC上でモデル、前処理、量子化、ラベル写像が機能することの確認である。STM32向けC生成、firmware build、オンボードマイク、実機時間、OTAは未評価であり、STM32実装全体のPASSを意味しない。

## 読んだ文書

対象ブランチのMarkdown 17ファイルを列挙し、プロジェクト固有の計画、要件、開発記録、OTA、Wi-Fi、TrustZone、ML pipeline、monitor、BLE patch、Codex実行準備を確認した。3件のdriver/utility licenseも確認した。加えて添付IOCと回路図のマイク関連箇所を照合した。

重要な現行条件:

- 現行AIは合成`silence/tone/noise`用で、人声モデルではない。
- 現行AIはSecure側、既存OTA対象はNonSecure側である。
- MDF1/ADF1とマイクGPIOはIOC上Secure、既存実装ではMDF1音声取得をSecureに残している。
- 基板にはMP23DB01HPデジタルマイク2個が実装され、PB1/PF10とPE10/PE9へ接続されている。
- 大きいイメージの連続hot OTAには過去のHardFault課題があり、最初のAI確認はST-LINKを優先する。

## 固定した公式ソース

| 項目 | 値 |
|---|---|
| Model Zoo | `STMicroelectronics/stm32ai-modelzoo` |
| commit | `1423c78953a830903485135febe1dd98ff31aed8` |
| model | `yamnet_e256_64x96_tl_int8.tflite` |
| model size | 184,240 bytes |
| SHA-256 | `cd75689f072fac00d2a0fca063faec0ae0070a78d128d7f8d60c0f7ff88cd48d` |
| Model Zoo Services commit | `0f6210ed5156126b782e1c43249063a477484b20` |
| license | Apache-2.0 |

## ラベル順の訂正

既存manifestは設定YAMLの記載順を出力順としていたため誤りだった。ST公式の`parse_config.py`、dataset one-hot生成、`gen_h_file.py`はクラス名を学習時・ヘッダ生成時にソートする。正しい出力順は次である。

| index | label | 二値判定 |
|---:|---|---|
| 0 | `Crying_and_sobbing` | non_voice |
| 1 | `Glass` | non_voice |
| 2 | `Gunshot_and_gunfire` | non_voice |
| 3 | `Knock` | non_voice |
| 4 | `Speech` | voice |
| 5 | `other` | non_voice |

garbage classの内部名は`Unknown`ではなく`other`である。manifestとunit testを修正した。

## 実行環境

| 項目 | 実測 |
|---|---|
| OS | Linux x86_64 sandbox |
| Python | 3.12.13 |
| Git | 2.51.1 |
| Git LFS | 3.4.1 |
| TensorFlow | 2.18.0 |
| librosa | 0.10.2.post1 |
| NumPy | 2.0.2 |
| STM32CubeIDE | なし |
| ARM GCC | なし |
| CubeProgrammer | なし |
| `stedgeai` | なし |
| myST Developer Cloud接続 | なし |

TensorFlow起動時にCUDA plugin重複登録警告が出たが、CPU XNNPACK interpreterは生成され、すべての推論はexit code 0で完了した。

## 試験結果

### 1. 完全性

- size: 184,240 bytes、一致
- SHA-256: 一致
- Git LFS pointer誤使用: なし
- 判定: PASS

### 2. TFLite I/O

| 項目 | 実測 | 判定 |
|---|---|---|
| input shape | `[1,64,96,1]` | PASS |
| input dtype | `int8` | PASS |
| input quantization | scale `0.057150375097990036`, zero point `33` | PASS |
| output shape | `[1,6]` | PASS |
| output dtype | `float32` | PASS |

### 3. 保存WAV推論

前処理は16 kHz mono、silence removal、512 FFT、400-sample Hann、160-sample hop、64 mel、125–7500 Hz、HTK、振幅melの`log(mel + 1e-6)`、96-frame patch、24-frame overlapを使用した。

| 入力 | top-1 | top score | Speech score | 期待 | 判定 |
|---|---|---:|---:|---|---|
| FFmpeg flite合成音声 | Speech | 98.62% | 98.62% | voice | PASS |
| 1 kHz tone | other | 99.61% | 0.00% | non_voice | PASS |
| white noise | other | 99.61% | 0.00% | non_voice | PASS |
| ST付属`bus.wav` | other | 85.97% | 4.57% | non_voice | PASS |

4/4で期待する二値判定になった。合成音声1件と人工非音声を含むため、これは配線確認相当のsmoke testであり、公開accuracy 73.9%の再現や製品精度評価ではない。

### 4. 自動テスト

- `py_compile`: PASS
- `unittest`: 4/4 PASS
- 永続化した推論CLIによる再実行: PASS

## VS Code／Windowsローカル環境の確認

このセッションでは次を実測した。

- `D:\App\STM32CubeIDE\workspace\_2.1.1\B-U585I-IOT02A`は見えない。
- `/mnt/d/...`も存在しないためWSL経由でもない。
- `code`はVS Code本体ではなく、未導入を返すラッパーである。
- VS Code、WSL、PowerShell由来の環境変数とプロセスはない。
- PCローカル操作用toolはこの会話へ公開されていない。

結論: VS Code上のCodexはPCローカルで利用できるが、VS Code extensionを入れただけではこの既存chatへPC filesystem/shellが接続されない。PCで同じブランチを開き、VS CodeのCodex sidebarから作業するか、ChatGPT desktop appのRemoteでPC hostをこのchatから選択する必要がある。

## 未実施と理由

| 作業 | 状態 | 理由 |
|---|---|---|
| ST Edge AI analyze | BLOCKED | `stedgeai`またはDeveloper Cloud/myST接続なし |
| B-U585I benchmark | BLOCKED | Developer Cloud/myST接続なし |
| STM32 C生成 | BLOCKED | 同上 |
| 公式U5 application build | BLOCKED | CubeIDE/ARM GCCなし |
| 現行TrustZone build | BLOCKED | Windows workspaceへ未接続、CubeIDE/ARM GCCなし |
| flash/マイク/実時間評価 | NOT RUN | 基板未接続 |
| OTA | NOT RUN | 基板未接続。初回成功を優先するため後段 |

## 次の最小実行

Windows PCの対象workspaceで、まずブランチを更新する。

```powershell
git fetch origin feature/ml-voice-classification-test
git switch feature/ml-voice-classification-test
git pull --ff-only
python -m unittest discover -s pc_side/ml_pretrained_test -p "test_*.py" -v
```

次に`setup_pc.ps1`で固定モデルを取得・検証し、保存WAVで`run_pc_inference.py`を再実行する。その後だけST Edge AI analyze/generateと未書込みbuildへ進む。基板接続後はST-LINKで公式applicationを最初に実行し、オンボードマイクでSpeech/non-Speechを評価する。OTA統合はその成功後に行う。

## 参照

- [STM32 Model Zoo YamNet README](https://github.com/STMicroelectronics/stm32ai-modelzoo/blob/1423c78953a830903485135febe1dd98ff31aed8/audio_event_detection/yamnet/README.md)
- [採用TFLiteモデル](https://github.com/STMicroelectronics/stm32ai-modelzoo/blob/1423c78953a830903485135febe1dd98ff31aed8/audio_event_detection/yamnet/ST_pretrainedmodel_public_dataset/fsd50k/yamnet_e256_64x96_tl/with_unknown_class/yamnet_e256_64x96_tl_int8.tflite)
- [採用モデル設定](https://github.com/STMicroelectronics/stm32ai-modelzoo/blob/1423c78953a830903485135febe1dd98ff31aed8/audio_event_detection/yamnet/ST_pretrainedmodel_public_dataset/fsd50k/yamnet_e256_64x96_tl/with_unknown_class/yamnet_e256_64x96_tl_config.yaml)
- [Model Zoo Services U5 deployment](https://github.com/STMicroelectronics/stm32ai-modelzoo-services/blob/0f6210ed5156126b782e1c43249063a477484b20/audio_event_detection/docs/README_DEPLOYMENT.md)
- [Codex IDE extension](https://learn.chatgpt.com/docs/codex/ide)
- [Remote connections](https://learn.chatgpt.com/docs/remote-connections)
