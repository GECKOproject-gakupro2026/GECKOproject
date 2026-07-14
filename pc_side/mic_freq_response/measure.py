"""マイク周波数特性の単発測定（要求4: マイクのオフセット修正のための解析）。

PCから既知周波数のトーンを鳴らし、ボードのマイク(MIC2/MDF1, 16kHz名目)で
録音したCMD_AUDIOフレームをFFT解析して、実際に検出されたピーク周波数から
実効サンプルレートを逆算する。ファームウェアは一切変更しない、測定のみ。

使い方:
    cd <REPO>/pc_side/mic_freq_response
    python measure.py --freq 1000

前提:
    - ボードにSecure/NonSecureを書き込み、リセット済み(12秒以上経過)
    - ST-LINK VCPがCOM9に見えている
    - PCのスピーカーとボードのマイクが近くにあり、正しく音が拾える距離にある
    - 周囲が静かであること(雑音がFFTのピーク検出を妨げる)
"""
from __future__ import annotations

import argparse
import sys
import time
import winsound

import numpy as np

sys.path.insert(0, r"d:\App\STM32CubeIDE\workspace_2.1.1\B-U585I-IOT02A\pc_side\status_monitor")

import serial  # noqa: E402
import protocol  # noqa: E402

PORT = "COM9"
BAUD = 921600
NOMINAL_SAMPLE_RATE = protocol.AUDIO_SAMPLE_RATE  # 16000, ファームの名目レート


def capture_audio_samples(ser: serial.Serial, parser: protocol.FrameParser,
                           duration_s: float, keepalive: bool = True) -> list[int]:
    """duration_s秒間CMD_AUDIOフレームを受信し、PCMサンプルを1本に連結して返す。"""
    samples: list[int] = []
    t_end = time.time() + duration_s
    next_ka = time.time()
    while time.time() < t_end:
        if keepalive and time.time() >= next_ka:
            ser.write(b"\x00")
            next_ka += 1.0
        for item in parser.feed(ser.read(16384)):
            if item[0] == "frame" and item[1] == protocol.CMD_AUDIO:
                samples.extend(protocol.decode_audio(item[3]))
        time.sleep(0.01)
    return samples


def find_peak_freq(samples: list[int], nominal_rate: int) -> tuple[float, np.ndarray, np.ndarray]:
    """PCMサンプル列をFFTし、最大振幅の周波数(名目レート基準)を返す。
    戻り値: (peak_freq_hz, freqs, magnitudes) - freqs/magnitudesはプロット用。"""
    x = np.asarray(samples, dtype=np.float64)
    x = x - np.mean(x)  # DCオフセット除去
    window = np.hanning(len(x))
    spec = np.fft.rfft(x * window)
    freqs = np.fft.rfftfreq(len(x), d=1.0 / nominal_rate)
    mag = np.abs(spec)
    # DC近傍(0〜50Hz)は無視してピークを探す
    mask = freqs > 50.0
    peak_idx = np.argmax(mag[mask])
    peak_freq = freqs[mask][peak_idx]
    return float(peak_freq), freqs, mag


def measure_one(freq_hz: float, tone_duration_ms: int = 2000,
                 capture_s: float = 3.0) -> dict:
    """1周波数分の測定を行う。戻り値: 測定結果の辞書。"""
    parser = protocol.FrameParser()
    with serial.Serial(PORT, BAUD, timeout=0.2) as ser:
        # ボードを起こしてバックログを掃く
        for _ in range(3):
            ser.write(b"\x00")
            time.sleep(0.2)
            ser.read(8192)

        # 音声ストリーミング開始
        ser.write(b"a")
        time.sleep(0.3)
        ser.read(8192)  # 起動直後のバーストは捨てる

        # トーン再生と録音を並走させたいが、winsound.Beepはブロッキングなので
        # 別スレッドで鳴らし、メインスレッドで録音する
        import threading
        beep_thread = threading.Thread(
            target=winsound.Beep, args=(int(freq_hz), tone_duration_ms))
        beep_thread.start()
        samples = capture_audio_samples(ser, parser, capture_s, keepalive=True)
        beep_thread.join()

        ser.write(b"s")
        time.sleep(0.2)
        ser.read(8192)

    if len(samples) < NOMINAL_SAMPLE_RATE // 4:
        return {"freq_in": freq_hz, "ok": False, "reason": f"サンプル数不足({len(samples)})"}

    peak_freq, freqs, mag = find_peak_freq(samples, NOMINAL_SAMPLE_RATE)
    ratio = peak_freq / freq_hz if freq_hz > 0 else float("nan")
    effective_rate = NOMINAL_SAMPLE_RATE * ratio if not np.isnan(ratio) else float("nan")

    return {
        "freq_in": freq_hz,
        "ok": True,
        "n_samples": len(samples),
        "peak_freq_measured": peak_freq,
        "ratio_measured_to_input": ratio,
        "effective_sample_rate_hz": effective_rate,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--freq", type=float, default=1000.0, help="再生するトーン周波数(Hz)")
    ap.add_argument("--tone-ms", type=int, default=2000, help="トーン再生時間(ms)")
    ap.add_argument("--capture-s", type=float, default=3.0, help="録音時間(秒)")
    args = ap.parse_args()

    print(f"{args.freq:.1f} Hz のトーンを {args.tone_ms}ms 再生しながら "
          f"{args.capture_s}秒間録音します...")
    result = measure_one(args.freq, args.tone_ms, args.capture_s)

    if not result["ok"]:
        print(f"[FAIL] {result['reason']}")
        return 1

    print(f"  受信サンプル数        : {result['n_samples']}")
    print(f"  入力周波数            : {result['freq_in']:.1f} Hz")
    print(f"  検出ピーク周波数      : {result['peak_freq_measured']:.1f} Hz "
          f"(名目レート{NOMINAL_SAMPLE_RATE}Hz基準のFFT)")
    print(f"  比率(検出/入力)       : {result['ratio_measured_to_input']:.4f}")
    print(f"  逆算した実効サンプルレート: {result['effective_sample_rate_hz']:.1f} Hz "
          f"(名目 {NOMINAL_SAMPLE_RATE} Hz)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
