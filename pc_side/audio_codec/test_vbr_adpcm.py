"""可変ビットレート ADPCM(非可逆)の健全性テスト。

非可逆なので「ビット完全一致」は検証できない。代わりに:
 - 全 profile で round-trip が例外なく完走し、sample 数が保存されること
 - 各 profile の bitrate/圧縮率が想定順(4bit>3bit>2bit, decim1>decim2)であること
 - ブロック自己完結性: 途中の 1 ブロックを破損しても、後続ブロックが
   自分のヘッダ state から復号でき、破損の影響がそのブロック内に限定されること
 - 緩やかな信号(正弦波)で 4bit の SNR が実用域(>= 25 dB)にあること
"""
from __future__ import annotations

import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import vbr_adpcm as v


def snr_db(orig, dec):
    n = min(len(orig), len(dec))
    se = sum(orig[i] * orig[i] for i in range(n))
    ee = sum((orig[i] - dec[i]) ** 2 for i in range(n)) or 1
    return 10 * math.log10(se / ee) if se else 99.0


def sine(n=16000, f=440, sr=16000, amp=15000):
    return [int(amp * math.sin(2 * math.pi * f * i / sr)) for i in range(n)]


def ramp(n=16000):
    return [((i * 31) % 65536) - 32768 for i in range(n)]


def test_all_profiles_roundtrip_preserves_length():
    sig = sine()
    for name, p in v.PROFILES.items():
        blocks, tds = v.encode(sig, p)
        dec = v.decode(blocks, tds, p, len(sig))
        assert len(dec) == len(sig), f"{name}: length {len(dec)} != {len(sig)}"


def test_bitrate_ordering():
    sig = sine()
    def kbps(p):
        blocks, _ = v.encode(sig, p)
        return sum(len(b) for b in blocks) * 8 / (len(sig) / 16000) / 1000
    # 同じ decim なら bit が多いほど bitrate が高い
    assert kbps(v.PROFILES["ADPCM_4BIT_16K"]) > kbps(v.PROFILES["ADPCM_3BIT_16K"])
    assert kbps(v.PROFILES["ADPCM_3BIT_16K"]) > kbps(v.PROFILES["ADPCM_2BIT_16K"])
    # 同じ bit なら decim2 の方が bitrate が低い
    assert kbps(v.PROFILES["ADPCM_4BIT_16K"]) > kbps(v.PROFILES["ADPCM_4BIT_8K"])


def test_4bit_snr_usable():
    sig = sine()
    p = v.PROFILES["ADPCM_4BIT_16K"]
    blocks, tds = v.encode(sig, p)
    dec = v.decode(blocks, tds, p, len(sig))
    snr = snr_db(sig, dec)
    assert snr >= 25.0, f"4bit SNR {snr:.1f} dB < 25 dB"


def test_block_self_contained():
    """1 ブロックを破損しても後続に波及しないこと(自己完結性)。"""
    sig = ramp()
    p = v.PROFILES["ADPCM_4BIT_16K"]
    blocks, tds = v.encode(sig, p)
    assert len(blocks) >= 4
    # 3 番目のブロックを別ブロックで置換(破損模擬)。
    corrupted = list(blocks)
    corrupted[2] = blocks[0]
    dec_good = v.decode(blocks, tds, p, len(sig))
    dec_bad = v.decode(corrupted, tds, p, len(sig))
    bs = p.block_samples
    # 破損ブロック(index2)より後ろは good と一致(state がヘッダから復元される)。
    start_after = 3 * bs
    mism = sum(1 for i in range(start_after, min(len(dec_good), len(dec_bad)))
               if dec_good[i] != dec_bad[i])
    assert mism == 0, f"corruption leaked into later blocks: {mism} samples differ"


def main():
    tests = [
        test_all_profiles_roundtrip_preserves_length,
        test_bitrate_ordering,
        test_4bit_snr_usable,
        test_block_self_contained,
    ]
    fails = []
    for t in tests:
        try:
            t()
            print(f"  PASS {t.__name__}")
        except AssertionError as e:
            print(f"  FAIL {t.__name__}: {e}")
            fails.append(t.__name__)
    # 各 profile の実測値を表示
    print("\n=== profile 特性(正弦波 440Hz) ===")
    sig = sine()
    print(f"{'profile':<17}{'kbps':>8}{'ratio':>8}{'saved%':>8}{'SNR':>8}")
    for name in ["ADPCM_4BIT_16K", "ADPCM_3BIT_16K", "ADPCM_2BIT_16K",
                 "ADPCM_4BIT_8K", "ADPCM_3BIT_8K", "ADPCM_2BIT_8K"]:
        p = v.PROFILES[name]
        blocks, tds = v.encode(sig, p)
        dec = v.decode(blocks, tds, p, len(sig))
        cb = sum(len(b) for b in blocks)
        ratio = cb / (len(sig) * 2)
        print(f"{name:<17}{cb*8/(len(sig)/16000)/1000:>8.1f}{ratio:>8.3f}"
              f"{(1-ratio)*100:>7.1f}%{snr_db(sig, dec):>8.1f}")
    print()
    if fails:
        print(f"FAIL: {len(fails)} tests")
        return 1
    print("PASS: 全テスト成功")
    return 0


if __name__ == "__main__":
    sys.exit(main())
