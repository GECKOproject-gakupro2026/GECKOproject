# Step 7: 境界の形式化（comm_backend.h）と移植キット（portability/）

## 目的
1. 通信サービスの「移植境界」を `comm_backend.h` として明文化する
2. TrustZoneが無い基板へ移るときの手順とテンプレートを `portability/` に用意する

**このステップはコードの動作を変えない**（宣言の整理とドキュメント追加のみ）。バイナリはほぼ同じになる。

## 前提条件
- Step 6 が完了しコミット済み（`comm_service.cpp` が存在し、`verify_regression.py` が PASS）

## 対象ファイル
- **新規**: `Secure\Core\Inc\comm_backend.h`
- **変更**: `Secure\Core\Src\secure_nsc.c`（extern宣言をヘッダincludeに置換）
- **変更**: `Secure\Core\Src\comm_service.cpp`（同ヘッダをinclude）
- **新規**: `portability\comm_api_direct.c`（ビルド対象外のテンプレート）
- **新規**: `portability\PORTING.md`（移植ガイド）

---

## 背景: すでに境界は存在している

NonSecure から通信サービスを呼ぶ経路は、すでに2段になっている:

```
NonSecureアプリ
    ↓ Comm_Poll() など7関数                     [comm_api.h = 契約層。Step 2で導入済み]
secure_nsc.c の CMSEゲートウェイ                 [アダプタ層。TrustZone固有]
    - cmse_check_address_range でポインタ検証
    - Secureローカルにコピー
    ↓ CommBridge_Poll() など7関数                [★これが実質の移植境界]
comm_service.cpp の CommBridge_* 実装            [コア層]
```

`CommBridge_*` は既に `extern "C"` の素のC関数で、CMSEに依存していない。
つまり**TrustZoneが無い基板では、`secure_nsc.c` の代わりに「検証もコピーもせず、そのまま `CommBridge_*` を呼ぶだけ」のファイルを置けばよい**。

このステップでは、その事実をヘッダとして明文化し、テンプレートを用意する。

---

## 1. 新規: `Secure/Core/Inc/comm_backend.h`

```c
/**
  ******************************************************************************
  * @file    comm_backend.h
  * @brief   通信サービスの移植境界【契約層】。
  *
  *          ここに宣言された7関数が、アプリ層と通信サービスの間の唯一の接点。
  *          実装は comm_service.cpp にある。
  *
  *          アプリ層(NonSecure)からは、この7関数を直接は呼ばない。必ず
  *          アダプタ層を経由する:
  *
  *            TrustZoneあり基板:
  *              app --[comm_api.h]--> secure_nsc.c --[この境界]--> comm_service.cpp
  *                                     (CMSE検証 + ローカルコピー)
  *
  *            TrustZoneなし基板:
  *              app --[comm_api.h]--> comm_api_direct.c --[この境界]--> comm_service.cpp
  *                                     (単に転送するだけ)
  *
  *          【アダプタ層の責務】この境界に渡すポインタは、呼び出し前に検証済み
  *          であること。comm_service.cpp 側は、渡されたポインタを信頼して使う。
  *          TrustZone基板では secure_nsc.c が cmse_check_address_range で検証し、
  *          Secureローカルへコピーしてから渡している。この保証を弱めてはならない
  *          (OTAの信頼ルートがこの境界の内側で完結していることが安全性の根拠)。
  *
  *          【凍結】この7関数のシグネチャと戻り値の意味は変更禁止。
  ******************************************************************************
  */
#ifndef COMM_BACKEND_H
#define COMM_BACKEND_H

#include "comm_dto.h"   /* FullStatus_t, COMM_POLL_* */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 通信サービスを1回分駆動する */
void CommBridge_Poll(void);

/* テレメトリを送信する。stは検証済み・コピー済みのポインタであること。
 * 戻り値: 0=受理、負=エラー */
int CommBridge_SendTelemetry(const FullStatus_t *st);

/* ホスト受信バイトを1つ取り出す。
 * 戻り値: COMM_POLL_BYTE / COMM_POLL_ACTIVITY / COMM_POLL_NONE */
int CommBridge_PollHostCommand(uint8_t *out);

/* テレメトリ送信を止める/再開する（0=止める, 1=再開） */
void CommBridge_SetTelemetryEnabled(uint32_t on);

/* 接続状態ビット: bit0=BLE生存, bit1=Wi-Fi接続, bit2=BLE接続中, bit3=TCP接続中 */
uint32_t CommBridge_GetLinkStatus(void);

/* 音声窓をコピーする。戻り値: コピーしたサンプル数 */
uint32_t CommBridge_GetAudioBuffer(int16_t *dst, uint32_t maxSamples);

/* 起動成功を起動監視(BootGuard)に伝える。実装は boot_guard.cpp */
void BootGuard_ConfirmBoot(void);

#ifdef __cplusplus
}
#endif

#endif /* COMM_BACKEND_H */
```

## 2. 変更: `Secure/Core/Src/secure_nsc.c`

冒頭のベタ書き extern 宣言（L31, L36-41）を、ヘッダのincludeに置き換える。

**変更前**（L31, L36-41付近）:
```c
extern void BootGuard_ConfirmBoot(void);
...
extern void CommBridge_Poll(void);
extern int CommBridge_SendTelemetry(const FullStatus_t *st);
extern int CommBridge_PollHostCommand(uint8_t *out);
extern void CommBridge_SetTelemetryEnabled(uint32_t on);
extern uint32_t CommBridge_GetLinkStatus(void);
extern uint32_t CommBridge_GetAudioBuffer(int16_t *dst, uint32_t maxSamples);
```

**変更後**: 上の宣言をすべて削除し、include部に1行足す:
```c
#include "comm_backend.h"
```

**それ以外は1文字も変えない**。CMSEゲートウェイの実装（`cmse_check_address_range` による検証とローカルコピー）は**そのまま**。

## 3. 変更: `Secure/Core/Src/comm_service.cpp`

include部に1行足す:
```cpp
#include "comm_backend.h"
```

これにより、`CommBridge_*` の**宣言（ヘッダ）と定義（comm_service.cpp）が食い違ったらコンパイラが検出する**ようになる。

**注意**: `comm_service.cpp` 内の `extern "C" void CommBridge_Poll(void) { ... }` などの定義はそのまま。ヘッダが `extern "C"` を付けているので二重定義にはならない。

---

## 4. 新規: `portability/comm_api_direct.c`

**このファイルはビルド対象外**（`portability/` はプロジェクトのソースディレクトリではないため、自動で拾われない）。自作基板の新プロジェクトを作るときに、ここからコピーして使う。

```c
/**
  ******************************************************************************
  * @file    comm_api_direct.c
  * @brief   【TrustZoneなし基板用】comm_api.h の実装テンプレート。
  *
  *          ★このファイルは現在のプロジェクトではビルドされない。
  *            TrustZoneなしの基板へ移植するとき、secure_nsc.c の代わりに
  *            新プロジェクトへコピーして使う。
  *
  *          TrustZoneがあると、アプリ(NonSecure)と通信サービス(Secure)は
  *          別のメモリ空間にいるため、境界を越えるポインタを検証し、
  *          Secure側へコピーする必要があった(secure_nsc.c がそれをしている)。
  *
  *          TrustZoneが無ければ両者は同じメモリ空間にいるので、検証もコピーも
  *          要らない。単に転送するだけでよい。それがこのファイル。
  *
  *          【重要】TrustZoneが無くなっても、Secure/NonSecureの「設計上の区分」は
  *          維持すること:
  *            - 基礎領域(通信・OTA・起動監視) = 更新しない、壊すと復旧不能
  *            - アプリ領域(センサー・アプリロジック) = OTAで更新する
  *          ハードウェアによる隔離が無くなるだけで、フラッシュ配置・OTAの更新単位・
  *          起動時のイメージ検証(Stage-0)・ロールバック(BootGuard)はそのまま残す。
  *          詳しくは PORTING.md を読むこと。
  ******************************************************************************
  */
#include "comm_api.h"
#include "comm_backend.h"

#include <stddef.h>

void Comm_Poll(void)
{
  CommBridge_Poll();
}

int Comm_SendTelemetry(const FullStatus_t *st)
{
  if (st == NULL)
  {
    return -1;
  }
  /* TrustZoneなし: 同一メモリ空間なので、検証もローカルコピーも不要。
   * そのまま渡す。 */
  return CommBridge_SendTelemetry(st);
}

int Comm_PollHostCommand(uint8_t *out)
{
  if (out == NULL)
  {
    return -1;
  }
  return CommBridge_PollHostCommand(out);
}

void Comm_SetTelemetryEnabled(uint32_t on)
{
  CommBridge_SetTelemetryEnabled(on);
}

uint32_t Comm_GetLinkStatus(void)
{
  return CommBridge_GetLinkStatus();
}

uint32_t Comm_GetAudioBuffer(int16_t *dst, uint32_t maxSamples)
{
  if (dst == NULL || maxSamples == 0U)
  {
    return 0U;
  }
  return CommBridge_GetAudioBuffer(dst, maxSamples);
}

void Secure_ConfirmBoot(void)
{
  BootGuard_ConfirmBoot();
}
```

## 5. 新規: `portability/PORTING.md`

```markdown
# 自作基板への移植ガイド

このプロジェクトは、基板依存のコード(port層)を差し替えるだけで別の基板へ移せる構造になっている。

## レイヤの区分

| レイヤ | ファイル | 移植時の扱い |
|---|---|---|
| **契約層** | `comm_api.h` / `comm_backend.h` / `board_io.h` / `sensors.h` / `ns_audio.h` / `comm_dto.h` | **変更しない**。そのままコピー |
| **コア層** | `app_loop.c` / `ns_audio.c` / `comm_service.cpp` / `ota.cpp` / `boot_guard.cpp` / `frame_codec.c` | **変更しない**。そのままコピー |
| **port層** | `board_io.c` / `sensors.c` / `comm_wifi.cpp` / `comm_ble.cpp` / `comm_uart.cpp` / `audio_capture.cpp` / `mcu_info.cpp` / `console.cpp` / `stm32u5xx_hal_msp.c` | **書き直す**。新基板のピン配置・モジュールに合わせる |
| **アダプタ層** | `secure_nsc.c`(TZあり) / `comm_api_direct.c`(TZなし) | どちらか一方を選ぶ |

## ケース1: TrustZoneあり基板 (STM32U5/L5系)

現行構成をほぼそのまま持っていける。

1. 新プロジェクトをCubeMXで作る（TrustZone有効、Secure/NonSecureの2プロジェクト構成）
2. **契約層とコア層のファイルをそのままコピー**する
3. `secure_nsc.c` をコピーする（CMSEゲートウェイ。中身は変更不要）
4. **port層を新基板に合わせて書き直す**:
   - `board_io.c`: LED/ボタンのGPIOポート・ピン番号
   - `sensors.c`: 使うセンサーのBSPドライバ呼び出し。**`sensors.h` の4関数(Init/Refresh/Stop/Resume)のシグネチャは変えない**
   - `comm_wifi.cpp` / `comm_ble.cpp`: 使う通信モジュールのドライバ
   - `audio_capture.cpp`: 使うマイク/ADC
   - `mcu_info.cpp`: MCUが同系統(STM32U5)ならほぼそのまま
5. **フラッシュ配置とOTA設定を合わせる**:
   - リンカスクリプト(`*.ld`)のBank1(Secure)/Bank2(NonSecure)の配置
   - `ota.cpp` の `kNsFlashBase` / `kNsFlashSize`
   - `main.c`(Secure)の `MX_GTZC_S_Init` でペリフェラルのSecure/NonSecure属性
   - NonSecureの `.ns_appinfo` セクション配置（バージョン判定に使う）
6. `verify_regression.py` を新基板で走らせてPASSすることを確認する

**センサーが変わる場合**: `sensors.c` だけを書き直す。`FullStatus_t` のフィールド(温度・湿度・ToF距離など)に何を入れるかは自由だが、**構造体のレイアウト(165バイト)は変えない**。使わないフィールドは0のままでよい。PCツール側のパーサと必ず一致させること。

## ケース2: TrustZoneなし基板 (STM32F4/G4系など)

ハードウェアによるSecure/NonSecure隔離が無くなるが、**設計上の区分は維持する**。

### 何が変わるか

- `secure_nsc.c` は使えない（CMSEが無い） → 代わりに `comm_api_direct.c` を使う
- Secure/NonSecureの2プロジェクト構成にはならない → 1プロジェクトになる
- GTZC/SAUの設定は不要

### 何を維持するか（重要）

TrustZoneが無くても、**フラッシュを2つの領域に分ける設計はそのまま維持する**:

| 領域 | 中身 | 更新 |
|---|---|---|
| **基礎領域** (旧Secure) | 起動ローダー(Stage-0)、通信スタック、OTA機構、起動監視(BootGuard) | **OTAで更新しない**。壊れると復旧不能なので、書き込み器でしか更新しない |
| **アプリ領域** (旧NonSecure) | センサー、アプリロジック、LED制御 | **OTAで更新する**。壊れてもBootGuardが前のバージョンへ自動ロールバックする |

これを維持する理由: **OTAの安全性はハードウェア隔離ではなく「更新するものと、しないものを分ける」設計から来ている**。通信スタックやOTA機構が誤ったファームで壊れると、二度と更新できなくなる。TrustZoneはそれを強制する仕組みにすぎず、無くても設計原則は同じ。

### 手順

1. 新プロジェクトを作る（TrustZoneなし、1プロジェクト）
2. リンカスクリプトでフラッシュを2領域に分ける:
   ```
   FLASH_BASE  (rx) : ORIGIN = 0x08000000, LENGTH = 512K   /* 基礎領域 */
   FLASH_APP   (rx) : ORIGIN = 0x08080000, LENGTH = 512K   /* アプリ領域(OTA更新対象) */
   ```
   （サイズは基板のフラッシュ容量に合わせる）
3. 契約層・コア層をコピー
4. **`secure_nsc.c` の代わりに `comm_api_direct.c` をコピーしてビルドに含める**
5. port層を書き直す
6. `ota.cpp` のアドレス定数を新しいフラッシュ配置に合わせる
7. Stage-0（起動時のイメージ検証とジャンプ）を、`main.c`(Secure) の該当部分を参考に実装する:
   - アプリ領域の先頭ワード(スタックポインタ)とその次(Reset_Handler)が妥当か検証
   - `.ns_appinfo` 相当のマジック("NSAP")とバージョンを検証
   - 妥当ならアプリ領域へジャンプ（`VTOR` を書き換えてから）
   - 妥当でなければ基礎領域に留まり、OTAローダーとして動く
8. BootGuard（起動試行カウンタ）は、バックアップレジスタ(RTC/TAMP)があればそのまま使える

### アプリ層は無改造で動く

`app_loop.c` / `sensors.c` / `ns_audio.c` は `comm_api.h` しか知らないので、
アダプタが `comm_api_direct.c` に変わっても**1行も変える必要がない**。
これがこのリファクタリングの主目的だった。

## 移植後の検証

`docs/refactoring/verify_regression.py` を新基板で走らせる。COMポート番号だけ書き換えればよい。
6項目すべてPASSすれば、移植は成功している。
```

---

## 6. ビルドへの影響

`portability/` はプロジェクトのソースディレクトリ外なので、**ビルドには一切影響しない**。`comm_backend.h` の導入も宣言の整理だけなので、**生成されるバイナリはほぼ同一**（宣言の場所が変わるだけ）。

---

## 変更してはいけないこと

- `secure_nsc.c` の CMSEゲートウェイの実装（`cmse_check_address_range` の検証とローカルコピー）を**削除・簡略化しない**。extern宣言をヘッダincludeに置き換えるだけ
- `portability/comm_api_direct.c` を**現在のプロジェクトのビルドに含めない**（`secure_nsc.c` の `Comm_*` とシンボルが衝突してリンクエラーになる）
- `CommBridge_*` のシグネチャを変えない

## よくあるコンパイルエラーと対処

| エラー | 対処 |
|---|---|
| `conflicting types for 'CommBridge_Poll'` | comm_backend.h の宣言と comm_service.cpp の定義が食い違っている。**これは正しくエラーを検出できている**。定義側を確認して合わせる |
| `multiple definition of 'Comm_Poll'` | `portability/comm_api_direct.c` を誤ってビルドに含めてしまった。除外する |
| secure_nsc.c で `'CommBridge_Poll' undeclared` | `#include "comm_backend.h"` を足したか確認 |

---

## ビルド・検証

```bash
CUBEIDEC="d:/App/ST/STM32CubeIDE_2.1.1/STM32CubeIDE/stm32cubeidec.exe"
CLI="d:/App/ST/STM32CubeIDE_2.1.1/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_2.2.400.202601091506/tools/bin/STM32_Programmer_CLI.exe"
HWS="C:/temp/hws_refactor"
REPO="d:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A"

"$CUBEIDEC" --launcher.suppressErrors -nosplash \
  -application org.eclipse.cdt.managedbuilder.core.headlessbuild \
  -data "$HWS" -importAll "$REPO" -build "B-U585I-IOT02A_Secure/Debug"

"$CLI" -c port=SWD mode=UR -d "$REPO/Secure/Debug/B-U585I-IOT02A_Secure.elf" -v
"$CLI" -c port=SWD mode=UR -rst
sleep 14

cd "$REPO"
python docs/refactoring/verify_regression.py
```

**期待結果**: **PASS**（動作は変わらないはずなので、変わったら何かを壊している）

**追加確認**: `portability/` がビルドに含まれていないこと

```bash
cd "$REPO"
grep -rn "portability" Secure/.cproject Secure/.project NonSecure/.cproject NonSecure/.project
# 期待: 何も出力されない
```

## ロールバック

```bash
cd "d:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A"
git reset --hard HEAD
git clean -fd Secure/Core/Inc/comm_backend.h portability/
```

## 完了時のコミット

```bash
cd "d:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A"
git add Secure/Core/ portability/
git commit -m "refactor(step7): 通信サービスの移植境界をcomm_backend.hとして明文化し、移植キットを追加

secure_nsc.cにベタ書きされていたCommBridge_*のextern宣言を、正式なヘッダ
(comm_backend.h)へ昇格。宣言と定義の食い違いをコンパイラが検出できるようになった。

portability/にTrustZoneなし基板用のアダプタ実装テンプレート(comm_api_direct.c)と
移植ガイド(PORTING.md)を追加。ビルド対象外なので現行動作に影響なし。

これによりアプリ層(app_loop.c/sensors.c/ns_audio.c)は、TrustZoneの有無に関わらず
無改造で移植できる構造が完成した。"
```

## 次のステップ
`step8_総合回帰とドキュメント更新.md` へ進む（最終ステップ）。
