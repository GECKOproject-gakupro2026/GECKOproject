# Step 1: NonSecure 基板I/O分離（board_io.c / board_io.h）

## 目的
NonSecure の `main.c` に直書きされているLED・ボタンのGPIO操作を `board_io.c` に切り出し、「基板が変わったらこのファイルだけ書き直せばよい」状態にする。

## 前提条件
- Step 0 が完了し、`refactor-baseline` タグと `refactor/layering` ブランチがある
- `verify_regression.py` が PASS する状態

## 対象ファイル
- **変更**: `d:\App\STM32CubeIDE\workspace_2.1.1\B-U585I-IOT02A\NonSecure\Core\Src\main.c`
- **新規**: `d:\App\STM32CubeIDE\workspace_2.1.1\B-U585I-IOT02A\NonSecure\Core\Inc\board_io.h`
- **新規**: `d:\App\STM32CubeIDE\workspace_2.1.1\B-U585I-IOT02A\NonSecure\Core\Src\board_io.c`

---

## 1. 新規ファイル: `NonSecure/Core/Inc/board_io.h`

以下を**そのまま**作成する。

```c
/**
  ******************************************************************************
  * @file    board_io.h
  * @brief   基板依存のI/O契約（LED・ユーザーボタン・バージョン公開）。
  *
  *          【port層】この宣言は基板が変わっても変えない。実装(board_io.c)だけを
  *          新しい基板用に書き直すこと。呼び出し側(app_loop.c / main.c)は
  *          GPIOポート番号もHALも知らない。
  ******************************************************************************
  */
#ifndef BOARD_IO_H
#define BOARD_IO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* LED・ボタンのGPIOを初期化する。両LEDは消灯状態で始まる。 */
void Board_Init(void);

/* ユーザーボタンの状態。1=押されている、0=離されている。 */
uint8_t Board_ButtonRead(void);

/* LED制御。この基板では緑=ハートビート(ACTIVE)、赤=低消費電力(IDLE)を示す。 */
void Board_LedGreenOff(void);
void Board_LedRedOff(void);
void Board_LedGreenToggle(void);
void Board_LedRedToggle(void);

/* 動作中のNonSecureバージョンを、Secure側やホストツールが読める固定番地へ公開する。 */
void Board_PublishVersion(uint32_t version);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_IO_H */
```

## 2. 新規ファイル: `NonSecure/Core/Src/board_io.c`

以下を**そのまま**作成する。関数の中身は `main.c` からの移動であり、**1文字も変えていない**（コメントも含む）。

```c
/**
  ******************************************************************************
  * @file    board_io.c
  * @brief   board_io.h の B-U585I-IOT02A 向け実装【port層】。
  *
  *          基板を変えるときは、このファイルだけを新しい基板のピン配置で
  *          書き直す。board_io.h の関数名・シグネチャは変えないこと。
  ******************************************************************************
  */
#include "board_io.h"

#include "main.h"

/* User LEDs on this board: LD6 red = PH6, LD7 green = PH7 */
#define LED_RED_PIN      GPIO_PIN_6
#define LED_GREEN_PIN    GPIO_PIN_7
#define LED_PORT         GPIOH

/* USER button (B1): PC13, active-high, pulldown (matches the Secure BSP's
 * BSP_PB_Init/BUTTON_MODE_GPIO configuration it used before Phase B). */
#define BUTTON_USER_PIN  GPIO_PIN_13
#define BUTTON_USER_PORT GPIOC

/* Mirror the version into a fixed SRAM3 (non-secure RAM) word so the Secure
 * side can display which NonSecure version is actually running. */
#define NS_RUNNING_VERSION_ADDR  0x200BFFF0UL /* top of NS SRAM3 */

static void led_init(void)
{
  GPIO_InitTypeDef gpio = {0};
  __HAL_RCC_GPIOH_CLK_ENABLE();
  gpio.Pin = LED_RED_PIN | LED_GREEN_PIN;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LED_PORT, &gpio);
  /* both off (PH6/PH7 read off = SET on this board's LED wiring) */
  HAL_GPIO_WritePin(LED_PORT, LED_RED_PIN | LED_GREEN_PIN, GPIO_PIN_SET);
}

/* TrustZone app-layer refactor Phase B: first sensor input moved to
 * NonSecure. PC13 was released NSEC by Secure's MX_GPIO_Init(); this
 * mirrors the BSP's BUTTON_MODE_GPIO config (input, pulldown). */
static void button_init(void)
{
  GPIO_InitTypeDef gpio = {0};
  __HAL_RCC_GPIOC_CLK_ENABLE();
  gpio.Pin = BUTTON_USER_PIN;
  gpio.Mode = GPIO_MODE_INPUT;
  gpio.Pull = GPIO_PULLDOWN;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(BUTTON_USER_PORT, &gpio);
}

void Board_Init(void)
{
  led_init();
  button_init();
}

uint8_t Board_ButtonRead(void)
{
  return (HAL_GPIO_ReadPin(BUTTON_USER_PORT, BUTTON_USER_PIN) == GPIO_PIN_SET) ? 1U : 0U;
}

/* Both user LEDs use the same wiring: SET = off, RESET = on (matches the
 * long-standing NonSecure demo where green blinked via TogglePin and red
 * was held off with SET). */
void Board_LedGreenOff(void) { HAL_GPIO_WritePin(LED_PORT, LED_GREEN_PIN, GPIO_PIN_SET); }
void Board_LedRedOff(void)   { HAL_GPIO_WritePin(LED_PORT, LED_RED_PIN, GPIO_PIN_SET); }
void Board_LedGreenToggle(void) { HAL_GPIO_TogglePin(LED_PORT, LED_GREEN_PIN); }
void Board_LedRedToggle(void)   { HAL_GPIO_TogglePin(LED_PORT, LED_RED_PIN); }

void Board_PublishVersion(uint32_t version)
{
  *(volatile uint32_t *)NS_RUNNING_VERSION_ADDR = version;
}
```

## 3. 変更: `NonSecure/Core/Src/main.c`

### 3-1. 削除する部分（この表に無いものは削除しない）

| 削除するもの | 現在の場所（目安の行） |
|---|---|
| `#define LED_RED_PIN` / `LED_GREEN_PIN` / `LED_PORT` とその上のコメント | L44-47 |
| `#define BUTTON_USER_PIN` / `BUTTON_USER_PORT` とその上のコメント | L49-52 |
| `#define NS_RUNNING_VERSION_ADDR` とその上のコメント | L89-91 |
| `static void led_init(void)` 関数まるごと | L93-104 |
| `static void button_init(void)` 関数まるごと（上のコメントも） | L106-118 |
| `static uint8_t button_read(void)` 関数まるごと | L120-123 |
| `led_green_off` / `led_red_off` / `led_green_toggle` / `led_red_toggle` の4行（上のコメントも） | L125-131 |

**削除してはいけないもの（main.cに残す）**:
- `#define NS_APP_VERSION`（L42。ただし後述の通り**19に上げる**）
- `typedef struct { ... } ns_appinfo_t;`（L58-63）
- `const ns_appinfo_t g_ns_appinfo = ...`（L86-87）… リンカが `.ns_appinfo` セクションに固定配置する。OTAのバージョン判定に必須なので**絶対に移動しない**
- `static void build_status(FullStatus_t *st)`（L138-145）… これは Step 2 で移動する。Step 1 では残す

### 3-2. include を追加する

`main.c` の include 部（`#include "app_config.h"` の隣、L27付近）に1行足す:

```c
#include "board_io.h"
```

### 3-3. 呼び出しを置換する（本文はこの対応表の通りに置換するだけ）

| 変更前 | 変更後 |
|---|---|
| `led_init();` （L177付近） | `Board_Init();` |
| `button_init();` （L178付近） | **この行は削除**（`Board_Init()` が両方やるため） |
| `button_read()` （build_status内 L144） | `Board_ButtonRead()` |
| `*(volatile uint32_t *)NS_RUNNING_VERSION_ADDR = g_ns_appinfo.version;` （L182） | `Board_PublishVersion(g_ns_appinfo.version);` |
| `led_green_off();` （すべて） | `Board_LedGreenOff();` |
| `led_red_off();` （すべて） | `Board_LedRedOff();` |
| `led_green_toggle();` （すべて） | `Board_LedGreenToggle();` |
| `led_red_toggle();` （すべて） | `Board_LedRedToggle();` |

置換箇所はメインループ内（L196-267のwhileループ）に複数ある。`led_` で検索して全部置換すること。

### 3-4. バージョンを上げる

L42 を `18U` → `19U` に変更する。

```c
#define NS_APP_VERSION   19U
```

**理由**: 実機でどのイメージが動いているか判別するため。NonSecureを変更するステップでは毎回+1する。

---

## 変更してはいけないこと

- `main()` の初期化順序（`HAL_Init()` → `MX_GTZC_NS_Init()` → `Board_Init()` → `Sensors_Init()` → `Audio_Init()` → バージョン公開 → `Secure_ConfirmBoot()`）を変えない
- ACTIVE/IDLEステートマシンのロジック（whileループ内の条件式・タイミング定数）を一切変えない。**このステップでは呼び出し名の置換だけ**
- `Secure_ConfirmBoot()` の位置を動かさない（BootGuardのロールバック判定に関わる）

## よくあるコンパイルエラーと対処

| エラー | 対処 |
|---|---|
| `'GPIO_InitTypeDef' undeclared` in board_io.c | `board_io.c` に `#include "main.h"` があるか確認（HALの型はここから来る） |
| `undefined reference to 'Board_Init'` | `board_io.c` を `NonSecure/Core/Src/` **直下**に置いたか確認。別の場所だとビルド対象にならない |
| `implicit declaration of function 'Board_LedRedOff'` | `main.c` に `#include "board_io.h"` を足したか確認 |
| `'led_init' defined but not used` | main.c から関数本体を消し忘れている。3-1の表の通り全部消す |

---

## ビルド

```bash
CUBEIDEC="d:/App/ST/STM32CubeIDE_2.1.1/STM32CubeIDE/stm32cubeidec.exe"
HWS="C:/temp/hws_refactor"
REPO="d:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A"

# 新規ファイルを追加したので -importAll を付ける
"$CUBEIDEC" --launcher.suppressErrors -nosplash \
  -application org.eclipse.cdt.managedbuilder.core.headlessbuild \
  -data "$HWS" -importAll "$REPO" \
  -build "B-U585I-IOT02A_NonSecure/Debug"
```

**期待結果**: `Build Finished. 0 errors, 0 warnings`

## 書き込みと実機検証

NonSecureだけ変更したので、NonSecureのみ書き込めばよい。

```bash
CLI="d:/App/ST/STM32CubeIDE_2.1.1/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_2.2.400.202601091506/tools/bin/STM32_Programmer_CLI.exe"
REPO="d:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A"

"$CLI" -c port=SWD mode=UR -d "$REPO/NonSecure/Debug/B-U585I-IOT02A_NonSecure.elf" -v
"$CLI" -c port=SWD mode=UR -rst
sleep 14

# バージョンが19になっていることを確認（0x13 = 19）
"$CLI" -c port=SWD mode=HOTPLUG -r32 0x08100404 1
# 期待: 0x08100404 : 00000013

cd "$REPO"
python docs/refactoring/verify_regression.py
```

**期待結果**:
- SWD読み出しで `00000013`（= 19。バージョンが正しく上がっている）
- `verify_regression.py` が `=== PASS: 全項目OK ===`

**目視でも確認すること**:
- ACTIVE中: 緑LEDが250ms間隔で点滅
- 3秒無通信後: 赤LEDが1秒間隔でゆっくり点滅、緑は消灯

## ロールバック

```bash
cd "d:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A"
git reset --hard HEAD
git clean -fd NonSecure/Core/Src/board_io.c NonSecure/Core/Inc/board_io.h
# 再ビルド・再書き込みして元に戻す
```

## 完了時のコミット

```bash
cd "d:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A"
git add NonSecure/Core/Src/main.c NonSecure/Core/Src/board_io.c NonSecure/Core/Inc/board_io.h
git commit -m "refactor(step1): NonSecureの基板I/O(LED/ボタン/バージョン公開)をboard_io.cへ分離

main.cに直書きだったGPIO操作をport層として切り出し。基板を変えるときは
board_io.cだけを書き直せばよい構造にした。関数本体は機械的移動でロジック変更なし。
NS_APP_VERSION 18→19。"
```

## 次のステップ
`step2_NSアプリループ分離.md` へ進む。
