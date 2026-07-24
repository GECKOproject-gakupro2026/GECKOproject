"""sample.wav を全ロスレス profile で圧縮し、
  (1) 圧縮済みバイナリ(.flt = 限定 FLAC ブロック連結)
  (2) それを復号し直した WAV(16 kHz mono PCM16)
を profile ごとにフォルダへ出力する。

ロスレスなので 3 profile の復号 WAV は同一だが、設計検証として各 profile を
個別に round-trip 出力し、圧縮サイズの違いは .flt / レポートで確認できるようにする。

出力先: pc_side/audio_codec/sample_out/
  sample_LOSSLESS_FAST.flt      圧縮バイナリ
  sample_LOSSLESS_FAST.wav      復号音声(元と bit 完全一致)
  ... BALANCED / MAX も同様
  index.txt                     一覧と各サイズ・圧縮率
"""
from __future__ import annotations

import audioop
import hashlib
import os
import struct
import sys
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import flac_lite as fl
from flac_lite import Profile

TARGET_RATE = 16000
HERE = os.path.dirname(os.path.abspath(__file__))
IN_PATH = os.path.join(HERE, "sample.wav")
OUT_DIR = os.path.join(HERE, "sample_out")

# .flt コンテナ: 各ブロックを [u16 length][block bytes] で連結(自己完結・可変長)。
MAGIC = b"FLT1"


def load_and_normalize(path: str) -> list[int]:
    with wave.open(path, "rb") as w:
        nch, width, rate, nframes = (w.getnchannels(), w.getsampwidth(),
                                     w.getframerate(), w.getnframes())
        raw = w.readframes(nframes)
    if width != 2:
        raw = audioop.lin2lin(raw, width, 2)
        width = 2
    if nch == 2:
        raw = audioop.tomono(raw, 2, 0.5, 0.5)
    elif nch > 2:
        raise SystemExit(f"{nch}ch は未対応")
    if rate != TARGET_RATE:
        raw, _ = audioop.ratecv(raw, 2, 1, rate, TARGET_RATE, None)
    return list(struct.unpack(f"<{len(raw)//2}h", raw))


def pcm_sha256(samples: list[int]) -> str:
    return hashlib.sha256(struct.pack(f"<{len(samples)}h", *samples)).hexdigest()


def write_flt(path: str, blocks: list[bytes], total_samples: int) -> int:
    """圧縮ブロック列を .flt へ書き出す。戻り値=総バイト数。"""
    with open(path, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<I", total_samples))
        f.write(struct.pack("<I", len(blocks)))
        for b in blocks:
            f.write(struct.pack("<H", len(b)))
            f.write(b)
    return os.path.getsize(path)


def write_wav(path: str, samples: list[int], rate: int = TARGET_RATE) -> None:
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack(f"<{len(samples)}h", *samples))


def main() -> int:
    if not os.path.exists(IN_PATH):
        print(f"not found: {IN_PATH}")
        return 1
    os.makedirs(OUT_DIR, exist_ok=True)

    print("normalizing sample.wav -> 16 kHz mono ...")
    samples = load_and_normalize(IN_PATH)
    src_sha = pcm_sha256(samples)
    raw_bytes = len(samples) * 2
    print(f"  {len(samples)} samples ({len(samples)/TARGET_RATE:.1f}s), raw {raw_bytes:,} B")

    index = []
    index.append(f"sample.wav 全 profile 圧縮出力 (16 kHz mono, {len(samples)} samples, "
                 f"{len(samples)/TARGET_RATE:.2f}s)")
    index.append(f"入力 PCM SHA256: {src_sha}")
    index.append(f"raw PCM bytes  : {raw_bytes:,}")
    index.append("")
    index.append(f"{'profile':<20}{'flt_bytes':>12}{'ratio':>8}{'saved%':>9}"
                 f"{'wav_lossless':>14}")
    index.append("-" * 63)

    ok = True
    for prof in (Profile.LOSSLESS_FAST, Profile.LOSSLESS_BALANCED, Profile.LOSSLESS_MAX):
        print(f"  encoding {prof.name} ...", flush=True)
        blocks, _stats = fl.encode_stream(samples, prof)

        flt_path = os.path.join(OUT_DIR, f"sample_{prof.name}.flt")
        flt_bytes = write_flt(flt_path, blocks, len(samples))

        # 復号して WAV 出力(元とビット完全一致するはず)。
        decoded = fl.decode_stream(blocks, len(samples))
        wav_path = os.path.join(OUT_DIR, f"sample_{prof.name}.wav")
        write_wav(wav_path, decoded)

        lossless = (len(decoded) == len(samples) and pcm_sha256(decoded) == src_sha)
        ok = ok and lossless
        ratio = flt_bytes / raw_bytes
        index.append(f"{prof.name:<20}{flt_bytes:>12,}{ratio:>8.3f}"
                     f"{(1-ratio)*100:>8.1f}%{'OK' if lossless else 'FAIL':>14}")
        print(f"    -> {os.path.basename(flt_path)} ({flt_bytes:,} B, "
              f"ratio {ratio:.3f}), {os.path.basename(wav_path)} "
              f"({'lossless' if lossless else 'MISMATCH'})")

    index.append("-" * 63)
    index.append("")
    index.append("注記: .flt は限定 FLAC 圧縮バイナリ([MAGIC][total_samples][nblocks]"
                 "[u16 len + block]*)。")
    index.append("      .wav は .flt を復号し直した音声で、どの profile も入力と")
    index.append("      ビット完全一致(可逆・音質差 0)。圧縮サイズだけが profile で異なる。")

    index_path = os.path.join(OUT_DIR, "index.txt")
    with open(index_path, "w", encoding="utf-8") as f:
        f.write("\n".join(index) + "\n")

    print("\n出力フォルダ:", OUT_DIR)
    for name in sorted(os.listdir(OUT_DIR)):
        size = os.path.getsize(os.path.join(OUT_DIR, name))
        print(f"  {name:<32}{size:>12,} B")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
