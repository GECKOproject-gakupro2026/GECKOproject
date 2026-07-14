# Step 0: ベースライン確立

## 目的
リファクタリング前の「動く状態」をgitタグで固定し、以降の全ステップのロールバック先と、回帰検証の基準を確立する。

## 前提条件
- `00_全体像と共通ルール.md` を読み終えていること
- 作業ツリーがクリーンであること（`git status` で変更なし。docs/refactoring/ の追加分は除く）
- ボードがPCに接続され、COM9として見えていること

## このステップで行うこと
**コードは1行も変更しない。** git操作と、現状が正常に動くことの確認だけを行う。

---

## 手順

### 1. ブランチとタグを作る

```bash
cd "d:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A"

# 現在のコミットを確認（3c676b5 以降であること）
git log --oneline -1

# ベースラインタグを打つ（ロールバック先）
git tag refactor-baseline

# 作業ブランチを作って切り替える
git checkout -b refactor/layering
```

**期待結果**: `Switched to a new branch 'refactor/layering'`

### 2. 現状をビルドして0エラーを確認する

`<HWS>` は任意の空ディレクトリ（例: `C:/temp/hws_refactor`）。まだ無ければ作られる。

```bash
CUBEIDEC="d:/App/ST/STM32CubeIDE_2.1.1/STM32CubeIDE/stm32cubeidec.exe"
HWS="C:/temp/hws_refactor"
REPO="d:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A"

"$CUBEIDEC" --launcher.suppressErrors -nosplash \
  -application org.eclipse.cdt.managedbuilder.core.headlessbuild \
  -data "$HWS" -importAll "$REPO" \
  -build "B-U585I-IOT02A_Secure/Debug" -build "B-U585I-IOT02A_NonSecure/Debug"
```

**期待結果**:
- Secure: `Build Finished. 0 errors, 2 warnings`（warningは `g_AudioEvents` / `g_AudioErrors` の extern初期化警告。これは既存で問題ない）
- NonSecure: `Build Finished. 0 errors, 0 warnings`

**ここで0エラーにならなければ、リファクタリングを始めてはいけない。** 環境の問題なので先に解決すること。

### 3. ボードに書き込んでリセットする

```bash
CLI="d:/App/ST/STM32CubeIDE_2.1.1/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_2.2.400.202601091506/tools/bin/STM32_Programmer_CLI.exe"
REPO="d:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A"

"$CLI" -c port=SWD mode=UR -d "$REPO/Secure/Debug/B-U585I-IOT02A_Secure.elf" -v
"$CLI" -c port=SWD mode=UR -d "$REPO/NonSecure/Debug/B-U585I-IOT02A_NonSecure.elf" -v
"$CLI" -c port=SWD mode=UR -rst
```

**期待結果**: 両方とも `Download verified successfully`、最後に `Software reset is performed`

### 4. 起動を待ってから回帰検証を実行する

**リセット後12秒以上待つこと**（Wi-Fi/BLE初期化中はテレメトリが流れないため）。

```bash
sleep 14
cd "d:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A"
python docs/refactoring/verify_regression.py
```

**期待結果**: 最終行が `=== PASS: 全項目OK。コミットしてよい ===`

検証される6項目:
1. ACTIVE維持（5秒で150フレーム以上）
2. ToF生存（tof_mmが2種類以上変動する = センサーが生きている）
3. 温度センサー（10〜60℃の妥当値）
4. IDLE遷移（無通信でフレームが止まる）
5. IDLE復帰（keep-alive再開で復帰）
6. 音声ストリーミング（`'a'`でCMD_AUDIO受信、`'s'`で停止）
7. OTA照会（STATUS_REQ→STATUS_RESP）

**FAILが出た場合**: リファクタリングを始める前に既に壊れている。原因を調査すること。ハード的な問題（ボード未接続、COM番号違い）の可能性もある。

### 5. 指示書一式をコミットする

```bash
cd "d:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A"
git add docs/refactoring/
git status --short   # docs/refactoring/ 配下のみが A（追加）になっていることを確認
git commit -m "docs: リファクタリング指示書とベースライン回帰検証スクリプトを追加"
```

---

## 完了条件（すべて満たすこと）

- [ ] `git tag refactor-baseline` が打たれている（`git tag` で確認できる）
- [ ] ブランチ `refactor/layering` にいる（`git branch --show-current`）
- [ ] Secure/NonSecure両方がビルド0エラー
- [ ] `verify_regression.py` が **PASS**
- [ ] 指示書一式がコミット済み

## ロールバック
このステップは破壊的変更をしないので、ロールバックは不要。
やり直す場合は `git tag -d refactor-baseline && git checkout master && git branch -D refactor/layering`。

## 次のステップ
`step1_NS基板IO分離.md` へ進む。
