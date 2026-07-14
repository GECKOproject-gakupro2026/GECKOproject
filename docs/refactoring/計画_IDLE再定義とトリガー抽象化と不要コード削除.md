# 実装計画: IDLE再定義・トリガー抽象化・不要コード削除

> 作成日: 2026-07-14 / ブランチ: `refactor/layering`
> **この文書は自己完結した作業指示書である。** 軽量AIモデルが1ステップずつ読んで実行できるよう、
> 共通コマンド・ガードレール・アーキテクチャ・検証手順をすべて本文に含めてある。
> 各フェーズ(P0〜P3)の独立性・検証可能性を重視して分割してある。実行時は **P0から順に、1フェーズずつ** 実施すること。
>
> **前提の完了済み作業**: telemetry.cpp(1417行)を機能単位に分割しport層/コア層/契約層/アダプタ層へ再編する
> レイヤ分離リファクタリング(旧step0〜step8)は **完了済み**(master相当、実機回帰PASS)。本計画はその上に乗る次の作業。
> 完了済みリファクタリングの詳細記録が必要な場合は git log(`refactor(step1)`〜`refactor(step7)`)を参照。

---

## Context（なぜこの変更をするか）

ユーザーは自作基板への移行を見据え、以下4点を要求している。

1. **IDLE状態の定義を修正**（最優先）。現状のIDLEは「センサーも通信も全部止める」設計だが、これが既知の「IDLE復帰の間欠的不安定性」（`docs/refactoring/既知の問題_IDLE復帰の間欠的不安定性.md`）の主因。ユーザーの意図は「IDLE = トリガーを待機している状態。電力を食うToFだけ省電力化すればよく、通信や他機能まで止める必要はない」。
2. **状態遷移型トリガー設計への整理**。現状のトリガー源は通信（UART DMA / Wi-Fi・BLEのOTA）のみ。今後、照度センサーや音圧に基づくトリガーを追加できる構造にしたい。
3. **不要プログラムの削除**。テストコード・未使用のLPBAM・未使用ドライバがビルド/書き込み時間を無駄にしている。
4. **マイクのオフセット修正**。録音音声の音程が元音源からズレている。PCから既知周波数を出して録音・解析し、周波数特性を測定する。

### 中核概念: IDLE = 「トリガー待機状態」（要求1と要求2の共通軸）

確認済みの因果関係（実コード調査済み）:

- `NonSecure/Core/Src/app_loop.c` の `App_Run()` L72 が、IDLE突入時に `Comm_SetTelemetryEnabled(0U)` を呼ぶ（**唯一の呼び出し元**）。
- これが `Secure/Core/Src/comm_service.cpp` の `telemetryEnabled`（L81、初期値 `true`）を false にする。
- その結果 `Service::poll()`（L512-588）で、L552/557/567 のUART/BLEテレメトリ送信をスキップし、さらに **L574-588 で `pollTcp()` ごとスキップ**する。
- `pollTcp()` の `MX_WIFI_Socket_accept()` は未接続時に数百ms〜10秒ブロックし、その間コンソール受信バイトがdrainされず、IDLEから復帰できなくなる（L576-584のコメントに既述）。

→ **IDLE突入時に `Comm_SetTelemetryEnabled(0U)` を呼ばなければ、`telemetryEnabled` は常時 true のまま通信スタックが生存し続け、この間欠不安定は解消される見込み。** これが要求1の本体であり、「IDLE = ToF(電力大)だけSLEEP、通信は生かしてトリガーを待つ状態」という再定義そのもの。

---

## フェーズ構成と依存関係

| フェーズ | 内容 | 依存 | リスク |
|---|---|---|---|
| **P0** | 作業ツリー破損の修復（前提） | なし | 中（git操作） |
| **P1** | 要求1: IDLE再定義（通信を止めない） | P0 | 中 |
| **P2** | 要求2: トリガー抽象化 | P1 | 中 |
| **P3** | 要求3: 不要コード削除 | P0（P2後推奨） | 高（.cproject編集） |

実施順序: **P0 → P1 → P2 → P3**。P3はP0後なら独立実施可だがP2完了後を推奨。

> **要求4（マイク周波数特性解析）は本計画のスコープ外（別タスク）**。ユーザー判断により、今回はP0〜P3（IDLE再定義・トリガー抽象化・不要コード削除）に集中する。マイク解析の設計メモは本ファイル末尾の付録に残す。

---

## P0. 作業ツリー破損の修復（最優先）

### 現状
`git status` に以下がある（前セッションのファイル移動インシデントの残骸）:
- `Secure/Core/Inc/*`（23ヘッダ）が `deleted`
- `Secure/Core/Src/Inc/`（誤った移動先）が untracked
- `docs/refactoring/既知の問題_IDLE復帰の間欠的不安定性.md` が untracked（**今セッションで作った有用ファイル・保持する**）

HEAD（コミット済み）は正しく `Secure/Core/Inc/` にヘッダを持つ。`.cproject` と `Secure/Debug/Core/Src/subdir.mk` はどちらも正しく `../Core/Inc` を参照。つまり**未コミットの誤移動を破棄すれば正常化する**。

### 手順
- **P0-1**: 誤移動先 `Secure/Core/Src/Inc/` を削除し、`git checkout -- Secure/Core/Inc/` で23ヘッダをHEAD状態に復元する。
- **禁止事項**: `git reset --hard` や `git checkout -- .` は使わない（untrackedの有用な `.md` を巻き込むため）。**必ずパス指定**で行う。有用な `.md` は `??` のまま残す。

### 検証
`git status` で「deletedが消え、誤移動先が消え、`.md` のみ残る」ことを確認 → Secure/NonSecure両方をクリーンビルドして0エラー → 書込み→verify実行（この時点はIDLE以外の5項目PASSを基準。IDLEは既知の間欠問題があるためP1で解消予定）。

### ロールバック
P0は破壊的操作を含むため、着手前に `git stash -u` でのバックアップ可否を検討。ただしHEADが正しいので、失敗しても再度 `git checkout -- Secure/Core/Inc/` でやり直せる。

---

## P1. 要求1: IDLE = トリガー待機（通信を止めない）

### P1-1. `NonSecure/Core/Src/app_loop.c` の `App_Run()` 修正
- **IDLE突入部（現L68-75）**: `Sensors_Stop()`（ToF SLEEP化）と赤LED表示は**残す**。以下を**削除**:
  - `Audio_Stop()`（`ns_audio.c` 実装上ほぼno-op）
  - **`Comm_SetTelemetryEnabled(0U)`**（間欠不安定の主因）
- **IDLE復帰部（現L96-106）**: `Sensors_Resume()`（ToF WAKE）は**残す**。以下を**削除**:
  - `Audio_Resume()`
  - `Comm_SetTelemetryEnabled(1U)`
- **ループ構造の整理（推奨）**: 「テレメトリ送信＋LEDハートビートは毎ループ実施（周期だけACTIVE/IDLEで切替）」「ToFのSLEEP/WAKEとLED色のみmode依存」。IDLE中もテレメトリを流し続ける（通信を遮断しないユーザー意図に一致）。

### P1-2. コメント更新（現L38-43）
「IDLE = トリガー待機。ToFのみ省電力。通信は生存。`Comm_SetTelemetryEnabled` は呼ばない」に書き換える。

### P1-3. `NonSecure/Core/Inc/app_config.h`
IDLE中の送信レートを落とす場合、`CFG_IDLE_TELEMETRY_MS`（例 200U = 5Hz）を新設。**`FullStatus_t` には一切触れない**（165バイト不変。`static_assert(sizeof(FullStatus)==165)` が `telemetry.hpp:57`, `comm_service.cpp:655` にある）。

### P1-4. Secure側は変更不要
`Comm_SetTelemetryEnabled` / `CommBridge_SetTelemetryEnabled`（凍結API）は**残す**。呼ばれ方が変わるだけ。`telemetryEnabled` が常時trueになり、`pollTcp()` スキップが起きなくなる。

### P1-5. `docs/refactoring/verify_regression.py` の IDLE判定改訂（必須・同一コミット）
現行 L87-88 は「IDLE遷移 = 無通信2秒後にフレーム0」（`n_idle == 0`）を要求している。IDLE中も送信を継続する新設計では**この判定は必ずFAILする**ので再定義する:
- **IDLE遷移**: 「無通信後も通信は生存（フレーム継続）」かつ「`tof_ok == 0` / `tof_mm == 0`（ToFがSLEEP）」を確認する判定へ変更。
- **IDLE復帰**（L90-93）: 「復帰後に `tof_ok == 1` が再観測される」を追加。
- 改訂は verify スクリプトのみ。P1と同一コミットに含める。

### P1 検証
- ビルド0エラー → NonSecure書込み → 改訂版 verify 実行。
- **10回連続でリセット→IDLE経由→復帰を反復**し、間欠問題が解消したことを確認（これがP1の主眼）。`tof_ok` が 0↔1 で切り替わること。
- `NS_APP_VERSION`（`NonSecure/Core/Src/main.c` L42）を +1。

### P1 ロールバック
`git checkout -- NonSecure/Core/Src/app_loop.c NonSecure/Core/Inc/app_config.h docs/refactoring/verify_regression.py`

---

## P2. 要求2: トリガー抽象化

### 設計方針
現状のトリガー源は通信のみ（`comm_service.cpp` の `processRxByte` L426-431 が `nsActivity=true`、回収は `CommBridge_PollHostCommand` L663-675）。照度/音圧はNonSecure領域のセンサーなので、**トリガーの集約はNonSecure側で行うのがレイヤ的に正しい**。既存の `nsActivity`（通信トリガー）を活かしつつ、NonSecure側で全トリガーをORする。

### 新規契約: `NonSecure/Core/Inc/triggers.h`（契約層・基板非依存）
```c
typedef enum { TRIG_NONE = 0, TRIG_COMM = 1, TRIG_LIGHT = 2, TRIG_AUDIO = 3 } TriggerSource_t;

TriggerSource_t Trigger_Poll(void);            /* 直近ポール以降の発火要因 */
void Trigger_SetLightThreshold(uint32_t raw);  /* 照度しきい値 */
void Trigger_SetAudioThreshold(int16_t rms);   /* 音圧しきい値 */
```

### 実装: `NonSecure/Core/Src/triggers.c`（コア層）
- **通信トリガー**: `Comm_PollHostCommand()` の戻り値（`COMM_POLL_BYTE` / `COMM_POLL_ACTIVITY`）を `TRIG_COMM` に集約。
- **照度トリガー**: `Sensors_Refresh` が書く `st->light_raw`（`sensors.c` L166）がしきい値超えで `TRIG_LIGHT`。
- **音圧トリガー**: `Audio_Refresh` が書く `st->audio_rms`（`ns_audio.c` L63）がしきい値超えで `TRIG_AUDIO`。
- しきい値とトリガー状態は `triggers.c` の static 変数で保持。**`FullStatus_t` には絶対に入れない**（165バイト制約）。
- `triggers.c` は HAL/BSP を直接叩かず、`comm_api.h` / `sensors.h` / `ns_audio.h` の契約経由のみ（レイヤ厳守・基板非依存を維持）。
- 新規ファイルは `Core/Src` / `Core/Inc` 直下に置く（`.project` linkedResources の罠回避。上記「アーキテクチャ」節参照）。

### `app_loop.c` への統合
`App_Run()` L58-62 の「`Comm_PollHostCommand`→`lastActivityMs` 更新」を
`if (Trigger_Poll() != TRIG_NONE) { lastActivityMs = now; }` に置換する。
`cmdByte` 自体が別途必要なら `Comm_PollHostCommand` は併存させてよい。

### ステップ分割（各ステップ独立に検証）
- **P2-1**: `triggers.h` / `triggers.c` を新規作成（**まず `TRIG_COMM` のみ**実装、照度/音圧はスタブ）。`app_loop.c` を切替。**挙動が変わらない**ことを verify で確認。
- **P2-2**: 照度トリガーを有効化。IDLE中に照度刺激（明暗変化）→ ACTIVE復帰を実機確認。
- **P2-3**: 音圧トリガーを有効化。IDLE中に音 → ACTIVE復帰を実機確認。

### P2 注意
- Secure側（`processRxByte` の `nsActivity` 集約）は**変更しない**。
- しきい値はデフォルト無効（発火しない値）で段階導入。誤発火でIDLEに入れなくなる事故を防ぐ。

---

## P3. 要求3: 不要コード削除（1カテゴリ = 1コミット）

### P3-1. テストコード削除
- 削除対象: `Secure/Core/Src/test.cpp`, `tests_audio.cpp`, `tests_board.cpp`, `tests_memory.cpp`, `tests_security.cpp`, `tests_sensors.cpp`, `tests_wireless.cpp`（計7）、`Secure/Core/Inc/test.hpp`。
- `Secure/Core/Src/app_main.cpp` の調整: `App_RunTestsOnce` の宣言（L20付近）、`'t'` キー分岐ブロック（L121-128付近）、ヘルプ文字列内の `'t'=tests`（L72付近）、関連コメント（L5-6付近）を削除。
- Secure `.cproject` にはtest個別のexcludeが無く `Core/Src` 全体をビルドしている → **ファイル削除で対象外化**。削除後 `-importAll` で `subdir.mk` 再生成が必要。
- **ユーザー判断済み**: `'t'` キーのオンデマンド自己診断機能は失ってよい → **削除する**。

### P3-2. LPBAM一式削除（.cproject編集＝最大リスク）
- 削除対象: `NonSecure/LPBAM/LpbamAp1/`, `Utilities/lpbam/`, `NonSecure/Core/Inc/stm32_lpbam_conf.h`。`MX_LpbamAp1_*` はアプリから未呼び出し。
- `NonSecure/.cproject` を **Debug構成とRelease構成の両方**、対称に編集:
  - includePath から `../LPBAM/LpbamAp1`, `../../Utilities/lpbam`, `../../Utilities/lpbam/STM32U5` を削除。
  - defaults の巨大 value 文字列内の同パス群と、末尾 sourceEntry 名 `LPBAM` を削除。
  - sourceEntries の `<entry ... name="LPBAM"/>` を削除。
- 編集前に `.cproject` をscratchpadへ退避。編集後は必ず `-importAll` で両構成ビルド。

### P3-3. 未使用ドライバ削除（P3-1の後）
- **実施結果（2026-07-14）**: `Drivers/BSP/Components/m24256` と `Drivers/BSP/B-U585I-IOT02A/b_u585i_iot02a_eeprom.c/h` のみ削除。自己参照のみで完全に孤立していることを確認済み。
- **`aps6408`/`veml6030` は削除を見送った**（計画時の想定が誤っていたことが実装確認で判明）:
  - `aps6408`: `b_u585i_iot02a_ospi.c` が同一ファイル内でNOR（`ota.cpp`が使用中）とPSRAM（`APS6408_*`）の実装を混在させており、コンポーネント単体の削除はそのファイルの巻き添え削除を伴う。PSRAM自体は未使用だが、リスクに対して効果が小さく見送り。
  - `veml6030`: `b_u585i_iot02a_light_sensor.c` の `BSP_LIGHT_SENSOR_Init` が VEML6030 を先にprobeし、失敗したら VEML3235 にフォールバックする設計（ボードリビジョン差異の吸収）。削除すると一部リビジョンで光センサーが動かなくなるリスクがあるため見送り。

### P3 検証（各サブステップ）
`-importAll` 付きヘッドレスビルドで Secure+NonSecure Debug が0エラー → 書込み→verify全項目PASS → **ビルド時間・elfサイズの before/after を記録**（短縮効果の定量確認）。

### P3 リスク
- `.cproject` 編集後は必ず `-importAll`（しないと古い `subdir.mk` が削除済みファイルを参照してエラー）。
- Debug/Release を対称に編集する。
- 1カテゴリ = 1コミットで、いつでも1段階戻せるようにする。

---

## 付録: 要求4 マイク周波数特性解析（別タスク・スコープ外メモ）

> 本計画のスコープ外。ユーザー判断により別タスクとして後日実施する。設計メモのみ残す。

### 仮説
音程ズレ = `CFG_AUDIO_SAMPLE_RATE = 16000U`（`Secure/Core/Src/Inc/app_config.h` L30）と、PLL3由来のMDF1実クロックの乖離。PLL3設定は `audio_capture.cpp` L36-40（PLL3N=80 / PLL3P=28 / PLL3Q=28, DIVQ, source=MSI）。

### 測定手順（ファームウェア無変更）
1. PCで既知トーン `f_in` を再生 → `'a'` で音声ストリーム開始 → CMD_AUDIO(0x03) フレーム（`comm_service.cpp` L604-620、1024サンプル×2）を取得 → FFTでピーク周波数 `f_meas` を得る。
2. 実効レート `f_s_eff = 16000 × (f_meas / f_in)` を逆算。複数周波数のスイープ（200Hz〜4kHzを対数間隔）で、比が一定（純粋なクロックずれ）か周波数依存（デシメーションフィルタ要因）かを切り分ける。
3. 是正案（**本計画は測定まで。実際の修正は別タスク**）: 名目サンプルレート補正、または PLL3 再設計。

### 成果物: `pc_side/mic_freq_response/`
- `measure.py`（トーン再生 + フレーム取得 + FFT + 実効レート逆算）
- `sweep.py`（スイープ + 比のグラフ化）
- 既存 `pc_side/status_monitor/protocol.py` のフレームパーサを再利用。依存: numpy / scipy / pyserial。
- フレームのサンプル順序・エンディアンを `protocol.py` と `comm_service.cpp` で突合すること。

---

## アーキテクチャ（前提知識・レイヤ構造）

このプロジェクトは STM32U585 の TrustZone を使い、Secure(Bank1)/NonSecure(Bank2) に分かれる。
Secure = 更新しない基礎領域(通信・OTA・起動監視)、NonSecure = OTAで更新するアプリ層(センサー・アプリロジック)。

レイヤはディレクトリではなく **ファイル名** で表現する。
**重要な制約**: 新規ファイルは必ず `Core/Src` / `Core/Inc` 直下に置くこと。
理由: このプロジェクトのEclipse `.project` は特殊なlinkedResources構造を持ち、`Core/Src` 以外に置いたファイルはビルド対象にならない。

```
【契約層】(ヘッダのみ。自作基板でも一切変更しない)
  NonSecure/Core/Inc/comm_api.h      ... NS→Secure通信API
  NonSecure/Core/Inc/board_io.h      ... LED/ボタン契約
  NonSecure/Core/Inc/sensors.h       ... センサー契約
  NonSecure/Core/Inc/ns_audio.h      ... 音声契約
  NonSecure/Core/Inc/triggers.h      ... トリガー契約(P2で新規)
  Secure/Core/Inc/comm_backend.h     ... 通信サービス移植境界
  Secure_nsclib/comm_dto.h           ... FullStatus_t 165バイト(凍結)

【コア層】(基板非依存。自作基板へそのままコピーする)
  NonSecure/Core/Src/app_loop.c      ... ACTIVE/IDLEステートマシン
  NonSecure/Core/Src/ns_audio.c      ... 音声RMS/波形計算
  NonSecure/Core/Src/triggers.c      ... トリガー集約(P2で新規)
  Secure/Core/Src/comm_service.cpp   ... Service本体
  Secure/Core/Src/ota.cpp            ... OTA機構
  Secure/Core/Src/boot_guard.cpp     ... 起動監視
  Secure/Core/Src/frame_codec.c      ... フレームプロトコル

【port層】(基板依存。自作基板ではこれらだけ再実装する)
  NonSecure/Core/Src/board_io.c      ... LED PH6/PH7, ボタンPC13
  NonSecure/Core/Src/sensors.c       ... BSPセンサー実装
  Secure/Core/Src/comm_wifi.cpp      ... EMW3080 Wi-Fi/TCP
  Secure/Core/Src/comm_ble.cpp       ... WB5MMG BLE
  Secure/Core/Src/comm_uart.cpp      ... VCP非同期TX
  Secure/Core/Src/audio_capture.cpp  ... MDF1音声キャプチャ
  Secure/Core/Src/mcu_info.cpp       ... 内蔵ADC/メモリ統計
  Secure/Core/Src/console.cpp        ... UART RX DMA

【アダプタ層】(TrustZone形態依存)
  Secure/Core/Src/secure_nsc.c       ... CMSEゲートウェイ(TZ基板用)
  portability/comm_api_direct.c      ... 非TZ基板用の直結実装(ビルド対象外)
```

**設計方式: リンク時差し替え**（関数ポインタのopsテーブルは使わない）。契約=ヘッダの関数宣言、実装=portファイル。
自作基板への移行時は port層ファイルを新基板用に同名関数で再実装するだけ。実装漏れはリンクエラーで検出される。

---

## 共通コマンド（各フェーズの検証で使う）

### パス定義

| 名前 | 実際のパス |
|---|---|
| `<REPO>` | `d:\App\STM32CubeIDE\workspace_2.1.1\B-U585I-IOT02A` |
| `<CUBEIDEC>` | `d:\App\ST\STM32CubeIDE_2.1.1\STM32CubeIDE\stm32cubeidec.exe` |
| `<CLI>` | `d:\App\ST\STM32CubeIDE_2.1.1\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_2.2.400.202601091506\tools\bin\STM32_Programmer_CLI.exe` |
| `<HWS>` | ヘッドレスビルド専用ワークスペース(任意の空ディレクトリ)。初回に `-importAll` で作られる |

### ビルド（Git Bash構文）

```bash
# 初回、または .project/.cproject を変更した後は -importAll 付きで実行
"<CUBEIDEC>" --launcher.suppressErrors -nosplash \
  -application org.eclipse.cdt.managedbuilder.core.headlessbuild \
  -data "<HWS>" -importAll "<REPO>" \
  -build "B-U585I-IOT02A_Secure/Debug" -build "B-U585I-IOT02A_NonSecure/Debug"

# 2回目以降(ソース変更のみ)は -importAll 不要
"<CUBEIDEC>" --launcher.suppressErrors -nosplash \
  -application org.eclipse.cdt.managedbuilder.core.headlessbuild \
  -data "<HWS>" \
  -build "B-U585I-IOT02A_Secure/Debug" -build "B-U585I-IOT02A_NonSecure/Debug"
```

**期待結果**: 各ビルドの末尾に `Build Finished. 0 errors`。warningはSecureの既存2件(`g_AudioEvents`/`g_AudioErrors` のextern初期化警告)のみ許容。

**トラブル対処**:
- `Workspace already in use!` → `<HWS>/.metadata/.lock` を削除して再実行
- `-importAll` で `No file system is defined for scheme` → パスをWindows形式(`d:\...`)で渡しているか確認
- 新規 `.cpp`/`.c` が `undefined reference` → Core/Src直下に置いたか確認し、`-importAll` 付きで再ビルド

### 書き込みとリセット

```bash
# 書き込み(Secure→NonSecureの順。変更した側だけでよい)
"<CLI>" -c port=SWD mode=UR -d "<REPO>/Secure/Debug/B-U585I-IOT02A_Secure.elf" -v
"<CLI>" -c port=SWD mode=UR -d "<REPO>/NonSecure/Debug/B-U585I-IOT02A_NonSecure.elf" -v
# リセット(起動)
"<CLI>" -c port=SWD mode=UR -rst
```

**注意**: リセット後、Wi-Fi/BLE初期化に **約10〜15秒** かかる。検証スクリプトの実行はリセットの12秒後以降。
実機のフォルト確認は `"<CLI>" -c port=SWD mode=UR -r32 0xE000ED04 1`(ICSR)。`0x00000000`付近=正常、`VECTACTIVE=3`=HardFault。
書込み直後にHardFaultすることがあるが、多くは再リセットで回復する(SWD書込みの既知の癖)。

### 実機検証スクリプト

検証はST-LINK VCP (COM9, 921600baud) 経由。`<REPO>/docs/refactoring/verify_regression.py` を使う。

```bash
cd "<REPO>" && python docs/refactoring/verify_regression.py
```

**現行のPASS判定基準6項目**（P1でIDLE関連を改訂する。改訂内容はP1-5参照）:
1. ACTIVE維持: keep-alive(1秒毎に`\x00`)送信中、5秒で **150フレーム以上**
2. センサー生存: `tof_mm` が **2種類以上** 変動、`temp_c` が10〜60
3. IDLE遷移: keep-alive停止後フレーム停止 ← **P1で「通信は継続・ToFがSLEEP(tof_ok=0)」に改訂**
4. IDLE復帰: keep-alive再開後8秒で **100フレーム以上** ← **P1で「復帰後 tof_ok=1」を追加**
5. 音声: `'a'`送信後3秒でCMD_AUDIO(0x03) **50個以上**、`'s'`で停止
6. OTA照会: CMD_STATUS_REQ(0x06)へCMD_STATUS_RESP(0x07)応答

---

## 共通事項（全フェーズ）

- ビルド/検証は **Debug構成のみ**（NonSecure ReleaseにはBSPインクルードパス欠落の既存不整合あり）。
- 書込み順は Secure → NonSecure、リセット後 **12〜16秒待ってから** verify 実行。
- ロールバックはパス指定 `git checkout -- <path>`。`git reset --hard` は untracked の有用 `.md` 保護のため避ける。起動不能時は `refactor-baseline` タグを書き戻す。
- NonSecure変更を含むコミットでは `NS_APP_VERSION`（`NonSecure/Core/Src/main.c` L42）を +1。
- 1フェーズ = 原則1コミット（P2/P3はサブステップ単位で1コミット）。実機検証PASSまで次に進まない。指示にない「ついでの改善」を行わない。

## 守るべきガードレール（絶対厳守。改善に見えても違反禁止）

1. **`FullStatus_t` 165バイト不変**。`Secure_nsclib/comm_dto.h` と `telemetry.hpp:57`・`comm_service.cpp:655` の static_assert を削除・変更しない。メンバ追加/削除/並び替え禁止（PCパーサと一致必須）。**トリガーのしきい値・状態は別変数で持つ**（P2で厳守）。
2. **OTA信頼ルートを壊さない**: `handleFrame` / `ota::Manager` / `frame_codec` / `Frame_Decoder` / `processRxByte` をNonSecureへ移動しない。`secure_nsc.c` の `cmse_check_address_range` 検証を弱めない。
3. **境界API凍結**: `Comm_*` 7関数と `CommBridge_*` のシグネチャ・戻り値の意味(COMM_POLL_NONE/ACTIVITY/BYTE)を変更しない（P1では呼び方/意味を変えるがシグネチャは不変）。NS→Sへ関数ポインタを渡さない。
4. **インフラ凍結**: `.ioc` 再生成禁止。`main.c` の `MX_GTZC_S_Init` / GPIO NSEC設定 / Stage-0ジャンプフロー、リンカスクリプト(*.ld)、`partition_stm32u585xx.h`、startup、`ota.cpp` のアドレス定数(`kNsFlashBase`等)に触れない。
5. **機械的変更を心がける**: タイムアウト値・NVIC優先度・ボーレート・DMA属性・printf文字列・比較演算子を不用意に変えない。過去のバグ修正を説明するコメントブロックは保持する。
6. **HALコールバックは1ファイルに1定義**: `HAL_UART_TxCpltCallback` / `HAL_UART_RxCpltCallback` / `HAL_GPIO_EXTI_Rising_Callback` / `BSP_AUDIO_IN_*_CallBack`。**定義漏れはHALのweakに落ちてサイレント故障するので実機検証を省略しない**。
7. **volatile修飾を落とさない**: `nsActivity` / `g_AudioEvents` / `g_AudioErrors` / `uartTxBusy` / `bleLinkOk` 等は割り込みと共有。
8. `app_loop.c` / `triggers.c` は HAL/BSP を知らず契約ヘッダ経由のみ（基板非依存を維持）。

---

## 確認事項（ユーザー回答済み）

1. **テストコード削除**（P3-1）: → **削除する**（`'t'` 自己診断は失ってよい）。
2. **マイク解析**: → **別タスク**（本計画スコープ外、付録にメモのみ）。
3. **verify_regression.py のIDLE判定改訂**（P1-5）: IDLE中も通信を流す新定義では現行判定がFAILするため改訂必須。実装フェーズでこの方針で進める。

---

## Critical Files（実装で触る中心ファイル）

- `NonSecure/Core/Src/app_loop.c` — IDLEステートマシン（P1・P2）
- `NonSecure/Core/Inc/app_config.h` — IDLEチューナブル（P1）
- `NonSecure/Core/Inc/triggers.h` / `NonSecure/Core/Src/triggers.c` — 新規トリガー抽象（P2）
- `Secure/Core/Src/comm_service.cpp` — `telemetryEnabled` / `poll()`（P1は読み取り確認のみ、変更なし）
- `docs/refactoring/verify_regression.py` — IDLE判定改訂（P1-5）
- `NonSecure/.cproject` / `Secure/.cproject` — 不要コード削除（P3）
- `Secure/Core/Src/audio_capture.cpp` — マイクPLL3設定（付録・別タスク）
