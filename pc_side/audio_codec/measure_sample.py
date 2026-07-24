"""sample.wav を設計 §2 の共通入力(16 kHz mono PCM16)へ正規化し、
全ロスレス profile で round-trip + 圧縮率を計測して結果をファイルへ出力する。

44.1 kHz ステレオ等の任意 WAV を、Python 標準ライブラリ(wave + audioop)だけで
16 kHz mono PCM16 へ変換する(ffmpeg 不要)。設計 §3.1 の 3 profile
(LOSSLESS_FAST / BALANCED / MAX)について、ロスレス性(SHA-256 一致)・圧縮率・
サブフレーム種別内訳を表にまとめ、audio_codec/sample_compression_report.txt に書き出す。
"""
from __future__ import annotations

import audioop
import datetime
import hashlib
import os
import struct
import sys
import time
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import flac_lite as fl
from flac_lite import Profile, SubframeType

TARGET_RATE = 16000  # 設計 §2: codec 入力は 16 kHz 固定
IN_PATH = os.path.join(os.path.dirname(__file__), "sample.wav")
OUT_PATH = os.path.join(os.path.dirname(__file__), "sample_compression_report.txt")


def load_and_normalize(path: str) -> tuple[list[int], dict]:
    """任意 WAV を 16 kHz mono PCM16 の int 列へ。元情報も返す。"""
    with wave.open(path, "rb") as w:
        nch = w.getnchannels()
        width = w.getsampwidth()
        rate = w.getframerate()
        nframes = w.getnframes()
        raw = w.readframes(nframes)

    src_info = {
        "path": os.path.basename(path),
        "src_channels": nch,
        "src_sampwidth_bytes": width,
        "src_rate": rate,
        "src_frames": nframes,
        "src_duration_s": nframes / rate if rate else 0.0,
    }

    if width != 2:
        # 8/24/32bit -> 16bit へ変換(audioop.lin2lin)。
        raw = audioop.lin2lin(raw, width, 2)
        width = 2

    # ステレオ -> モノ(左右平均)。
    if nch == 2:
        raw = audioop.tomono(raw, 2, 0.5, 0.5)
    elif nch > 2:
        raise SystemExit(f"{nch}ch は未対応(mono/stereo のみ)")

    # リサンプル -> 16 kHz。
    if rate != TARGET_RATE:
        raw, _ = audioop.ratecv(raw, 2, 1, rate, TARGET_RATE, None)

    samples = list(struct.unpack(f"<{len(raw)//2}h", raw))
    src_info["out_rate"] = TARGET_RATE
    src_info["out_samples"] = len(samples)
    src_info["out_duration_s"] = len(samples) / TARGET_RATE
    return samples, src_info


def pcm_sha256(samples: list[int]) -> str:
    return hashlib.sha256(struct.pack(f"<{len(samples)}h", *samples)).hexdigest()


def measure(samples: list[int], profile: Profile) -> dict:
    t0 = time.time()
    blocks, stats = fl.encode_stream(samples, profile)
    enc_dt = time.time() - t0

    t1 = time.time()
    decoded = fl.decode_stream(blocks, len(samples))
    dec_dt = time.time() - t1

    lossless = (len(decoded) == len(samples)
                and pcm_sha256(decoded) == pcm_sha256(samples))

    comp_bytes = sum(len(b) for b in blocks)
    raw_bytes = len(samples) * 2
    ratio = comp_bytes / raw_bytes if raw_bytes else 1.0

    kinds = {SubframeType.CONSTANT: 0, SubframeType.VERBATIM: 0, SubframeType.FIXED: 0}
    for st in stats:
        kinds[st.subframe_type] += 1

    return {
        "profile": profile.name,
        "lossless": lossless,
        "raw_bytes": raw_bytes,
        "comp_bytes": comp_bytes,
        "ratio": ratio,
        "saved_pct": (1.0 - ratio) * 100.0,
        "blocks": len(blocks),
        "kinds": {k.name: v for k, v in kinds.items()},
        "encode_s": enc_dt,
        "decode_s": dec_dt,
        "decoded_sha256": pcm_sha256(decoded),
    }


def main() -> int:
    if not os.path.exists(IN_PATH):
        print(f"not found: {IN_PATH}")
        return 1

    print(f"loading + normalizing {os.path.basename(IN_PATH)} -> 16 kHz mono PCM16 ...")
    samples, info = load_and_normalize(IN_PATH)
    src_sha = pcm_sha256(samples)
    print(f"  {info['src_channels']}ch {info['src_rate']}Hz {info['src_duration_s']:.1f}s"
          f" -> mono {TARGET_RATE}Hz {info['out_samples']} samples "
          f"({info['out_duration_s']:.1f}s)")

    profiles = [Profile.LOSSLESS_FAST, Profile.LOSSLESS_BALANCED, Profile.LOSSLESS_MAX]
    results = []
    for prof in profiles:
        print(f"  measuring {prof.name} ...", flush=True)
        results.append(measure(samples, prof))

    # 一貫性: 全 profile で復号 PCM が同一・入力と一致。
    all_hashes = {r["decoded_sha256"] for r in results}
    profiles_consistent = (len(all_hashes) == 1 and next(iter(all_hashes)) == src_sha)

    # ---- レポート出力 ----
    now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    lines = []
    lines.append("=" * 78)
    lines.append("sample.wav ロスレス圧縮率レポート (限定 FLAC / STM32U575 リファレンス)")
    lines.append(f"生成: {now}")
    lines.append("設計: docs/STM32U575_音声圧縮アルゴリズム設計.md §3.1")
    lines.append("=" * 78)
    lines.append("")
    lines.append("[入力]")
    lines.append(f"  ファイル        : {info['path']}")
    lines.append(f"  元フォーマット  : {info['src_channels']}ch / {info['src_rate']} Hz"
                 f" / {info['src_sampwidth_bytes']*8}-bit / {info['src_duration_s']:.2f} s")
    lines.append(f"  codec 入力      : mono / {TARGET_RATE} Hz / 16-bit"
                 f" / {info['out_duration_s']:.2f} s ({info['out_samples']} samples)")
    lines.append(f"  raw PCM サイズ  : {info['out_samples']*2:,} bytes")
    lines.append(f"  入力 PCM SHA256 : {src_sha}")
    lines.append("")
    lines.append("[圧縮結果]  ※どの profile も復号 PCM は入力と完全一致(可逆・音質差 0)")
    lines.append("-" * 78)
    header = (f"{'profile':<20}{'lossless':<10}{'comp_bytes':>12}"
              f"{'ratio':>8}{'saved%':>9}{'enc_s':>8}")
    lines.append(header)
    lines.append("-" * 78)
    for r in results:
        lines.append(f"{r['profile']:<20}{'OK' if r['lossless'] else 'FAIL':<10}"
                     f"{r['comp_bytes']:>12,}{r['ratio']:>8.3f}"
                     f"{r['saved_pct']:>8.1f}%{r['encode_s']:>8.2f}")
    lines.append("-" * 78)
    lines.append("")
    lines.append("[サブフレーム種別内訳(ブロック数)]")
    for r in results:
        k = r["kinds"]
        lines.append(f"  {r['profile']:<20} CONSTANT={k['CONSTANT']:<6}"
                     f" VERBATIM={k['VERBATIM']:<6} FIXED={k['FIXED']:<6}"
                     f" (全 {r['blocks']} ブロック)")
    lines.append("")
    lines.append("[整合性]")
    lines.append(f"  全 profile の復号 PCM が入力と一致: "
                 f"{'YES (可逆性検証 PASS)' if profiles_consistent else 'NO (要調査)'}")
    lines.append("")
    lines.append("[注記]")
    lines.append("  - 本コーデックはロスレス(限定 FLAC)。profile は探索範囲のみ変え、")
    lines.append("    音質は不変。圧縮率は音声内容依存(白色雑音等は verbatim で ~1.0)。")
    lines.append("  - 設計 §3.2 の非可逆 Opus CBR profile(64K〜16K)は未実装のため本表に含まない。")
    lines.append("")

    report = "\n".join(lines)
    with open(OUT_PATH, "w", encoding="utf-8") as f:
        f.write(report)

    print("\n" + report)
    print(f"レポートを書き出しました: {OUT_PATH}")
    return 0 if profiles_consistent and all(r["lossless"] for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
