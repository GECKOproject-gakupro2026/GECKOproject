# Step 3: Secure Wi-Fi/TCP分離（comm_wifi.cpp / comm_wifi.hpp）

## 目的
`telemetry.cpp` から EMW3080 Wi-Fiモジュールの制御とTCPソケット操作を `comm_wifi.cpp` に切り出す。他のファイルから `MX_WIFI_*` API が消え、Wi-Fiモジュールを別のものに載せ替えるときはこのファイルだけを書き直せばよくなる。

## ⚠️ このステップの注意
ここからSecure側の分割に入る。**Secureが壊れるとボードが通信不能になり、OTAでの復旧もできなくなる**（SWD書き込みでのみ復旧可能）。以下を厳守すること:
- 関数本体は**コメントも含めて1文字も変えない**
- 特に `pollTcp` 内の長いコメント（`processRxByte` の説明、`recv` エラーコードの説明）は必ず保持する
- 実機検証がPASSするまでコミットしない

## 前提条件
- Step 2 が完了しコミット済み
- `verify_regression.py` が PASS する状態

## 対象ファイル
- **新規**: `Secure\Core\Inc\comm_wifi.hpp`
- **新規**: `Secure\Core\Src\comm_wifi.cpp`
- **変更**: `Secure\Core\Src\telemetry.cpp`

---

## 設計方針: 何を移し、何を残すか

**移す（Wi-Fiハードウェア操作そのもの）**: モジュール初期化、ソケットの生成/接続受付/送受信

**残す（プロトコル処理・OTA信頼ルート）**: `Service::sendTcp` / `Service::pollTcp` の**外枠**。`pollTcp` の中で受信バイトを `processRxByte()` に渡すループは、**OTAフレームの復号に関わる信頼ルートなのでServiceに残す**。`comm_wifi.cpp` は「バイトを受け取って返す」だけの下請けにする。

---

## 1. 新規ファイル: `Secure/Core/Inc/comm_wifi.hpp`

```cpp
/**
  ******************************************************************************
  * @file    comm_wifi.hpp
  * @brief   Wi-Fiモジュール(EMW3080)とTCPトランスポート【port層】。
  *
  *          Wi-Fiモジュールを別のものに載せ替えるときは comm_wifi.cpp だけを
  *          書き直す。この宣言と、呼び出し側(comm_service.cpp)は変えない。
  *
  *          【役割の境界】このファイルはバイト列の送受信までを担当する。
  *          受信バイトの解釈(OTAフレームの復号など)は呼び出し側の責任であり、
  *          ここには持ち込まない(OTA信頼ルートをSecure内の1箇所に集約するため)。
  ******************************************************************************
  */
#ifndef COMM_WIFI_HPP
#define COMM_WIFI_HPP

#include <cstddef>
#include <cstdint>

namespace comm_wifi
{

/* モジュールを初期化してAPに接続し、TCPサーバーを起動する。
 * 戻り値: Wi-Fiモジュールが生きていれば true（AP接続やTCP起動の失敗とは独立） */
bool Init();

/* APに接続してIPを取得済みか */
bool NetUp();

/* TCPクライアントが接続中か */
bool HasClient();

/* 組み立て済みのフレームをTCPクライアントへ送る。
 * 送信失敗が2回続いたらクライアントを切断する（切断検知はこの送信経路が権威）。 */
void SendFrame(const uint8_t *frame, size_t len);

/* 生バイト列をTCPクライアントへ送る（応答メッセージ用。失敗しても切断しない）。 */
void SendRaw(const uint8_t *data, int32_t len);

/* TCPの受信を1回分ポーリングする。
 *  - クライアント未接続時: 接続受付を試みる（1Hz）。戻り値は0
 *  - クライアント接続時:   受信バイトを buf に取り出す
 * 戻り値: buf に格納したバイト数（0以上）。呼び出し側がこのバイトを解釈する。 */
int32_t PollRecv(uint8_t *buf, size_t maxLen);

} // namespace comm_wifi

#endif /* COMM_WIFI_HPP */
```

## 2. 新規ファイル: `Secure/Core/Src/comm_wifi.cpp`

**骨格を以下に示す。関数本体は `telemetry.cpp` から機械的にコピーすること。**

```cpp
/**
  ******************************************************************************
  * @file    comm_wifi.cpp
  * @brief   comm_wifi.hpp の EMW3080(SPI2) 向け実装【port層】。
  ******************************************************************************
  */
#include "comm_wifi.hpp"

#include "main.h"

#include "mx_wifi.h"
#include "io_pattern/mx_wifi_io.h"

#include <cstdio>
#include <cstring>

extern "C" SPI_HandleTypeDef hspi2; /* EMW3080 Wi-Fi module */

namespace comm_wifi
{
namespace
{
/* --- Wi-Fi TCP server state (single client) --- */
bool wifiNetUp = false;      /* joined the AP, has an IP */
int32_t tcpListenFd = -1;
int32_t tcpClientFd = -1;
uint32_t nextAcceptTick = 0;
uint32_t tcpSendFails = 0;

volatile uint8_t wifiLastEvent = 0; /* MWIFI_EVENT_... */

// ここに telemetry.cpp から以下をそのままコピーする（順番もこの通り）:
//   1. constexpr uint16_t mxHtons(uint16_t v)          … telemetry.cpp L228-231
//   2. void wifiStatusCb(uint8_t cate, uint8_t event, void *arg)  … L235-244
//   3. void wifiPinsInit()                              … L145-189
//   4. bool wifiModuleInit()                            … L246-338
//   5. void tcpServerInit()                             … L340-376
//   6. void tcpCloseClient()                            … L378-386

} // namespace

// ここに公開関数(comm_wifi.hpp の6関数)を実装する。§3 の対応を見ること。

} // namespace comm_wifi

/* Wi-FiモジュールのIRQ(EXTI)。telemetry.cpp L1375-1383 からそのまま移す。
 * 【重要】この関数はプロジェクト内で1箇所にしか定義してはいけない。 */
extern "C" void HAL_GPIO_EXTI_Rising_Callback(uint16_t GPIO_Pin)
{
  // telemetry.cpp L1375-1383 の中身をそのままコピー
}
```

## 3. 移動する関数・変数（この表にないものは移動しない）

すべて `Secure/Core/Src/telemetry.cpp` から `Secure/Core/Src/comm_wifi.cpp` へ移す。

| 名前 | 現在の場所（目安） | 移動先での扱い | シグネチャ変更 |
|---|---|---|---|
| `bool wifiNetUp` | L192 | 無名namespace内のstatic変数 | なし |
| `int32_t tcpListenFd` | L193 | 同上 | なし |
| `int32_t tcpClientFd` | L194 | 同上 | なし |
| `uint32_t nextAcceptTick` | L195 | 同上 | なし |
| `uint32_t tcpSendFails` | L196 | 同上 | なし |
| `volatile uint8_t wifiLastEvent` | L233 | 同上（**volatileを保持**） | なし |
| `constexpr uint16_t mxHtons(uint16_t)` | L228-231 | 無名namespace内 | なし |
| `void wifiStatusCb(uint8_t, uint8_t, void*)` | L235-244 | 無名namespace内 | なし |
| `void wifiPinsInit()` | L145-189 | 無名namespace内 | なし |
| `bool wifiModuleInit()` | L246-338 | 無名namespace内 | なし |
| `void tcpServerInit()` | L340-376 | 無名namespace内 | なし |
| `void tcpCloseClient()` | L378-386 | 無名namespace内 | なし |
| `HAL_GPIO_EXTI_Rising_Callback` | L1375-1383 | ファイル末尾（namespace外、`extern "C"`） | なし |

## 4. 公開関数の実装（comm_wifi.cpp内）

上の私有関数を使って、`comm_wifi.hpp` の6関数を実装する。

```cpp
bool Init()
{
  bool ok = wifiModuleInit();
  tcpServerInit();     /* wifiNetUp が false なら中で何もしない */
  return ok;
}

bool NetUp() { return wifiNetUp; }

bool HasClient() { return tcpClientFd >= 0; }

void SendFrame(const uint8_t *frame, size_t len)
{
  /* telemetry.cpp の Service::sendTcp (L755-784) の「Frame_Encode より後」の
   * 部分をそのままここへ持ってくる。つまり:
   *   - tcpClientFd < 0 なら return
   *   - MX_WIFI_Socket_send して、失敗が2回続いたら tcpCloseClient()
   *   - printf("[TLM] TCP send failed ...") もそのまま
   * フレームの組み立て(Frame_Encode)は呼び出し側に残るので、ここではしない。 */
}

void SendRaw(const uint8_t *data, int32_t len)
{
  if (tcpClientFd < 0)
  {
    return;
  }
  (void)MX_WIFI_Socket_send(wifi_obj_get(), tcpClientFd,
                            const_cast<uint8_t *>(data), len, 0);
}

int32_t PollRecv(uint8_t *buf, size_t maxLen)
{
  /* telemetry.cpp の Service::pollTcp (L786-885) から、
   * 「受信バイトを processRxByte に渡すループ(L845-881)」を除いた部分を持ってくる:
   *   - tcpListenFd < 0 なら return 0
   *   - クライアント未接続なら accept を試みる(1Hz制限、RXNEガード、printf全部そのまま)
   *     → 接続できてもできなくても return 0
   *   - クライアント接続中なら MX_WIFI_Socket_recv して、受信バイト数を返す
   * 【重要】末尾のコメント「Note: recv error codes are unreliable...」も移すこと。 */
}
```

## 5. 変更: `telemetry.cpp`

### 5-1. 削除する（上の表で comm_wifi.cpp に移したもの）

file-scope変数6個と関数6個、`HAL_GPIO_EXTI_Rising_Callback`。**表にないものは消さない。**

### 5-2. include を調整する

- **追加**: `#include "comm_wifi.hpp"`
- **削除**: `#include "mx_wifi.h"` と `#include "io_pattern/mx_wifi_io.h"`（comm_wifi.cpp へ移したので不要）
- **削除**: `extern "C" SPI_HandleTypeDef hspi2;`（同上）

### 5-3. 呼び出しを置き換える（この対応表の通りに）

| 変更前 | 変更後 | 場所 |
|---|---|---|
| `Service::sendTcp` 内の `tcpClientFd < 0` チェックと `MX_WIFI_Socket_send`・失敗カウント処理 | `comm_wifi::SendFrame(frame, len);`（Frame_Encodeはそのまま残す） | L755-784 |
| `Service::pollTcp` の accept/recv 部分 | `uint8_t buf[64]; int32_t n = comm_wifi::PollRecv(buf, sizeof(buf));` に置換し、`n > 0` なら**既存のバイトループ（L845-881）をそのまま実行** | L786-885 |
| `pollTcp` のバイトループ内 `MX_WIFI_Socket_send(obj, tcpClientFd, ...)`（PONG応答） | `comm_wifi::SendRaw(reinterpret_cast<uint8_t*>(msg), m);` | L861-862 |
| 同（LED応答） | `comm_wifi::SendRaw(reinterpret_cast<const uint8_t*>(msg), static_cast<int32_t>(strlen(msg)));` | L868-870 |
| `Service::sendResponse` 内の `if (tcpClientFd >= 0) { MX_WIFI_Socket_send(...); }` | `comm_wifi::SendRaw(frame, static_cast<int32_t>(n));` | L904-909 |
| `Service::poll()` 内の音声TCP送信 `if (tcpClientFd >= 0) { MX_WIFI_Socket_send(...); }` | `comm_wifi::SendRaw(audioFrame, static_cast<int32_t>(len));` | L1208-1212 |
| `Service::wifiTcpLinkBits()` の `tcpClientFd >= 0` | `comm_wifi::HasClient()` | L1090 |
| `Service::initRadio()` 冒頭の `wifiModuleInit()` | `comm_wifi::Init()` | L481付近 |
| `Service::initRadio()` 末尾の `tcpServerInit()` | **削除**（`comm_wifi::Init()` が中でやるため） | L481付近 |

**`Service::pollTcp` の変更後の姿**（これが正解の形）:

```cpp
void Service::pollTcp()
{
  uint8_t buf[64];
  int32_t n = comm_wifi::PollRecv(buf, sizeof(buf));
  if (n > 0)
  {
    for (int32_t i = 0; i < n; i++)
    {
      int plain = processRxByte(buf[i], true);
      if (plain < 0)
      {
        /* consumed by the frame layer (OTA & co.), or - when nsDriven - by
         * processRxByte()'s own 'a'/'s' handling + nsCmdPush(). This loop's
         * 'p'/'l'/'a'/'s' below only fires in the legacy (non-NS-driven)
         * App_Main path where processRxByte() returns the byte instead. */
        continue;
      }
      char c = static_cast<char>(plain);
      if (c == 'p' || c == 'P')
      {
        char msg[48];
        int m = snprintf(msg, sizeof(msg), "[TCP] PONG uptime=%lu\r\n", HAL_GetTick());
        comm_wifi::SendRaw(reinterpret_cast<uint8_t *>(msg), m);
      }
      else if (c == 'l' || c == 'L')
      {
        BSP_LED_Toggle(LED_GREEN);
        const char *msg = "[TCP] LED toggled\r\n";
        comm_wifi::SendRaw(reinterpret_cast<const uint8_t *>(msg),
                           static_cast<int32_t>(strlen(msg)));
      }
      else if (c == 'a' || c == 'A')
      {
        setAudioStream(true);
      }
      else if (c == 's' || c == 'S')
      {
        setAudioStream(false);
      }
    }
  }
}
```

**`Service::poll()` 内の `pollTcp()` 呼び出しを囲む `if (telemetryEnabled)` は絶対に外さないこと**（IDLE中にブロッキングacceptが走ると、ウェイクできなくなるバグが再発する）。

---

## 変更してはいけないこと

- `pollTcp` のバイトループ（`processRxByte` を呼ぶ部分）を `comm_wifi.cpp` に移さない。**OTA信頼ルートはServiceに集約したまま**にする
- accept の1Hz制限（`nextAcceptTick = now + 5000U`）、RXNEガード、`static bool warned` の警告1回制限、すべて維持する
- `if (telemetryEnabled)` で `pollTcp()` を囲む構造（IDLE中スキップ）を維持する
- `sendTcp` の「送信失敗2回で切断」ロジックと `printf` の文言を変えない

## よくあるコンパイルエラーと対処

| エラー | 対処 |
|---|---|
| `'MX_WIFI_Socket_send' was not declared` in comm_wifi.cpp | `#include "mx_wifi.h"` があるか確認 |
| `'wifi_obj_get' was not declared` | `#include "io_pattern/mx_wifi_io.h"` があるか確認 |
| `'hspi2' was not declared` in comm_wifi.cpp | `extern "C" SPI_HandleTypeDef hspi2;` を冒頭に書いたか確認 |
| `'huart1' was not declared` in comm_wifi.cpp（RXNEガード用） | `extern UART_HandleTypeDef huart1;` を冒頭に追加。※`::huart1` の `::` は不要になる |
| `multiple definition of 'HAL_GPIO_EXTI_Rising_Callback'` | telemetry.cpp 側から消し忘れている |
| telemetry.cpp で `'tcpClientFd' was not declared` | 5-3 の置換漏れ。`tcpClientFd` を直接参照している箇所が残っている |
| `undefined reference to 'comm_wifi::Init()'` | `comm_wifi.cpp` が `Secure/Core/Src/` **直下**にあるか確認 |

---

## ビルド

```bash
CUBEIDEC="d:/App/ST/STM32CubeIDE_2.1.1/STM32CubeIDE/stm32cubeidec.exe"
HWS="C:/temp/hws_refactor"
REPO="d:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A"

"$CUBEIDEC" --launcher.suppressErrors -nosplash \
  -application org.eclipse.cdt.managedbuilder.core.headlessbuild \
  -data "$HWS" -importAll "$REPO" \
  -build "B-U585I-IOT02A_Secure/Debug"
```

**期待結果**: `Build Finished. 0 errors, 2 warnings`（warningは既存の `g_AudioEvents`/`g_AudioErrors` のみ）

## 書き込みと実機検証

Secureを変更したので**Secureを書き込む**（NonSecureは変更なしなので不要）。

```bash
CLI="d:/App/ST/STM32CubeIDE_2.1.1/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_2.2.400.202601091506/tools/bin/STM32_Programmer_CLI.exe"
REPO="d:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A"

"$CLI" -c port=SWD mode=UR -d "$REPO/Secure/Debug/B-U585I-IOT02A_Secure.elf" -v
"$CLI" -c port=SWD mode=UR -rst
sleep 14

cd "$REPO"
python docs/refactoring/verify_regression.py
```

**期待結果**: `verify_regression.py` が **PASS**

**Wi-Fi固有の追加確認**（このステップでは必須）:

起動ログにWi-Fi初期化の成功が出ることを確認する。

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
    txt = buf.decode('utf-8', errors='replace')
    for line in txt.splitlines():
        if 'TLM' in line or 'WiFi' in line or 'EMW' in line:
            print(line)
"
```

**期待される出力**（リセット直後に実行した場合）:
- `[TLM] EMW3080 FW=V2.3.4 MAC=...` … Wi-Fiモジュールとの通信が生きている
- `[TLM] scan: N APs` … スキャンできている
- `[TLM] radio: BLE=OK WiFi=OK` … 両方生きている

これらが出なければ Wi-Fi の初期化が壊れている。ロールバックすること。

## ロールバック

```bash
cd "d:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A"
git reset --hard HEAD
git clean -fd Secure/Core/Src/comm_wifi.cpp Secure/Core/Inc/comm_wifi.hpp
# 再ビルドしてSecureを書き戻す（Secureが壊れると通信できなくなるので必ず実施）
```

## 完了時のコミット

```bash
cd "d:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A"
git add Secure/Core/
git commit -m "refactor(step3): Wi-Fi/TCPスタックをcomm_wifi.cppへ分離

telemetry.cppからEMW3080制御とTCPソケット操作(約330行)をport層として切り出し。
Wi-Fiモジュールを載せ替えるときはcomm_wifi.cppだけを書き直せばよくなった。
受信バイトをprocessRxByteに渡すループ(OTA信頼ルート)はServiceに残し、
comm_wifiは「バイトを送受信する」下請けに徹する設計。ロジック変更なし。"
```

## 次のステップ
`step4_Secure_BLE分離.md` へ進む。
