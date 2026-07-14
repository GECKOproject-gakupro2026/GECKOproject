# Step 6: MCU情報分離（mcu_info.cpp）+ telemetry.cpp → comm_service.cpp リネーム

## 目的
1. 内蔵ADC（ダイ温度・電源電圧）とメモリ統計の収集を `mcu_info.cpp` に切り出す
2. 分割し終わった `telemetry.cpp` を、実態に合った名前 `comm_service.cpp` にリネームする

これで分割作業は完了。1417行あったファイルが約450行の「通信サービス本体」になる。

## 前提条件
- Step 5 が完了しコミット済み（`verify_regression.py` が PASS）

## 対象ファイル
- **新規**: `Secure\Core\Inc\mcu_info.hpp`
- **新規**: `Secure\Core\Src\mcu_info.cpp`
- **変更→リネーム**: `Secure\Core\Src\telemetry.cpp` → `Secure\Core\Src\comm_service.cpp`
- **変更**: `Secure\Core\Inc\telemetry.hpp`（名前は据え置き。Serviceクラスの定義なので）

---

## パート A: MCU情報の分離

### A-1. 何を分離し、何を残すか（重要な判断）

`Service::refreshMcuInfo()` は2つの異なる仕事をしている:

| 部分 | 内容 | 扱い |
|---|---|---|
| ADC読み取り + メモリ統計（L596-617） | ダイ温度・VDDA・heap/RAM/Flash使用量 | **`mcu_info.cpp` へ移す** |
| CPU負荷計算（L619-637） | `loopCount_` / `loopWindowStart_` / `loopMax_` を使う | **Serviceに残す** |

**CPU負荷計算を分離しない理由**: これはServiceのメインループの回転数を測るもので、Serviceの内部状態（メンバ変数3つ）に強く依存している。無理に分離すると「Serviceの状態を外に晒す」ことになり、かえって結合が強くなる。

### A-2. 新規: `Secure/Core/Inc/mcu_info.hpp`

```cpp
/**
  ******************************************************************************
  * @file    mcu_info.hpp
  * @brief   MCU内蔵の自己診断情報（ダイ温度・電源電圧・メモリ使用量）。
  *
  *          【チップ依存・基板非依存】同じSTM32U5系なら基板が変わっても
  *          そのまま使える。別系統のMCUに移るときはここを書き直す。
  ******************************************************************************
  */
#ifndef MCU_INFO_HPP
#define MCU_INFO_HPP

#include "telemetry.hpp"   /* telemetry::FullStatus */

namespace mcu_info
{

/* 起動時に1回だけ呼ぶ。リセット要因・クロック・UID・IDCODEを st に埋め、
 * 内蔵ADC（VREFINT / 温度センサー）を較正して使える状態にする。 */
void Init(telemetry::FullStatus &st);

/* 定期的に呼ぶ。ダイ温度・VDDA・heap/RAM/Flash使用量を st に埋める。
 * CPU負荷(cpu_load_pct)はここでは埋めない（呼び出し側が計算する）。 */
void Refresh(telemetry::FullStatus &st);

} // namespace mcu_info

#endif /* MCU_INFO_HPP */
```

### A-3. 新規: `Secure/Core/Src/mcu_info.cpp`

```cpp
/**
  ******************************************************************************
  * @file    mcu_info.cpp
  * @brief   mcu_info.hpp の STM32U5 向け実装。
  ******************************************************************************
  */
#include "mcu_info.hpp"

#include "main.h"

#include <malloc.h>

/* Linker symbols for memory statistics */
extern "C" uint8_t _end;    /* end of .bss (start of heap)   */
extern "C" uint8_t _sdata;  /* start of .data                */
extern "C" uint8_t _edata;  /* end of .data                  */
extern "C" uint8_t _etext;  /* end of .text (flash)          */

namespace mcu_info
{
namespace
{
/* Internal ADC for die temperature and VDDA (via VREFINT) */
ADC_HandleTypeDef hadcMcu = {};
bool adcOk = false;

// ここに telemetry.cpp から adcReadChannel() をそのままコピー（L70-86）
} // namespace

void Init(telemetry::FullStatus &st)
{
  /* telemetry.cpp の Service::initMcuInfo() (L545-592) の中身をそのままコピー。
   * ただし `status_.xxx` を `st.xxx` に置き換える（それ以外は変えない）。 */
}

void Refresh(telemetry::FullStatus &st)
{
  /* telemetry.cpp の Service::refreshMcuInfo() (L594-617) から、
   * ADC読み取りとメモリ統計の部分だけをコピーする。
   *
   * 【移さない】L619-637 の CPU負荷計算（loopCount_/loopWindowStart_/loopMax_ を
   * 使う部分）。これはServiceに残す。
   *
   * 具体的にはこの範囲:
   *   if (adcOk) { ... VREFINT/温度センサー読み取り ... }
   *   struct mallinfo mi = mallinfo();
   *   ... heap_used/heap_free/ram_used/ram_total/flash_used/flash_total ...
   * までをコピーし、CPU負荷計算の直前で終わる。 */
}

} // namespace mcu_info
```

### A-4. 移動する（この表にないものは移動しない）

| 名前 | 現在の場所 | 移動先 | シグネチャ変更 |
|---|---|---|---|
| `ADC_HandleTypeDef hadcMcu` | telemetry.cpp L67 | mcu_info.cpp 無名namespace | なし |
| `bool adcOk` | L68 | 同上 | なし |
| `uint32_t adcReadChannel(uint32_t)` | L70-86 | 同上 | なし |
| `Service::initMcuInfo()` | L545-592 | `mcu_info::Init(FullStatus&)` | `status_` → 引数 `st` |
| `Service::refreshMcuInfo()` の **ADC+メモリ統計部のみ**（L596-617） | | `mcu_info::Refresh(FullStatus&)` | 同上 |
| `extern "C" uint8_t _end / _sdata / _edata / _etext` | L49-52 | mcu_info.cpp 冒頭 | なし |

**Serviceに残すもの**:
- `Service::refreshMcuInfo()` の**CPU負荷計算部（L619-637）**。ただし関数名を変えず、中身をCPU負荷計算だけにして、先頭で `mcu_info::Refresh(st);` を呼ぶ形にする

### A-5. `telemetry.cpp` の変更

**`Service::refreshMcuInfo()` の変更後の姿**:

```cpp
void Service::refreshMcuInfo(FullStatus &st)
{
  mcu_info::Refresh(st);

  /* CPU load: main-loop iterations in this window vs the best window seen */
  uint32_t now = HAL_GetTick();
  uint32_t win = now - loopWindowStart_;
  if (win >= 500U)
  {
    uint32_t rate = (loopCount_ * 1000U) / win;
    if (rate > loopMax_)
    {
      loopMax_ = rate;
    }
    st.cpu_load_pct = (loopMax_ > 0U)
        ? static_cast<uint8_t>(100U - (100U * rate) / loopMax_) : 0U;
    loopCount_ = 0;
    loopWindowStart_ = now;
  }
}
```

**`Service::init()` 内の `initMcuInfo();` を `mcu_info::Init(status_);` に置換する。**

### A-6. `telemetry.hpp` の変更

- `private:` の `void initMcuInfo();` 宣言を**削除**
- `void refreshMcuInfo(FullStatus &st);` は**残す**（中身がCPU負荷計算になる）

### A-7. include の調整（telemetry.cpp）

- **追加**: `#include "mcu_info.hpp"`
- **削除**: `#include <malloc.h>`、`extern "C" uint8_t _end;` などのリンカシンボル4行（mcu_info.cpp へ移した）

---

## パート B: telemetry.cpp → comm_service.cpp リネーム

分割が終わり、`telemetry.cpp` の中身は「通信サービス（Service クラス）」だけになった。実態に合った名前にする。

### B-1. git mv でリネームする

```bash
cd "d:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A"
git mv Secure/Core/Src/telemetry.cpp Secure/Core/Src/comm_service.cpp
```

**`telemetry.hpp` はリネームしない。** これは `Service` クラスと `FullStatus` / `MiniStatus` の定義であり、多くのファイル（comm_ble.hpp, mcu_info.hpp, ai_app.cpp など）がincludeしている。名前を変えると影響範囲が広がるので、このステップではやらない。

### B-2. ファイル冒頭のコメントを更新する

`comm_service.cpp` の冒頭コメントを、実態に合わせて書き換える（**これはコメントなので変更してよい**）:

```cpp
/**
  ******************************************************************************
  * @file    comm_service.cpp
  * @brief   通信サービス本体【コア層・基板非依存】。
  *
  *          テレメトリの収集・組み立て、フレームプロトコルの処理(OTA含む)、
  *          NonSecureからのNSCゲートウェイの受け口(CommBridge_*)を担当する。
  *
  *          ハードウェアには直接触らない。すべてport層のAPI経由:
  *            comm_wifi.hpp     ... Wi-Fi/TCP
  *            comm_ble.hpp      ... BLE
  *            comm_uart.hpp     ... UART送信
  *            audio_capture.hpp ... マイク
  *            mcu_info.hpp      ... 内蔵ADC/メモリ統計
  *            console.h         ... UART受信
  *          だから基板を変えてもこのファイルは(ほぼ)無改造で移植できる。
  ******************************************************************************
  */
```

### B-3. ビルドシステムへの影響

`Core/Src` 直下でのリネームなので、`.project` / `.cproject` の変更は**不要**（自動収集される）。ただし念のため `-importAll` 付きでビルドすること。

---

## 変更してはいけないこと

- CPU負荷計算のロジック（`loopMax_` を基準にした相対値）を変えない
- `mcu_info::Init()` 内のADC較正シーケンス（`HAL_PWREx_EnableVddA()` → クロック設定 → `HAL_ADCEx_Calibration_Start`）の順序を変えない。`HAL_PWREx_EnableVddA()` を忘れるとADCが動かない（過去に踏んだ罠）
- `telemetry.hpp` をリネームしない
- `FullStatus` / `MiniStatus` の定義と static_assert を触らない

## よくあるコンパイルエラーと対処

| エラー | 対処 |
|---|---|
| `'mallinfo' was not declared` in mcu_info.cpp | `#include <malloc.h>` があるか確認 |
| `'_end' was not declared` | mcu_info.cpp 冒頭に `extern "C" uint8_t _end;` 等の4行を書いたか |
| `'ADC_CHANNEL_VREFINT' undeclared` | `#include "main.h"` があるか（HALのADC定義はここから） |
| comm_service.cpp で `'adcOk' was not declared` | A-5 の置換漏れ。`refreshMcuInfo` から ADC 部分を消し忘れている |
| `undefined reference to 'mcu_info::Init'` | `mcu_info.cpp` が `Secure/Core/Src/` 直下にあるか確認 |
| リネーム後 `No rule to make target 'Core/Src/telemetry.o'` | ビルドキャッシュが古い。`-importAll` 付きで再ビルド。それでも駄目なら `Secure/Debug/` を削除してフルビルド |

---

## ビルド・書き込み・検証

```bash
CUBEIDEC="d:/App/ST/STM32CubeIDE_2.1.1/STM32CubeIDE/stm32cubeidec.exe"
CLI="d:/App/ST/STM32CubeIDE_2.1.1/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_2.2.400.202601091506/tools/bin/STM32_Programmer_CLI.exe"
HWS="C:/temp/hws_refactor"
REPO="d:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A"

# リネームしたのでビルドキャッシュを消してからフルビルドする
rm -rf "$REPO/Secure/Debug"

"$CUBEIDEC" --launcher.suppressErrors -nosplash \
  -application org.eclipse.cdt.managedbuilder.core.headlessbuild \
  -data "$HWS" -importAll "$REPO" -build "B-U585I-IOT02A_Secure/Debug"

"$CLI" -c port=SWD mode=UR -d "$REPO/Secure/Debug/B-U585I-IOT02A_Secure.elf" -v
"$CLI" -c port=SWD mode=UR -rst
sleep 14

cd "$REPO"
python docs/refactoring/verify_regression.py
```

**期待結果**: **PASS**

**MCU情報固有の追加確認**:

```bash
cd "$REPO"
python -c "
import sys, time
sys.path.insert(0, 'pc_side/status_monitor')
import serial, protocol
p = protocol.FrameParser()
with serial.Serial('COM9', 921600, timeout=0.2) as s:
    t0=time.time(); last=None
    while time.time()-t0 < 4:
        s.write(b'\x00'); time.sleep(0.3)
        for it in p.feed(s.read(16384)):
            if it[0]=='frame' and it[1]==1:
                st = protocol.decode_status(it[1], it[3])
                if st: last = st
    if last:
        print(f'die_temp = {last.die_temp_c:.1f} C   (期待: 20〜70)')
        print(f'vdda     = {last.vdda_mv} mV        (期待: 3000〜3500)')
        print(f'ram_used = {last.ram_used} bytes    (期待: 0でない)')
        print(f'flash_kb = {last.flash_kb} KB       (期待: 2048)')
        ok = (20 < last.die_temp_c < 70) and (3000 < last.vdda_mv < 3500) \
             and last.ram_used > 0 and last.flash_kb == 2048
        print('PASS' if ok else 'FAIL: MCU情報が壊れている')
"
```

**期待結果**: すべて妥当値で `PASS`。`die_temp=0` や `vdda=0` ならADCが死んでいる。

## 分割完了の確認

```bash
cd "$REPO"
wc -l Secure/Core/Src/comm_service.cpp Secure/Core/Src/comm_wifi.cpp \
      Secure/Core/Src/comm_ble.cpp Secure/Core/Src/comm_uart.cpp \
      Secure/Core/Src/audio_capture.cpp Secure/Core/Src/mcu_info.cpp
```

**期待される目安**（多少ずれてよい）:
- `comm_service.cpp`: 約450行（元は1417行）
- `comm_wifi.cpp`: 約370行
- `comm_ble.cpp`: 約230行
- `audio_capture.cpp`: 約140行
- `mcu_info.cpp`: 約140行
- `comm_uart.cpp`: 約60行

## ロールバック

```bash
cd "d:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A"
git reset --hard HEAD   # git mv も戻る
git clean -fd Secure/Core/Src/mcu_info.cpp Secure/Core/Inc/mcu_info.hpp
rm -rf Secure/Debug     # ビルドキャッシュを消す
```

## 完了時のコミット

```bash
cd "d:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A"
git add -A Secure/Core/
git commit -m "refactor(step6): MCU情報をmcu_info.cppへ分離し、telemetry.cppをcomm_service.cppへリネーム

内蔵ADC(ダイ温度/VDDA)とメモリ統計をmcu_info.cppへ。CPU負荷計算はServiceの
内部状態(loopCount_等)に依存するため分離せず残した。

分割完了により telemetry.cpp(1417行) は comm_service.cpp(約450行)になり、
実態(通信サービス本体)に合った名前にリネームした。ハードウェアには直接触らず、
port層のAPI経由でのみアクセスする構造になった。"
```

## 次のステップ
`step7_境界形式化と移植キット.md` へ進む。
