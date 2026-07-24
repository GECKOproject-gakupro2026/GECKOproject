"""限定 FLAC encoder/decoder の round-trip テストハーネス。

設計書 §7(実装順序 1-2)・§6(完全性)・§8(採用試験)に対応:
 - constant / impulse / ramp / 最大最小交互 / white noise / 実録音で round-trip
 - 復号 PCM の SHA-256 と sample 数が入力と完全一致すること(ロスレス保証)
 - white noise で verbatim fallback し圧縮率が ~1.0(膨張しない)こと
 - 全 profile(FAST/BALANCED/MAX)で復号結果が同一であること
 - 圧縮率を記録(実録音)

使い方:
  python test_flac_lite.py            # 合成ベクタ + 見つかった実録音
  python -m pytest test_flac_lite.py  # pytest でも実行可
"""
from __future__ import annotations

import glob
import hashlib
import os
import random
import struct
import sys
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import flac_lite as fl
from flac_lite import Profile, SubframeType


def pcm_sha256(samples: list[int]) -> str:
    """PCM16 little-endian バイト列の SHA-256(設計 §6: byte order 固定)。"""
    return hashlib.sha256(struct.pack(f"<{len(samples)}h", *samples)).hexdigest()


def roundtrip(samples: list[int], profile: Profile
              ) -> tuple[bool, float, dict, str]:
    """符号化→復号し、ロスレス性・圧縮率・サブフレーム種別内訳・復号hashを返す。"""
    blocks, stats = fl.encode_stream(samples, profile)
    decoded = fl.decode_stream(blocks, len(samples))

    dec_hash = pcm_sha256(decoded)
    lossless = (len(decoded) == len(samples) and dec_hash == pcm_sha256(samples))

    comp_bytes = sum(len(b) for b in blocks)
    raw_bytes = len(samples) * 2
    ratio = comp_bytes / raw_bytes if raw_bytes else 1.0

    kinds = {SubframeType.CONSTANT: 0, SubframeType.VERBATIM: 0, SubframeType.FIXED: 0}
    for st in stats:
        kinds[st.subframe_type] += 1
    return lossless, ratio, {k.name: v for k, v in kinds.items()}, dec_hash


# ---------------- 合成テストベクタ(設計 §7-2) ----------------
def vec_constant(n=1000):
    return [1234] * n


def vec_zeros(n=1000):
    return [0] * n


def vec_impulse(n=1000):
    s = [0] * n
    s[n // 2] = 32767
    s[n // 3] = -32768
    return s


def vec_ramp(n=1000):
    # -32768..32767 を巡回する ramp(order2 予測が効きやすい)
    return [((i * 37) % 65536) - 32768 for i in range(n)]


def vec_minmax_alternating(n=1000):
    return [32767 if i % 2 == 0 else -32768 for i in range(n)]


def vec_white_noise(n=4096, seed=1):
    rng = random.Random(seed)
    return [rng.randint(-32768, 32767) for _ in range(n)]


def vec_sine(n=4096, freq=440, sr=16000, amp=20000):
    import math
    return [int(amp * math.sin(2 * math.pi * freq * i / sr)) for i in range(n)]


def vec_short_block(n=5):
    # 末尾 partial block(< 256)の境界検証
    return [100, -200, 300, -400, 500][:n]


SYNTH_VECTORS = {
    "constant": vec_constant(),
    "zeros": vec_zeros(),
    "impulse": vec_impulse(),
    "ramp": vec_ramp(),
    "minmax_alt": vec_minmax_alternating(),
    "white_noise": vec_white_noise(),
    "sine_440": vec_sine(),
    "short_block": vec_short_block(),
}


def load_wav_mono16(path: str, max_samples: int = 80000) -> list[int]:
    with wave.open(path, "rb") as w:
        assert w.getsampwidth() == 2, f"{path}: not 16-bit"
        ch = w.getnchannels()
        n = min(w.getnframes(), max_samples)
        raw = w.readframes(n)
        vals = list(struct.unpack(f"<{len(raw)//2}h", raw))
        if ch > 1:
            vals = vals[::ch]  # 先頭チャンネルのみ
        return vals


# ---------------- テスト本体 ----------------
def _all_profiles():
    return [Profile.LOSSLESS_FAST, Profile.LOSSLESS_BALANCED, Profile.LOSSLESS_MAX]


def run_synthetic() -> list[str]:
    failures = []
    print("=== 合成テストベクタ(round-trip + ロスレス検証) ===")
    print(f"{'vector':<14}{'profile':<20}{'lossless':<10}{'ratio':<8}{'subframes'}")
    saved_metrics = {}
    for name, samples in SYNTH_VECTORS.items():
        # 全 profile で復号結果が入力と一致すること
        decoded_hashes = set()
        for prof in _all_profiles():
            lossless, ratio, kinds, dec_hash = roundtrip(samples, prof)
            decoded_hashes.add(dec_hash)
            flag = "OK" if lossless else "FAIL"
            print(f"{name:<14}{prof.name:<20}{flag:<10}{ratio:<8.3f}{kinds}")
            if not lossless:
                failures.append(f"{name}/{prof.name}: not lossless")
            saved_metrics[(name, prof)] = (ratio, kinds)
        # profile 間で復号 PCM が一致(可逆・音質差 0 の保証)
        if len(decoded_hashes) != 1:
            failures.append(f"{name}: profiles produced different decoded PCM")

    # white noise は verbatim fallback で膨張しない(<= 1.03 = header 分のみ)
    wn_ratio, wn_kinds = saved_metrics[("white_noise", Profile.LOSSLESS_MAX)]
    if wn_ratio > 1.03:
        failures.append(f"white_noise ratio {wn_ratio:.3f} > 1.03 (fallback ineffective)")
    if wn_kinds["VERBATIM"] == 0:
        failures.append("white_noise did not use any VERBATIM subframe")

    # constant は CONSTANT subframe を使い、極端に小さくなる
    c_ratio, c_kinds = saved_metrics[("constant", Profile.LOSSLESS_BALANCED)]
    if c_kinds["CONSTANT"] == 0:
        failures.append("constant did not use CONSTANT subframe")
    if c_ratio > 0.05:
        failures.append(f"constant ratio {c_ratio:.3f} too large")

    return failures


def run_real_recordings() -> list[str]:
    failures = []
    search = [
        os.path.join(os.path.dirname(__file__), "..", "status_monitor", "recordings", "*.wav"),
    ]
    paths = []
    for pat in search:
        paths.extend(sorted(glob.glob(pat)))
    if not paths:
        print("\n(実録音 WAV が見つからないためスキップ)")
        return failures

    print("\n=== 実録音(round-trip + 圧縮率) ===")
    print(f"{'file':<28}{'samples':<9}{'profile':<20}{'lossless':<10}{'ratio':<8}{'saved%'}")
    for path in paths[:6]:
        samples = load_wav_mono16(path)
        base = os.path.basename(path)
        for prof in _all_profiles():
            lossless, ratio, _kinds, _hash = roundtrip(samples, prof)
            saved = (1.0 - ratio) * 100.0
            flag = "OK" if lossless else "FAIL"
            print(f"{base:<28}{len(samples):<9}{prof.name:<20}{flag:<10}{ratio:<8.3f}{saved:5.1f}")
            if not lossless:
                failures.append(f"{base}/{prof.name}: not lossless")
    return failures


def main() -> int:
    random.seed(0)
    failures = []
    failures += run_synthetic()
    failures += run_real_recordings()

    print("\n=== 結果 ===")
    if failures:
        print(f"FAIL: {len(failures)} 件")
        for f in failures:
            print("  -", f)
        return 1
    print("PASS: 全 round-trip でロスレス(SHA-256 一致・sample 数一致)")
    return 0


# pytest フック(assert ベース)
def test_synthetic_lossless():
    assert run_synthetic() == []


def test_real_recordings_lossless():
    assert run_real_recordings() == []


if __name__ == "__main__":
    sys.exit(main())
