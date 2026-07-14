"""マイク周波数特性のスイープ測定（要求4）。

複数の周波数で measure.py の measure_one() を呼び出し、検出周波数/入力周波数の
比が周波数によらず一定かどうかを見る。
    - 比がほぼ一定 -> 純粋なサンプルレートのズレ(クロック設定の問題)
    - 比が周波数によって変わる -> デシメーションフィルタ等、別要因の関与を疑う

使い方:
    cd <REPO>/pc_side/mic_freq_response
    python sweep.py
    python sweep.py --freqs 200 500 1000 2000 4000
    python sweep.py --out sweep_result.csv
"""
from __future__ import annotations

import argparse
import csv
import sys
import time

from measure import measure_one, NOMINAL_SAMPLE_RATE

DEFAULT_FREQS = [200, 300, 500, 700, 1000, 1500, 2000, 3000, 4000]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--freqs", type=float, nargs="+", default=DEFAULT_FREQS,
                     help="スイープする周波数のリスト(Hz)")
    ap.add_argument("--tone-ms", type=int, default=2000)
    ap.add_argument("--capture-s", type=float, default=3.0)
    ap.add_argument("--out", type=str, default=None, help="結果をCSVに保存するパス")
    args = ap.parse_args()

    results = []
    for f in args.freqs:
        print(f"\n=== {f:.0f} Hz ===")
        r = measure_one(f, args.tone_ms, args.capture_s)
        if not r["ok"]:
            print(f"  [FAIL] {r['reason']}")
        else:
            print(f"  peak={r['peak_freq_measured']:.1f}Hz "
                  f"ratio={r['ratio_measured_to_input']:.4f} "
                  f"eff_rate={r['effective_sample_rate_hz']:.1f}Hz")
        results.append(r)
        time.sleep(0.5)  # ボード側のtelemetry/audio切り替えに余裕を持たせる

    ok_results = [r for r in results if r["ok"]]
    print("\n=== スイープ結果まとめ ===")
    print(f"{'freq_in':>10} {'peak_meas':>10} {'ratio':>8} {'eff_rate':>10}")
    for r in ok_results:
        print(f"{r['freq_in']:>10.1f} {r['peak_freq_measured']:>10.1f} "
              f"{r['ratio_measured_to_input']:>8.4f} {r['effective_sample_rate_hz']:>10.1f}")

    if len(ok_results) >= 2:
        ratios = [r["ratio_measured_to_input"] for r in ok_results]
        ratio_mean = sum(ratios) / len(ratios)
        ratio_std = (sum((x - ratio_mean) ** 2 for x in ratios) / len(ratios)) ** 0.5
        print(f"\n比の平均={ratio_mean:.4f} 標準偏差={ratio_std:.4f} "
              f"(標準偏差/平均={ratio_std / ratio_mean * 100:.2f}%)")
        if ratio_std / ratio_mean < 0.02:
            print("=> 比がほぼ一定。純粋なサンプルレートのズレの可能性が高い。")
            print(f"   推定実効サンプルレート: {NOMINAL_SAMPLE_RATE * ratio_mean:.1f} Hz "
                  f"(名目 {NOMINAL_SAMPLE_RATE} Hz)")
        else:
            print("=> 比が周波数依存。デシメーションフィルタ等、別要因の関与を疑うこと。")

    if args.out:
        with open(args.out, "w", newline="", encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=[
                "freq_in", "ok", "n_samples", "peak_freq_measured",
                "ratio_measured_to_input", "effective_sample_rate_hz", "reason"])
            writer.writeheader()
            for r in results:
                writer.writerow({k: r.get(k, "") for k in writer.fieldnames})
        print(f"\n結果を {args.out} に保存しました")

    return 0


if __name__ == "__main__":
    sys.exit(main())
