# ChatGPTからWindows PC上のCodexを実行する準備

- 対象: Android版ChatGPTからWindows PCを操作
- 対象プロジェクト: `Fly0KUBOKI/B-U585I-IOT02A`
- 使用機能: Codex Remote
- 作成日: 2026-08-23

## 1. 接続方式

ChatGPT mobileのRemoteは、PC上で動くChatGPTデスクトップアプリをホストとして使用する。接続後は、Android側からPC上のCodex chatを開始・継続し、指示、承認、差分、テスト結果、terminal出力を確認できる。

Remoteの提供状況はaccount・workspaceへのrolloutで異なる。Androidアプリに`Remote`、またはPCアプリに`Control this PC`が表示されない場合は、更新後も未提供かworkspace管理者により無効化されている可能性がある。

実際のshell commandはPC上で実行され、ファイル、Git credential、myST credential、STM32 toolchain、plugin、sandbox設定もPC側のものが使用される。Codex CLIまたはIDE extensionだけではRemoteの初期設定はできない。

## 2. PC側の準備

### Step 1: ChatGPT Windowsアプリを導入

```powershell
winget install --id 9PLM9XGG6VKS -s msstore
```

アプリを更新し、Android版ChatGPTと同じChatGPT account・workspaceへサインインする。

### Step 2: 開発ツールを確認

最低限、次をPCへ導入する。

- Git
- Git LFS
- Python 3.12
- GitHub CLI

確認:

```powershell
git --version
git lfs version
python --version
gh --version
gh auth status
```

Pythonは今回固定したModel Zoo Servicesに合わせて3.12を使用する。STM32CubeIDE、CubeProgrammer、ST Edge AIは第2B段階までに導入すればよい。

### Step 3: 対象リポジトリを取得

```powershell
git clone https://github.com/Fly0KUBOKI/B-U585I-IOT02A.git
cd B-U585I-IOT02A
git fetch origin feature/ml-voice-classification-test
git switch feature/ml-voice-classification-test
git pull --ff-only
```

ChatGPT WindowsアプリでCodexを選択し、`B-U585I-IOT02A`フォルダをprojectとして開く。権限は最初に`Ask for approval`を選び、project外への書込みや管理者操作を自動許可しない。

### Step 4: PCをRemote hostにする

PCのChatGPTアプリで次を開く。

```text
Settings
  → Connections
  → Control this PC
  → Set up / Add
```

remote accessを許可し、表示されたQR codeをAndroid端末で読み取る。同じaccount・workspaceであることを確認し、MFA、SSO、passkeyが要求された場合は完了する。

### Step 5: Androidから接続確認

Android版ChatGPTで`Remote`を開き、登録したPCを選択する。次を確認する。

- PC上のprojectが表示される
- Codex chatを開始できる
- command approvalがAndroidへ届く
- terminal出力とGit diffを確認できる

PCは起動、online、ChatGPTアプリ起動状態にする。Computer Useを使う作業ではWindows sessionをunlockした状態にする。

## 3. 最初にPC Codexへ渡す指示

```text
B-U585I-IOT02Aリポジトリの
feature/ml-voice-classification-testブランチで作業する。

docs/ml/学習済みAI試験_3段階実行計画.md と
pc_side/ml_pretrained_test/AGENTS.md を読み、
基板なしの第2A段階を実行する。

最初に環境とGitの状態をread-onlyで確認する。
その後、pc_side/ml_pretrained_test/setup_pc.ps1を
依存関係インストールなしで実行し、モデルのGit LFS取得、
SHA-256、size検証まで行う。

既存のdirty fileを変更しない。
認証情報、モデルbinary、venv、STのcloneをcommitしない。
失敗した項目は推測せず、command、exit code、errorを報告する。
```

完全性検証がPASSした後、次を指示する。

```text
setup_pc.ps1 -InstallModelZooDependencies を実行し、
TFLite input/outputを検証する。
結果をtest_results/ml_pretrained_smoke/pc_setupへ保存し、
実行したcommand、tool version、結果、blockerをreport.mdへまとめる。
まだ基板へのflash、option bytes変更、OTAは行わない。
```

## 4. セキュリティ条件

- Codex app serverをinternetへ直接公開しない。
- Remoteの認証済みrelayを使用する。
- SSH hostを追加する場合も公開app-server portを作らず、SSH keyとleast-privilege accountを使う。
- PCのfull accessは常用せず、project sandboxとapprovalを維持する。
- myST、GitHub、OpenAI tokenをrepository、prompt、logへ貼らない。
- installerや管理者権限が必要な操作はAndroid側で内容を確認してから承認する。

## 5. 接続後の作業分担

| 場所 | 担当 |
|---|---|
| このChatGPT chat | 調査、計画、GitHub資料管理、結果レビュー |
| PC Codex | Git LFS取得、Python環境構築、TFLite検証、ST変換、build |
| 実機接続後のPC Codex | ST-LINK、UART、マイク推論、TrustZone統合、OTA試験 |

## 6. 接続できない場合

1. PCとAndroidのChatGPTアプリを更新する。
2. 両方が同じaccount・workspaceか確認する。
3. PCのChatGPTアプリが起動中で、PCがsleepしていないか確認する。
4. `Settings > Connections`でRemote Controlを有効化する。
5. QR codeで再pairingする。
6. workspace利用時はadministratorにRemote Control許可を確認する。

## 7. 公式資料

- OpenAI, [Remote connections](https://learn.chatgpt.com/docs/remote-connections)
- OpenAI, [Codex Remote](https://learn.chatgpt.com/docs/remote)
- OpenAI, [ChatGPT desktop app for Windows](https://learn.chatgpt.com/docs/windows/windows-app)
- OpenAI, [Local environments](https://learn.chatgpt.com/docs/environments/local-environment)
- OpenAI, [Codex CLI](https://learn.chatgpt.com/docs/codex/cli)
