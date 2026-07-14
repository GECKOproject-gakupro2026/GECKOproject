# Step 2: NonSecure アプリループ分離（app_loop.c）+ 通信API境界（comm_api.h）

## 目的
ACTIVE/IDLEステートマシンを `app_loop.c` に切り出して**完全に基板非依存**にし、通信APIの窓口を `comm_api.h` に統一して**TrustZoneの有無に依存しない**構造にする。

## なぜこれが重要か
現在 `main.c` は `secure_nsc.h`（TrustZone専用ヘッダ）を直接includeしている。これを `comm_api.h` に差し替えることで、アプリ側のコードは「Comm_* という7つの関数がある」ことだけを知り、その実体がCMSEゲートウェイ（TrustZone基板）なのか直接呼び出し（非TrustZone基板）なのかを知らなくなる。**アプリコードを1行も変えずに移植できるようになる。**

## 前提条件
- Step 1 が完了しコミット済み（`git log --oneline -1` で step1 のコミットが見える）
- `verify_regression.py` が PASS する状態

## 対象ファイル
- **新規**: `NonSecure\Core\Inc\comm_api.h`
- **新規**: `NonSecure\Core\Src\app_loop.c`
- **新規**: `NonSecure\Core\Inc\app_loop.h`
- **変更**: `NonSecure\Core\Src\main.c`
- **変更**: `NonSecure\Core\Src\sensors.c`（includeの差し替えのみ）
- **変更**: `NonSecure\Core\Src\ns_audio.c`（includeの差し替えのみ）

---

## 1. 新規ファイル: `NonSecure/Core/Inc/comm_api.h`

以下を**そのまま**作成する。

```c
/**
  ******************************************************************************
  * @file    comm_api.h
  * @brief   アプリ層(NonSecure)から通信サービスを呼ぶための唯一の窓口【契約層】。
  *
  *          アプリ側のコードは、この7関数の実体がどう実装されているかを
  *          知らなくてよい:
  *            - TrustZoneあり基板: Secure/Core/Src/secure_nsc.c のCMSEゲートウェイ
  *            - TrustZoneなし基板: portability/comm_api_direct.c の直接呼び出し
  *          どちらの場合もこのヘッダは変更しない。だからアプリ層(app_loop.c,
  *          sensors.c, ns_audio.c)は無改造で移植できる。
  *
  *          【重要】この7関数のシグネチャと戻り値の意味は凍結。変更禁止。
  ******************************************************************************
  */
#ifndef COMM_API_H
#define COMM_API_H

#include "comm_dto.h"   /* FullStatus_t(165バイト固定) と COMM_POLL_* */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 通信サービスを1回分駆動する。アプリのメインループから毎周回必ず呼ぶこと。
 * これを呼ばないと受信もOTAも進まない。 */
void Comm_Poll(void);

/* テレメトリ1件を通信サービスに渡して送信させる。
 * 戻り値: 0=受理、負=エラー(不正ポインタ等) */
int Comm_SendTelemetry(const FullStatus_t *st);

/* ホストからの受信を1バイト取り出す。低消費電力モードからの復帰検知源。
 * 戻り値: COMM_POLL_BYTE(*outにバイトあり) / COMM_POLL_ACTIVITY(バイトは無いが
 *         受信はあった) / COMM_POLL_NONE(何もなし) / 負=不正ポインタ */
int Comm_PollHostCommand(uint8_t *out);

/* 通信サービスのテレメトリ送信を止める/再開する（低消費電力用）。
 * 0=止める、1=再開する。止めても受信は生きているのでウェイクできる。 */
void Comm_SetTelemetryEnabled(uint32_t on);

/* 接続状態のビット: bit0=BLE生存, bit1=Wi-Fi接続済, bit2=BLE接続中, bit3=TCPクライアント接続中 */
uint32_t Comm_GetLinkStatus(void);

/* 通信サービス側が録っている音声窓をコピーして受け取る。
 * 戻り値: 実際にコピーされたサンプル数 */
uint32_t Comm_GetAudioBuffer(int16_t *dst, uint32_t maxSamples);

/* 起動が成功したことを通信サービス側の起動監視に伝える。
 * これを呼ばないまま数回リセットが続くと、前のファームへ自動ロールバックされる。 */
void Secure_ConfirmBoot(void);

#ifdef __cplusplus
}
#endif

#endif /* COMM_API_H */
```

**注意**: この7関数は既存の `Secure_nsclib/secure_nsc.h` に宣言されているものと**シグネチャが完全に同じ**。だから実装（CMSEゲートウェイ）を変えずにヘッダだけ差し替えられる。名前も変えないこと。

## 2. 新規ファイル: `NonSecure/Core/Inc/app_loop.h`

```c
/**
  ******************************************************************************
  * @file    app_loop.h
  * @brief   アプリのメインループ【コア層・基板非依存】。
  ******************************************************************************
  */
#ifndef APP_LOOP_H
#define APP_LOOP_H

#ifdef __cplusplus
extern "C" {
#endif

/* ACTIVE/IDLEステートマシンを回し続ける。戻ってこない。
 * 呼ぶ前に Board_Init() / Sensors_Init() / Audio_Init() を済ませておくこと。 */
void App_Run(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_LOOP_H */
```

## 3. 新規ファイル: `NonSecure/Core/Src/app_loop.c`

`main.c` からステートマシンと `build_status` を**そのまま**移す。**ロジック・タイミング定数・コメントを1文字も変えないこと。**

```c
/**
  ******************************************************************************
  * @file    app_loop.c
  * @brief   ACTIVE/IDLEステートマシン【コア層・基板非依存】。
  *
  *          このファイルはHALもGPIOもBSPも知らない。すべて契約ヘッダ経由:
  *            board_io.h  ... LED・ボタン
  *            sensors.h   ... センサー
  *            ns_audio.h  ... 音声
  *            comm_api.h  ... 通信(TrustZoneの有無に非依存)
  *          だから基板を変えてもこのファイルは無改造で移植できる。
  ******************************************************************************
  */
#include "app_loop.h"

#include "app_config.h"
#include "board_io.h"
#include "comm_api.h"
#include "ns_audio.h"
#include "sensors.h"

#include "main.h"   /* HAL_GetTick */

/* NonSecure is the application layer's main loop. It pumps the Secure comm
 * service via Comm_Poll() and submits a telemetry snapshot through the
 * Comm_SendTelemetry() NSC gateway. Phase C: env/motion/light/ToF sensors
 * (I2C1/I2C2) are now read here too; audio/AI/MCU-info fields stay zeroed
 * until Phase D/F. */
static void build_status(FullStatus_t *st)
{
  Sensors_Refresh(st);
  Audio_Refresh(st);
  st->ver = 2U;
  st->uptime_ms = HAL_GetTick();
  st->button = Board_ButtonRead();
}

/* TrustZone Phase E: ACTIVE/IDLE low-power state machine. In ACTIVE the app
 * reads sensors, streams 50 Hz telemetry and blinks the green heartbeat.
 * After CFG_IDLE_TIMEOUT_MS with no host command it drops to IDLE: sensors
 * and telemetry stop (Secure comm push is quieted too), ToF stops ranging,
 * and the red LED slow-blinks. Any inbound host activity - which the Secure
 * comm stack keeps listening for even in IDLE - wakes it back to ACTIVE. */
void App_Run(void)
{
  enum { MODE_ACTIVE = 0, MODE_IDLE = 1 } mode = MODE_ACTIVE;
  uint32_t lastActivityMs = HAL_GetTick(); /* start ACTIVE, not instantly idle */
  uint32_t nextTelemetryTick = 0U;
  uint32_t nextLedTick = 0U;

  while (1)
  {
    Comm_Poll(); /* pumps the Secure comm service (TCP/OTA/BLE/audio) */

    uint32_t now = HAL_GetTick();

    /* Poll host input every loop (even in IDLE) - this is the wake source. */
    uint8_t cmdByte = 0U;
    if (Comm_PollHostCommand(&cmdByte) != COMM_POLL_NONE)
    {
      lastActivityMs = now; /* any inbound host traffic counts as activity */
    }

    if (mode == MODE_ACTIVE)
    {
      if ((int32_t)(now - lastActivityMs) >= (int32_t)CFG_IDLE_TIMEOUT_MS)
      {
        /* Enter IDLE: stop sensors/telemetry, quiet the Secure links. */
        mode = MODE_IDLE;
        Sensors_Stop();
        Audio_Stop();
        Comm_SetTelemetryEnabled(0U);
        Board_LedGreenOff();
        Board_LedRedOff();
        nextLedTick = now;
      }
      else
      {
        if ((int32_t)(now - nextTelemetryTick) >= 0)
        {
          nextTelemetryTick = now + CFG_ACTIVE_TELEMETRY_MS;
          static FullStatus_t st;
          build_status(&st);
          (void)Comm_SendTelemetry(&st);
        }
        if ((int32_t)(now - nextLedTick) >= 0)
        {
          nextLedTick = now + CFG_ACTIVE_HB_MS;
          Board_LedRedOff();
          Board_LedGreenToggle(); /* green heartbeat */
        }
      }
    }
    else /* MODE_IDLE */
    {
      if ((int32_t)(now - lastActivityMs) < (int32_t)CFG_IDLE_TIMEOUT_MS)
      {
        /* Woke on host activity: resume ACTIVE. */
        mode = MODE_ACTIVE;
        Sensors_Resume();
        Audio_Resume();
        Comm_SetTelemetryEnabled(1U);
        Board_LedRedOff();
        nextTelemetryTick = now;
        nextLedTick = now;
      }
      else if ((int32_t)(now - nextLedTick) >= 0)
      {
        nextLedTick = now + CFG_IDLE_LED_BLINK_MS;
        Board_LedGreenOff();
        Board_LedRedToggle(); /* red slow blink = low-power indicator */
      }
    }
  }
}
```

## 4. 変更: `NonSecure/Core/Src/main.c`

### 4-1. include を差し替える（L24-27付近）

**変更前**:
```c
#include "secure_nsc.h"
#include "sensors.h"
#include "ns_audio.h"
#include "app_config.h"
```

**変更後**:
```c
#include "app_loop.h"
#include "board_io.h"
#include "comm_api.h"
#include "sensors.h"
#include "ns_audio.h"
```

**ポイント**: `secure_nsc.h`（TrustZone専用）への直接依存を切り、`comm_api.h` 経由にする。`app_config.h` は app_loop.c 側でincludeするので main.c からは外してよい（残っていても害はない）。

### 4-2. `build_status` 関数を削除する

L138-145の `static void build_status(FullStatus_t *st)` を**関数まるごと削除**（上のコメントブロックも一緒に app_loop.c へ移したので削除する）。

### 4-3. メインループを `App_Run()` の呼び出しに置き換える

`main()` の中の、`/* USER CODE BEGIN WHILE */` から `/* USER CODE END 3 */` までの**whileループ全体**（L196-268付近）を削除し、代わりに `App_Run();` の1行にする。

**変更後の `main()` はこうなる**（`/* USER CODE BEGIN 2 */` 以降）:

```c
  /* USER CODE BEGIN 2 */
  Board_Init();
  Sensors_Init();
  Audio_Init();
  /* Publish the running version for the Secure side / host tools */
  Board_PublishVersion(g_ns_appinfo.version);
  /* Startup reached the main loop without a fault: tell the Secure Stage-0
   * loader this boot was good, clearing its rollback attempt counter (OTA
   * Phase 2, see boot_guard.hpp on the Secure side). */
  Secure_ConfirmBoot();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  App_Run(); /* does not return */
  /* USER CODE END WHILE */

  /* USER CODE BEGIN 3 */
  /* USER CODE END 3 */
}
```

### 4-4. バージョンを上げる

`#define NS_APP_VERSION` を `19U` → `20U`。

### 4-5. 残すもの（削除しない）

- `typedef struct { ... } ns_appinfo_t;`
- `const ns_appinfo_t g_ns_appinfo = {0x4E534150U, NS_APP_VERSION, {0U, 0U}};`（リンカ固定。**絶対に動かさない**）
- `MX_GTZC_NS_Init()` / `Error_Handler()` / `assert_failed()`
- `#include <string.h>`（使われていなければ消してもよいが、残っていても害はない）

## 5. 変更: `sensors.c` と `ns_audio.c` の include 差し替え

両ファイルで、`secure_nsc.h` を include している行があれば `comm_api.h` に差し替える。

- `NonSecure/Core/Src/ns_audio.c`: `Comm_GetAudioBuffer` を呼んでいる。`#include "secure_nsc.h"` → `#include "comm_api.h"`
- `NonSecure/Core/Src/sensors.c`: `comm_dto.h` 経由で `FullStatus_t` を使っている。`secure_nsc.h` を直接includeしていなければ**変更不要**

`grep -rn "secure_nsc.h" NonSecure/` で確認し、ヒットしたものをすべて `comm_api.h` に置き換える。**置き換え後、NonSecure配下から `secure_nsc.h` への参照がゼロになること**を確認する。

---

## 変更してはいけないこと

- **`App_Run()` の中身のロジック・タイミング定数・条件式を一切変えない**。main.cからのコピーであり、変更は「Board_*」への呼び出し名置換のみ（それはStep 1で済んでいる）
- `static FullStatus_t st;`（App_Run内）の `static` を外さない。165バイトの構造体をスタックに置くとスタックオーバーフローの危険がある
- `g_ns_appinfo` の位置・属性（`__attribute__((section(".ns_appinfo"), used))`）を触らない
- `Secure_ConfirmBoot()` の呼び出し位置（App_Run の前）を変えない

## よくあるコンパイルエラーと対処

| エラー | 対処 |
|---|---|
| `'FullStatus_t' undeclared` in app_loop.c | `comm_api.h` をincludeしているか確認（中で `comm_dto.h` をincludeしている） |
| `'COMM_POLL_NONE' undeclared` | 同上。`comm_dto.h` に定義がある |
| `'CFG_IDLE_TIMEOUT_MS' undeclared` | app_loop.c に `#include "app_config.h"` があるか確認 |
| `implicit declaration of 'HAL_GetTick'` | app_loop.c に `#include "main.h"` があるか確認 |
| `undefined reference to 'App_Run'` | `app_loop.c` を `NonSecure/Core/Src/` 直下に置いたか確認 |
| main.c で `'build_status' defined but not used` | 4-2 の削除を忘れている |

---

## ビルド

```bash
CUBEIDEC="d:/App/ST/STM32CubeIDE_2.1.1/STM32CubeIDE/stm32cubeidec.exe"
HWS="C:/temp/hws_refactor"
REPO="d:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A"

"$CUBEIDEC" --launcher.suppressErrors -nosplash \
  -application org.eclipse.cdt.managedbuilder.core.headlessbuild \
  -data "$HWS" -importAll "$REPO" \
  -build "B-U585I-IOT02A_NonSecure/Debug"
```

**期待結果**: `Build Finished. 0 errors, 0 warnings`

## 書き込みと実機検証

```bash
CLI="d:/App/ST/STM32CubeIDE_2.1.1/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_2.2.400.202601091506/tools/bin/STM32_Programmer_CLI.exe"
REPO="d:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A"

"$CLI" -c port=SWD mode=UR -d "$REPO/NonSecure/Debug/B-U585I-IOT02A_NonSecure.elf" -v
"$CLI" -c port=SWD mode=UR -rst
sleep 14

# バージョン20（0x14）を確認
"$CLI" -c port=SWD mode=HOTPLUG -r32 0x08100404 1
# 期待: 0x08100404 : 00000014

cd "$REPO"
python docs/refactoring/verify_regression.py
```

**期待結果**: SWD読み出しで `00000014`、`verify_regression.py` が **PASS**

**追加確認**: NonSecure配下に `secure_nsc.h` への参照が残っていないこと

```bash
cd "$REPO"
grep -rn "secure_nsc.h" NonSecure/Core/
# 期待: 何も出力されない（TrustZone非依存化の達成条件）
```

## ロールバック

```bash
cd "d:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A"
git reset --hard HEAD
git clean -fd NonSecure/Core/Src/app_loop.c NonSecure/Core/Inc/app_loop.h NonSecure/Core/Inc/comm_api.h
```

## 完了時のコミット

```bash
cd "d:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A"
git add NonSecure/Core/
git commit -m "refactor(step2): アプリループをapp_loop.cへ分離し、通信窓口をcomm_api.hに統一

ACTIVE/IDLEステートマシンをapp_loop.c(コア層・基板非依存)へ機械的に移動。
NonSecure側がsecure_nsc.h(TrustZone専用)を直接includeするのをやめ、
comm_api.h(契約層)経由にした。これによりアプリ層はTrustZoneの有無を知らなくなり、
非TrustZone基板へも無改造で移植できる。ロジック変更なし。NS_APP_VERSION 19→20。"
```

## 次のステップ
`step3_Secure_WiFi分離.md` へ進む。ここからSecure側の分割に入る（リスクが上がるので慎重に）。
