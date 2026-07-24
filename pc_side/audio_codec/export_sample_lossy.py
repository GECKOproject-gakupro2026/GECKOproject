"""sample.wav を全 non-lossy(可変ビットレート ADPCM)profile で圧縮し、
圧縮後の音声(復号 WAV)と圧縮バイナリをフォルダへ出力する。

非可逆なので profile ごとに音質が異なる WAV が得られる(聴き比べ可能)。
STM32U575 移植前提の整数 ADPCM(vbr_adpcm)を使用。設計 §3.2 の Opus CBR の
代替(自作・外部依存なし・組込み移植容易)として、bits/sample と decimation で
bitrate を段階制御する。

出力先: pc_side/audio_codec/sample_lossy_out/
  sample_<PROFILE>.vad   圧縮バイナリ
  sample_<PROFILE>.wav   復号音声(16 kHz mono、profile ごとに音質が異なる)
  index.txt              一覧(bitrate / 圧縮率 / SNR)
"""
from __future__ import annotations

import audioop
import math
import os
import struct
import sys
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import vbr_adpcm as v

TARGET_RATE = 16000
HERE = os.path.dirname(os.path.abspath(__file__))
IN_PATH = os.path.join(HERE, "sample.wav")
OUT_DIR = os.path.join(HERE, "sample_lossy_out")
MAGIC = b"VAD1"


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


def snr_db(orig: list[int], dec: list[int]) -> float:
    n = min(len(orig), len(dec))
    sig_e = sum(orig[i] * orig[i] for i in range(n))
    err_e = sum((orig[i] - dec[i]) ** 2 for i in range(n)) or 1
    return 10 * math.log10(sig_e / err_e) if sig_e else 0.0


def write_vad(path: str, blocks: list[bytes], total_ds: int,
              prof: v.ProfileDef, total_16k: int) -> int:
    with open(path, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<BBII", prof.bits, prof.decim, total_ds, total_16k))
        f.write(struct.pack("<I", len(blocks)))
        for b in blocks:
            f.write(struct.pack("<H", len(b)))
            f.write(b)
    return os.path.getsize(path)


def write_wav(path: str, samples: list[int]) -> None:
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(TARGET_RATE)
        w.writeframes(struct.pack(f"<{len(samples)}h", *samples))


def main() -> int:
    if not os.path.exists(IN_PATH):
        print(f"not found: {IN_PATH}")
        return 1
    os.makedirs(OUT_DIR, exist_ok=True)

    print("normalizing sample.wav -> 16 kHz mono ...")
    samples = load_and_normalize(IN_PATH)
    dur = len(samples) / TARGET_RATE
    raw_bytes = len(samples) * 2
    print(f"  {len(samples)} samples ({dur:.1f}s), raw {raw_bytes:,} B")

    index = []
    index.append(f"sample.wav 非可逆圧縮出力 (可変ビットレート ADPCM / STM32U575 移植前提)")
    index.append(f"入力: 16 kHz mono, {len(samples)} samples, {dur:.2f}s, raw {raw_bytes:,} B")
    index.append(f"設計: docs/STM32U575_音声圧縮アルゴリズム設計.md §3.2 の非可逆枠")
    index.append("      (Opus の代替として整数 ADPCM で bitrate を段階制御)")
    index.append("")
    index.append(f"{'profile':<17}{'bits':>5}{'SR':>7}{'kbps':>8}{'ratio':>8}"
                 f"{'saved%':>8}{'SNR(dB)':>9}")
    index.append("-" * 62)

    # 高音質(削減率が小さい)順に並べる。
    order = ["ADPCM_4BIT_16K", "ADPCM_3BIT_16K", "ADPCM_2BIT_16K",
             "ADPCM_4BIT_8K", "ADPCM_3BIT_8K", "ADPCM_2BIT_8K"]
    for pname in order:
        prof = v.PROFILES[pname]
        print(f"  encoding {pname} ...", flush=True)
        blocks, total_ds = v.encode(samples, prof)
        vad_path = os.path.join(OUT_DIR, f"sample_{pname}.vad")
        vad_bytes = write_vad(vad_path, blocks, total_ds, prof, len(samples))

        dec = v.decode(blocks, total_ds, prof, len(samples))
        wav_path = os.path.join(OUT_DIR, f"sample_{pname}.wav")
        write_wav(wav_path, dec)

        ratio = vad_bytes / raw_bytes
        kbps = vad_bytes * 8 / dur / 1000
        sr_eff = TARGET_RATE // prof.decim
        snr = snr_db(samples, dec)
        index.append(f"{pname:<17}{prof.bits:>5}{sr_eff:>7}{kbps:>8.1f}"
                     f"{ratio:>8.3f}{(1-ratio)*100:>7.1f}%{snr:>9.1f}")
        print(f"    -> {os.path.basename(vad_path)} ({vad_bytes:,} B, {kbps:.1f} kbps), "
              f"{os.path.basename(wav_path)} (SNR {snr:.1f} dB)")

    index.append("-" * 62)
    index.append("")
    index.append("注記:")
    index.append("  - 非可逆。profile ごとに音質(SNR)が異なる WAV を出力(聴き比べ用)。")
    index.append("  - bits=量子化ビット幅(4/3/2)、SR=実効サンプルレート(decimation 後)。")
    index.append("  - .vad は自作圧縮バイナリ、.wav はそれを復号し 16 kHz へ戻した音声。")
    index.append("  - 整数演算・static・ブロック自己完結で STM32U575 に移植可能。")

    with open(os.path.join(OUT_DIR, "index.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(index) + "\n")

    print("\n出力フォルダ:", OUT_DIR)
    for name in sorted(os.listdir(OUT_DIR)):
        size = os.path.getsize(os.path.join(OUT_DIR, name))
        print(f"  {name:<30}{size:>12,} B")
    return 0


if __name__ == "__main__":
    sys.exit(main())
