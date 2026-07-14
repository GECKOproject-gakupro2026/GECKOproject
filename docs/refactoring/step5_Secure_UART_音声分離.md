# Step 5: Secure UART送信・音声キャプチャ分離（comm_uart.cpp / audio_capture.cpp）

## 目的
2つの独立した部品を切り出す。
- **`comm_uart.cpp`**: VCP(USART1)への非同期送信。UARTを別のペリフェラルに変えるときはここだけ書き直す
- **`audio_capture.cpp`**: マイク(MIC2/MDF1)のDMAキャプチャ。マイクを変えるときはここだけ書き直す

この2つは互いに無関係だが、どちらも小さく独立性が高いので1ステップにまとめる。

## 前提条件
- Step 4 が完了しコミット済み（`verify_regression.py` が PASS）

## 対象ファイル
- **新規**: `Secure\Core\Inc\comm_uart.hpp`
- **新規**: `Secure\Core\Src\comm_uart.cpp`
- **新規**: `Secure\Core\Inc\audio_capture.hpp`
- **新規**: `Secure\Core\Src\audio_capture.cpp`
- **変更**: `Secure\Core\Src\telemetry.cpp`
- **変更**: `Secure\Core\Inc\telemetry.hpp`

---

## パート A: UART送信の分離

### A-1. 新規: `Secure/Core/Inc/comm_uart.hpp`

```cpp
/**
  ******************************************************************************
  * @file    comm_uart.hpp
  * @brief   VCP(USART1)への非同期送信【port層】。
  *
  *          受信(RX)は console.cpp が循環DMAで担当している。ここは送信のみ。
  ******************************************************************************
  */
#ifndef COMM_UART_HPP
#define COMM_UART_HPP

#include <cstddef>
#include <cstdint>

namespace comm_uart
{

/* 割り込み駆動で送る。前の送信が終わるのを最大 waitMs だけ待つ。
 * 戻り値: 送信を開始できたら true、待っても前の送信が終わらなければ false */
bool SendAsync(const uint8_t *data, size_t len, uint32_t waitMs);

} // namespace comm_uart

#endif /* COMM_UART_HPP */
```

### A-2. 新規: `Secure/Core/Src/comm_uart.cpp`

```cpp
/**
  ******************************************************************************
  * @file    comm_uart.cpp
  * @brief   comm_uart.hpp の USART1(VCP) 向け実装【port層】。
  ******************************************************************************
  */
#include "comm_uart.hpp"

#include "frame_codec.h"   /* FRAME_OVERHEAD */
#include "main.h"

#include <cstring>

extern UART_HandleTypeDef huart1; /* VCP console / telemetry stream */

namespace comm_uart
{
namespace
{
/* --- Non-blocking VCP transmit (interrupt driven, single in-flight buffer).
 * telemetry.cpp L107-111 のコメントもそのまま持ってくること --- */
volatile bool uartTxBusy = false;
uint8_t uartTxBuf[1024 + FRAME_OVERHEAD];
} // namespace

bool SendAsync(const uint8_t *data, size_t len, uint32_t waitMs)
{
  // telemetry.cpp の uartSendAsync (L113-135) の中身をそのままコピー
}

} // namespace comm_uart

/* USART1の送信完了割り込み。
 * 【重要】プロジェクト内で1箇所にしか定義してはいけない。 */
extern "C" void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  // telemetry.cpp L1353-1359 の中身をそのままコピー
  // （uartTxBusy を触るので、comm_uart の無名namespace変数が見える位置に置く）
}
```

**注意**: `HAL_UART_TxCpltCallback` は `uartTxBusy`（無名namespace内）を触る。同一ファイル内なので参照できるが、`comm_uart::` を明示する必要はない（無名namespaceはファイルスコープ）。

### A-3. 移動する（UART分）

| 名前 | 現在の場所 | 移動先 | シグネチャ変更 |
|---|---|---|---|
| `volatile bool uartTxBusy` | telemetry.cpp L110 | comm_uart.cpp 無名namespace（**volatile保持**） | なし |
| `uint8_t uartTxBuf[1024 + FRAME_OVERHEAD]` | L111 | 同上 | なし |
| `bool uartSendAsync(...)` | L113-135 | `comm_uart::SendAsync` | 名前のみ変更 |
| `HAL_UART_TxCpltCallback` | L1353-1359 | comm_uart.cpp 末尾 | なし |

---

## パート B: 音声キャプチャの分離

### B-1. 新規: `Secure/Core/Inc/audio_capture.hpp`

```cpp
/**
  ******************************************************************************
  * @file    audio_capture.hpp
  * @brief   マイク音声のDMAキャプチャ(MIC2 / MDF1 / PLL3)【port層】。
  *
  *          マイクやADCを変えるときは audio_capture.cpp だけを書き直す。
  *          呼び出し側は「16bit PCMの循環バッファがある」ことだけを知る。
  ******************************************************************************
  */
#ifndef AUDIO_CAPTURE_HPP
#define AUDIO_CAPTURE_HPP

#include <cstdint>

namespace audio_capture
{

/* マイクを初期化して連続キャプチャを開始する。
 * 戻り値: キャプチャが動き始めたら true */
bool Init();

/* 循環キャプチャバッファの先頭。サンプル数は Samples() で得る。 */
const int16_t *Buffer();

/* バッファのサンプル数（固定値） */
uint32_t Samples();

} // namespace audio_capture

#endif /* AUDIO_CAPTURE_HPP */
```

### B-2. 新規: `Secure/Core/Src/audio_capture.cpp`

```cpp
/**
  ******************************************************************************
  * @file    audio_capture.cpp
  * @brief   audio_capture.hpp の MIC2(MDF1 + GPDMA + PLL3) 向け実装【port層】。
  ******************************************************************************
  */
#include "audio_capture.hpp"

#include "app_config.h"   /* CFG_AUDIO_SAMPLE_RATE */
#include "main.h"

#include "b_u585i_iot02a_audio.h"

#include <cstdio>

/* CubeMX-generated ADF1 handle (main.c), released before the BSP takes over */
extern "C" MDF_HandleTypeDef AdfHandle0;

/* Shared BSP audio DMA event flags */
extern "C" volatile uint32_t g_AudioEvents;
extern "C" volatile uint32_t g_AudioErrors;

namespace audio_capture
{
namespace
{
constexpr size_t kAudioSamples = 2048;   /* circular capture buffer */
int16_t audioBuf[kAudioSamples];

// ここに telemetry.cpp から reselectAudioPll3() をそのままコピー（L391-404）
} // namespace

bool Init()
{
  /* telemetry.cpp の Service::initAudio() (L406-446) の中身をそのままコピーする。
   * ただし:
   *   - `audioOk_ = false;` → ローカル変数 `bool audioOk = false;` に置き換える
   *   - 最後に `return audioOk;` を足す
   *   - それ以外（DMA属性の設定、BSP_AUDIO_IN_Init/Record、printf）は1文字も変えない
   *
   * 【最重要】DMAチャネル属性の設定
   *     HAL_DMA_ConfigChannelAttributes(..., DMA_CHANNEL_SEC | ...)
   *   は TrustZone で苦労した箇所。数値も順序も絶対に変えないこと。 */
}

const int16_t *Buffer() { return audioBuf; }

uint32_t Samples() { return kAudioSamples; }

} // namespace audio_capture

/* AI推論パス用の音声窓（C linkage）。telemetry.cpp L94-99 から移動。 */
extern "C" const int16_t *Telemetry_GetAudioBuffer(uint32_t *count)
{
  *count = audio_capture::Samples();
  return audio_capture::Buffer();
}

/* 音声DMAのイベントフラグ。telemetry.cpp L1301-1302 から移動。
 * 【重要】volatile を落とさないこと。 */
extern "C" volatile uint32_t g_AudioEvents = 0; /* bit0 = half, bit1 = full */
extern "C" volatile uint32_t g_AudioErrors = 0;

/* BSPオーディオのDMAコールバック。telemetry.cpp L1304-1321 から移動。
 * 【重要】プロジェクト内で1箇所にしか定義してはいけない。 */
extern "C" void BSP_AUDIO_IN_HalfTransfer_CallBack(uint32_t Instance)
{
  (void)Instance;
  g_AudioEvents |= 1U;
}

extern "C" void BSP_AUDIO_IN_TransferComplete_CallBack(uint32_t Instance)
{
  (void)Instance;
  g_AudioEvents |= 2U;
}

extern "C" void BSP_AUDIO_IN_Error_CallBack(uint32_t Instance)
{
  (void)Instance;
  g_AudioErrors = g_AudioErrors + 1U;
}
```

**注意**: ファイル冒頭の `extern "C" volatile uint32_t g_AudioEvents;`（宣言）と、末尾の `extern "C" volatile uint32_t g_AudioEvents = 0;`（定義）が両方必要。これは元の telemetry.cpp と同じ構造（だから既存の警告2件も comm_service.cpp から audio_capture.cpp に移るだけ）。

### B-3. 移動する（音声分）

| 名前 | 現在の場所 | 移動先 | シグネチャ変更 |
|---|---|---|---|
| `constexpr size_t kAudioSamples = 2048` | telemetry.cpp L87 | audio_capture.cpp 無名namespace | なし |
| `int16_t audioBuf[kAudioSamples]` | L89 | 同上 | なし |
| `Telemetry_GetAudioBuffer` | L94-99 | audio_capture.cpp 末尾 | なし |
| `static void reselectAudioPll3()` | L391-404 | audio_capture.cpp 無名namespace | なし |
| `Service::initAudio()` | L406-446 | `audio_capture::Init()` | `audioOk_` メンバ → 戻り値 |
| `extern "C" volatile uint32_t g_AudioEvents = 0` | L1301 | audio_capture.cpp 末尾 | なし |
| `extern "C" volatile uint32_t g_AudioErrors = 0` | L1302 | 同上 | なし |
| `BSP_AUDIO_IN_HalfTransfer_CallBack` | L1304-1308 | 同上 | なし |
| `BSP_AUDIO_IN_TransferComplete_CallBack` | L1310-1314 | 同上 | なし |
| `BSP_AUDIO_IN_Error_CallBack` | L1316-1320 | 同上 | なし |

**移さないもの（telemetry.cpp に残す）**:
- `uint8_t audioFrame[1024 + FRAME_OVERHEAD]`（L105）… ストリーミング送信用のバッファ。送信はServiceの仕事
- `Service::setAudioStream()`（L887-892）… ストリーミングのON/OFFはServiceの状態
- `Service::poll()` 内の音声ストリーミング送信ループ（L1177-1216）… フレーム組み立てと送信はServiceの仕事
- `Service::collect()` 内のRMS/peak/波形の計算（L673-697）… テレメトリの中身を作るのはServiceの仕事

---

## パート C: telemetry.cpp / telemetry.hpp の変更

### C-1. include を追加

```cpp
#include "audio_capture.hpp"
#include "comm_uart.hpp"
```

**削除**: `#include "b_u585i_iot02a_audio.h"` と `extern "C" MDF_HandleTypeDef AdfHandle0;`（audio_capture.cpp へ移した）

### C-2. 呼び出しの置換

| 変更前 | 変更後 | 場所 |
|---|---|---|
| `uartSendAsync(frame, n, 20)` | `comm_uart::SendAsync(frame, n, 20)` | sendResponse L916付近 |
| `uartSendAsync(frame, len, 5)` 等（sendUart内） | `comm_uart::SendAsync(...)` | sendUart L700-712 |
| `uartSendAsync(audioFrame, len, 15)` | `comm_uart::SendAsync(audioFrame, len, 15)` | poll L1207付近 |
| `initAudio();`（`Service::init()` 内） | `audioOk_ = audio_capture::Init();` | init L518-544 |
| `collect()` 内の `audioBuf[i]` 参照（L675-695） | `const int16_t *audioBuf = audio_capture::Buffer();` をループ前に置いて、以降はそのまま `audioBuf[i]` | collect |
| `collect()` 内の `kAudioSamples` | `audio_capture::Samples()` | collect |
| `poll()` 内の `&audioBuf[0]` / `&audioBuf[kAudioSamples / 2]` | `audio_capture::Buffer()` と `audio_capture::Samples()` を使って書き換え | poll L1196-1197 |
| `CommBridge_GetAudioBuffer` 内の `telemetry::kAudioSamples` / `telemetry::audioBuf` | `audio_capture::Samples()` / `audio_capture::Buffer()` | L1289-1298 |

**`Service::poll()` の音声ストリーミング部の変更後**:

```cpp
  /* PCM streaming: forward each ready buffer half as two 512-sample frames */
  if (audioStream_)
  {
    uint32_t events = g_AudioEvents;
    if (events != 0U)
    {
      g_AudioEvents = 0;
      const int16_t *buf = audio_capture::Buffer();
      const uint32_t nSamples = audio_capture::Samples();
      const int16_t *half = (events & 1U) != 0U ? &buf[0] : &buf[nSamples / 2];
      for (int part = 0; part < 2; part++)
      {
        size_t len = Frame_Encode(
            FRAME_CMD_AUDIO, audioSeq_++,
            reinterpret_cast<const uint8_t *>(half + part * 512), 1024U,
            audioFrame, sizeof(audioFrame));
        if (len > 0U)
        {
          /* audio must not drop: wait for the in-flight frame (<= 12 ms) */
          (void)comm_uart::SendAsync(audioFrame, len, 15);
          comm_wifi::SendRaw(audioFrame, static_cast<int32_t>(len));
        }
      }
    }
  }
```

### C-3. `telemetry.hpp` の変更

- `private:` の `void initAudio();` 宣言を**削除**
- `bool audioOk_ = false;` メンバは**残す**（`setAudioStream` が使う）

`g_AudioEvents` を telemetry.cpp が参照するので、`extern "C" volatile uint32_t g_AudioEvents;`（**宣言のみ**、`= 0` なし）は telemetry.cpp の冒頭に残すこと。

---

## 変更してはいけないこと

- **`audio_capture::Init()` 内のDMAチャネル属性設定を1文字も変えない**。TrustZoneでDMAが動かず苦労した箇所。`DMA_CHANNEL_SEC` 等のフラグの組み合わせを触ると音声が全ゼロになる
- `reselectAudioPll3()` のPLL3の数値（PLL3M=1, PLL3N=80, PLL3P=28, PLL3Q=28, PLL3R=2）を変えない
- `uartSendAsync` の待ち時間引数（sendResponse=20ms, 音声=15ms）を変えない。音声の15msは「音を落とさないため」の値
- `volatile uint32_t g_AudioEvents` / `g_AudioErrors` の volatile を落とさない

## よくあるコンパイルエラーと対処

| エラー | 対処 |
|---|---|
| `'BSP_AUDIO_IN_Init' was not declared` | audio_capture.cpp に `#include "b_u585i_iot02a_audio.h"` があるか確認 |
| `'AdfHandle0' was not declared` | audio_capture.cpp 冒頭に `extern "C" MDF_HandleTypeDef AdfHandle0;` を書いたか |
| `'CFG_AUDIO_SAMPLE_RATE' undeclared` | `#include "app_config.h"` があるか |
| `multiple definition of 'g_AudioEvents'` | telemetry.cpp に `= 0` 付きの**定義**が残っている。telemetry.cpp 側は `extern` **宣言のみ**にする |
| `multiple definition of 'HAL_UART_TxCpltCallback'` | telemetry.cpp から消し忘れ |
| `multiple definition of 'BSP_AUDIO_IN_*_CallBack'` | 同上 |
| `undefined reference to 'Telemetry_GetAudioBuffer'` | audio_capture.cpp に移し忘れ。AI推論(`ai_app.cpp`)がこれを呼ぶ |
| telemetry.cpp で `'audioBuf' was not declared` | C-2 の置換漏れ |

---

## ビルド・書き込み・検証

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

**期待結果**: **PASS**

**音声固有の追加確認**（このステップでは特に重要。DMAが死ぬと全ゼロになる）:

`verify_regression.py` の項目「音声ストリーム開始」がPASSすることに加え、**マイクに向かって音を出したときにRMSが上がること**を確認する:

```bash
cd "$REPO"
python -c "
import sys, time
sys.path.insert(0, 'pc_side/status_monitor')
import serial, protocol
p = protocol.FrameParser()
print('マイクに向かって声を出してください（5秒間計測）...')
with serial.Serial('COM9', 921600, timeout=0.2) as s:
    t0=time.time(); mx=0
    while time.time()-t0 < 5:
        s.write(b'\x00'); time.sleep(0.3)
        for it in p.feed(s.read(16384)):
            if it[0]=='frame' and it[1]==1:
                st = protocol.decode_status(it[1], it[3])
                if st: mx = max(mx, st.audio_rms)
    print(f'最大RMS = {mx}')
    print('PASS: マイクが生きている' if mx > 500 else 'FAIL: 音声DMAが死んでいる（全ゼロ）')
"
```

**期待結果**: 声を出したとき `最大RMS` が **500以上**（無音時は200前後）。0や極端に低い値なら**DMAが死んでいる**のでロールバックすること。

## ロールバック

```bash
cd "d:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A"
git reset --hard HEAD
git clean -fd Secure/Core/Src/comm_uart.cpp Secure/Core/Inc/comm_uart.hpp \
             Secure/Core/Src/audio_capture.cpp Secure/Core/Inc/audio_capture.hpp
```

## 完了時のコミット

```bash
cd "d:/App/STM32CubeIDE/workspace_2.1.1/B-U585I-IOT02A"
git add Secure/Core/
git commit -m "refactor(step5): UART送信とマイクキャプチャをport層へ分離

comm_uart.cpp: VCP(USART1)の非同期送信とTxCpltコールバック
audio_capture.cpp: MIC2/MDF1のDMAキャプチャ、PLL3設定、音声DMAコールバック群

どちらもハードウェアを載せ替えるときの差し替え単位。フレームの組み立てと
ストリーミング送信の判断(Service)は分離せず残した。DMA属性設定は1文字も
変えていない(TrustZoneで音声が動かなくなる箇所のため)。ロジック変更なし。"
```

## 次のステップ
`step6_MCU情報分離とリネーム.md` へ進む。
