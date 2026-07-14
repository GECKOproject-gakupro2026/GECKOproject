# Step 4: Secure BLE分離（comm_ble.cpp / comm_ble.hpp）

## 目的
`telemetry.cpp` から STM32WB5MMG BLEモジュール（UART4のATコマンド通信）を `comm_ble.cpp` に切り出す。BLEモジュールを別のものに載せ替えるときはこのファイルだけを書き直せばよくなる。

## 前提条件
- Step 3 が完了しコミット済み（`comm_wifi.cpp` が存在し、`verify_regression.py` が PASS）

## 対象ファイル
- **新規**: `Secure\Core\Inc\comm_ble.hpp`
- **新規**: `Secure\Core\Src\comm_ble.cpp`
- **変更**: `Secure\Core\Src\telemetry.cpp`

---

## 1. 新規ファイル: `Secure/Core/Inc/comm_ble.hpp`

```cpp
/**
  ******************************************************************************
  * @file    comm_ble.hpp
  * @brief   BLEモジュール(STM32WB5MMG, UART4のATコマンド)【port層】。
  *
  *          BLEモジュールを載せ替えるときは comm_ble.cpp だけを書き直す。
  *          呼び出し側(comm_service.cpp)はAT方言もUART番号も知らない。
  ******************************************************************************
  */
#ifndef COMM_BLE_HPP
#define COMM_BLE_HPP

#include "telemetry.hpp"   /* telemetry::FullStatus（送信するデータの型） */

namespace comm_ble
{

/* BLEモジュールを初期化し、P2Pサーバー＋アドバタイズを開始する。
 * 戻り値: モジュールとAT通信が成立すれば true */
bool Init();

/* モジュールとのAT通信が生きているか */
bool IsAlive();

/* セントラル(スマホ等)が接続中か */
bool IsConnected();

/* テレメトリを圧縮(MiniStatus 39バイト)してNotifyで送る。
 * 未接続なら何もしない。 */
void SendStatus(const telemetry::FullStatus &st);

} // namespace comm_ble

#endif /* COMM_BLE_HPP */
```

## 2. 新規ファイル: `Secure/Core/Src/comm_ble.cpp`

**骨格を示す。関数本体は `telemetry.cpp` から機械的にコピーすること。**

```cpp
/**
  ******************************************************************************
  * @file    comm_ble.cpp
  * @brief   comm_ble.hpp の STM32WB5MMG(UART4) 向け実装【port層】。
  ******************************************************************************
  */
#include "comm_ble.hpp"

#include "app_config.h"    /* CFG_BLE_BAUDRATE */
#include "frame_codec.h"   /* Frame_Encode, FRAME_CMD_STATUS_MINI */
#include "main.h"

#include "stm32wb_at.h"
#include "stm32wb_at_ble.h"
#include "stm32wb_at_client.h"

#include <cstdio>
#include <cstring>

extern UART_HandleTypeDef huart4; /* STM32WB5MMG BLE module (AT server) */

namespace comm_ble
{
namespace
{
/* --- BLE AT link state (set from AT reply/event callbacks) --- */
volatile bool bleLinkOk = false;
volatile bool bleConnected = false;
uint8_t bleAtBuffer[160]; /* long +BLE_EVT_WRITE events exceed 64 chars */
uint8_t bleRxByte;
bool bleGlueReady = false;

uint8_t bleSeq_ = 0;   /* Service のメンバから移す（BLE専用のシーケンス番号） */

// ここに telemetry.cpp から bleRawProbe() をそのままコピーする（L462-479）。
// static は付けたままでよい（このファイル内でしか使わない）。

} // namespace

// ここに公開関数(Init / IsAlive / IsConnected / SendStatus)を実装する。§4 を見ること。

} // namespace comm_ble

/* ---- ATライブラリが要求するグルー関数（telemetry.cpp から移す） ----
 * 【重要】これらはATライブラリから呼ばれる。プロジェクト内で1箇所にしか
 * 定義してはいけない。 */

extern "C" uint8_t stm32wb_at_ll_Init(void)
{
  // telemetry.cpp L1324-1335 の中身をそのままコピー
}

extern "C" uint8_t stm32wb_at_ll_DeInit(void)
{
  // telemetry.cpp L1336-1341 の中身をそのままコピー
}

extern "C" uint8_t stm32wb_at_ll_Transmit(uint8_t *pBuff, uint16_t Size)
{
  // telemetry.cpp L1342-1346 の中身をそのままコピー
}

extern "C" void stm32wb_at_ll_Async_receive(uint8_t new_frame)
{
  // telemetry.cpp L1347-1351 の中身をそのままコピー
}

/* BLE(UART4)の受信完了割り込み。
 * 【重要】プロジェクト内で1箇所にしか定義してはいけない。 */
extern "C" void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  // telemetry.cpp L1361-1373 の中身をそのままコピー
}

/* ATイベントのコールバック（モジュールから通知が来たときに呼ばれる） */
extern "C" uint8_t stm32wb_at_BLE_TEST_cb(stm32wb_at_BLE_TEST_t *param)
{
  // telemetry.cpp L1385-1391 の中身をそのままコピー
}

extern "C" uint8_t stm32wb_at_BLE_EVT_CONN_cb(stm32wb_at_BLE_EVT_CONN_t *param)
{
  // telemetry.cpp L1392-1399 の中身をそのままコピー
}

extern "C" uint8_t stm32wb_at_BLE_EVT_WRITE_cb(stm32wb_at_BLE_EVT_WRITE_t *param)
{
  // telemetry.cpp L1400-1417 の中身をそのままコピー
}
```

**注意**: これらのコールバックは `comm_ble` namespace の**外**（ファイル末尾）に置く。中の実装で `bleLinkOk` 等を触るときは `comm_ble::` の無名namespaceにアクセスできないので、**無名namespaceではなくファイルスコープのstatic変数にする**か、コールバックも同じ翻訳単位に置いて無名namespaceの変数を参照できるようにする（同一ファイル内なら無名namespaceの変数はファイル末尾からも見える。上の骨格のままでよい）。

## 3. 移動する関数・変数（この表にないものは移動しない）

すべて `telemetry.cpp` から `comm_ble.cpp` へ。

| 名前 | 現在の場所（目安） | 移動先での扱い | シグネチャ変更 |
|---|---|---|---|
| `volatile bool bleLinkOk` | L138 | 無名namespace（**volatile保持**） | なし |
| `volatile bool bleConnected` | L139 | 同上（**volatile保持**） | なし |
| `uint8_t bleAtBuffer[160]` | L140 | 同上 | なし |
| `uint8_t bleRxByte` | L141 | 同上 | なし |
| `bool bleGlueReady` | L142 | 同上 | なし |
| `bleSeq_`（Serviceのメンバ） | telemetry.hpp L143 | comm_ble.cpp の無名namespace内 static | なし |
| `static void bleRawProbe(const char *cmd)` | L462-479 | 無名namespace | なし |
| `stm32wb_at_ll_Init` | L1324-1335 | ファイル末尾 `extern "C"` | なし |
| `stm32wb_at_ll_DeInit` | L1336-1341 | 同上 | なし |
| `stm32wb_at_ll_Transmit` | L1342-1346 | 同上 | なし |
| `stm32wb_at_ll_Async_receive` | L1347-1351 | 同上 | なし |
| `HAL_UART_RxCpltCallback` | L1361-1373 | 同上 | なし |
| `stm32wb_at_BLE_TEST_cb` | L1385-1391 | 同上 | なし |
| `stm32wb_at_BLE_EVT_CONN_cb` | L1392-1399 | 同上 | なし |
| `stm32wb_at_BLE_EVT_WRITE_cb` | L1400-1417 | 同上 | なし |

## 4. 公開関数の実装

```cpp
bool Init()
{
  /* telemetry.cpp の Service::initRadio() (L481-516) から、BLE部分だけを
   * そのまま持ってくる。具体的には L485-513:
   *   - huart4.Init.BaudRate = CFG_BLE_BAUDRATE; HAL_UART_Init(&huart4);
   *   - bleRawProbe("AT\r\n"); ×3回（コメントもそのまま）
   *   - bleLinkOk = false;
   *   - stm32wb_at_ll_Init() / stm32wb_at_Init() / stm32wb_at_client_Init() の連鎖
   *   - bleGlueReady = true; stm32wb_at_client_Query(BLE_TEST); HAL_Delay(500);
   *   - bleLinkOk なら stm32wb_at_client_Set(BLE_SVC, &svc)
   * 最後に return bleLinkOk; とする。
   *
   * 【移さないもの】initRadio の先頭にある Wi-Fi初期化(comm_wifi::Init())と、
   * 末尾の printf("[TLM] radio: ...") は telemetry.cpp 側に残す。 */
}

bool IsAlive() { return bleLinkOk; }

bool IsConnected() { return bleConnected; }

void SendStatus(const telemetry::FullStatus &st)
{
  /* telemetry.cpp の Service::sendBle() (L713-754) の中身をそのまま持ってくる。
   *   - 冒頭の if (!bleLinkOk || !bleConnected) return; もそのまま
   *   - MiniStatus の組み立ても全部そのまま
   *   - bleSeq_++ はこのファイルの static 変数を使う
   * 型は telemetry::MiniStatus / telemetry::FullStatus なので、
   * using namespace telemetry; を関数の先頭に置くか、telemetry:: を付ける。 */
}
```

## 5. 変更: `telemetry.cpp`

### 5-1. 削除する（§3 の表で comm_ble.cpp に移したもの）

**注意**: `Service::sendBle` は関数まるごと削除する（呼び出し側は `comm_ble::SendStatus` に置換する）。

### 5-2. include を調整する

- **追加**: `#include "comm_ble.hpp"`
- **削除**: `#include "stm32wb_at.h"` / `#include "stm32wb_at_ble.h"` / `#include "stm32wb_at_client.h"`
- **削除**: `extern UART_HandleTypeDef huart4;`

### 5-3. `telemetry.hpp` を変更する

- `private:` の `void sendBle(const FullStatus &st);` 宣言を**削除**
- `private:` の `uint8_t bleSeq_ = 0;` メンバを**削除**（comm_ble.cpp に移したため）

**`MiniStatus` 構造体は telemetry.hpp に残す**（comm_ble.hpp が telemetry.hpp をincludeして使う）。`static_assert(sizeof(MiniStatus) == 39, ...)` も残す。

### 5-4. 呼び出しを置き換える

| 変更前 | 変更後 | 場所 |
|---|---|---|
| `Service::initRadio()` の BLE初期化部分（L485-513） | `status_.ble_alive = comm_ble::Init() ? 1U : 0U;` | L481-516 |
| `sendBle(status_);`（poll内） | `comm_ble::SendStatus(status_);` | L1153付近 |
| `collect()` 内の `st.ble_alive = bleLinkOk ? 1U : 0U;`（L697付近） | `st.ble_alive = comm_ble::IsAlive() ? 1U : 0U;` | L697 |
| `CommBridge_GetLinkStatus()` 内の `bleLinkOk` | `comm_ble::IsAlive()` | L1274付近 |
| `CommBridge_GetLinkStatus()` 内の `bleConnected` | `comm_ble::IsConnected()` | L1281付近 |

**`Service::initRadio()` の変更後の姿**:

```cpp
void Service::initRadio()
{
  status_.wifi_alive = comm_wifi::Init() ? 1U : 0U;   /* Step 3 で置換済み */
  status_.ble_alive = comm_ble::Init() ? 1U : 0U;

  printf("[TLM] radio: BLE=%s WiFi=%s\r\n",
         status_.ble_alive != 0U ? "OK" : "NG",
         status_.wifi_alive != 0U ? "OK" : "NG");
}
```

**注意**: printf の文言（`[TLM] radio: BLE=%s WiFi=%s`）は**変えない**。検証手順がこのログ文字列に依存している。

---

## 変更してはいけないこと

- `bleRawProbe` の3回の呼び出し（`AT\r\n` / `AT+BLE_TEST?\r\n` / `AT+BLE_SVC=1\r\n`）とその順序・コメントを変えない
- `HAL_Delay(500)`（AT応答待ち）を短くしない
- `volatile bool bleLinkOk` / `bleConnected` の **volatile を落とさない**（割り込みから書かれる）
- `MiniStatus` の39バイト固定と `static_assert` を維持する

## よくあるコンパイルエラーと対処

| エラー | 対処 |
|---|---|
| `'stm32wb_at_client_Set' was not declared` | comm_ble.cpp に `#include "stm32wb_at_client.h"` があるか確認 |
| `'huart4' was not declared` | comm_ble.cpp 冒頭に `extern UART_HandleTypeDef huart4;` を書いたか確認 |
| `'CFG_BLE_BAUDRATE' undeclared` | `#include "app_config.h"` があるか確認 |
| `'MiniStatus' was not declared` in comm_ble.cpp | `telemetry.hpp` をincludeしているか、`telemetry::MiniStatus` と書いているか確認 |
| `multiple definition of 'HAL_UART_RxCpltCallback'` | telemetry.cpp 側から消し忘れ |
| `multiple definition of 'stm32wb_at_ll_Init'` | 同上 |
| telemetry.cpp で `'bleLinkOk' was not declared` | 5-4 の置換漏れ |
| `'bleSeq_' is not a member of 'Service'` | telemetry.hpp からメンバ宣言を消したのに、telemetry.cpp のどこかでまだ使っている |

---

## ビルド・書き込み

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
```

## 実機検証

```bash
cd "$REPO"
python docs/refactoring/verify_regression.py
```

**期待結果**: **PASS**

**BLE固有の追加確認**（このステップでは必須）:

```bash
cd "$REPO"
python -c "
import sys, time
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
import serial
with serial.Serial('COM9', 921600, timeout=0.2) as s:
    t0 = time.time(); buf = b''
    while time.time() - t0 < 8:
        buf += s.read(8192)
    for line in buf.decode('utf-8', errors='replace').splitlines():
        if 'BLE' in line or 'radio' in line:
            print(line)
"
```

**期待される出力**（リセット直後）:
- `[BLE-RAW] AT` → `-> 3 bytes: O||` のようなAT応答（モジュールとUART4通信が生きている）
- `[TLM] radio: BLE=OK WiFi=OK` … **BLE=OK が出ること**

`BLE=NG` になったらBLE初期化が壊れている。ロールバックすること。

**さらに（可能なら）**: スマホのBLEアプリで `P2PSRV1` が見え、接続するとNotifyでデータが流れることを確認する。

## ロールバック

```bash
cd "d:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A"
git reset --hard HEAD
git clean -fd Secure/Core/Src/comm_ble.cpp Secure/Core/Inc/comm_ble.hpp
```

## 完了時のコミット

```bash
cd "d:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A"
git add Secure/Core/
git commit -m "refactor(step4): BLEスタックをcomm_ble.cppへ分離

telemetry.cppからSTM32WB5MMGのAT通信(約230行: 初期化・Notify送信・ATグルー・
コールバック群)をport層として切り出し。BLEモジュールを載せ替えるときは
comm_ble.cppだけを書き直せばよくなった。ロジック変更なし。"
```

## 次のステップ
`step5_Secure_UART_音声分離.md` へ進む。
