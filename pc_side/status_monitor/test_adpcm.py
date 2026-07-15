"""Round-trip self-test for the IMA ADPCM codec (adpcm.py / adpcm.c parity).

Encodes a 1 kHz sine (16 kHz, 4096 samples) with a pure-Python encoder that
mirrors adpcm.c bit-for-bit, decodes it with adpcm.decode_block, and checks
SNR and compression ratio.

Run: python test_adpcm.py
"""
from __future__ import annotations

import math
import struct

import adpcm


def _encode_sample(predictor: int, step_index: int, sample: int) -> tuple[int, int, int]:
    """Mirrors encode_sample() in adpcm.c. Returns (nibble, predictor, step_index)."""
    diff = sample - predictor
    sign = 0
    if diff < 0:
        sign = 8
        diff = -diff

    step = adpcm._STEP_TABLE[step_index]
    diffq = step >> 3
    nibble = 0

    if diff >= step:
        nibble |= 4
        diff -= step
        diffq += step
    step >>= 1
    if diff >= step:
        nibble |= 2
        diff -= step
        diffq += step
    step >>= 1
    if diff >= step:
        nibble |= 1
        diffq += step

    predicted = predictor + (-diffq if sign else diffq)
    predictor = adpcm._clamp_sample(predicted)

    nibble |= sign
    step_index = adpcm._clamp_index(step_index + adpcm._INDEX_TABLE[nibble])

    return nibble, predictor, step_index


def encode_block(pcm: list[int], predictor: int = 0, step_index: int = 0) -> bytes:
    """Mirrors adpcm_encode() in adpcm.c: one self-contained block."""
    header = struct.pack("<hBB", predictor, step_index, 0)
    out = bytearray(header)

    i = 0
    while i < len(pcm):
        lo, predictor, step_index = _encode_sample(predictor, step_index, pcm[i])
        i += 1
        hi = 0
        if i < len(pcm):
            hi, predictor, step_index = _encode_sample(predictor, step_index, pcm[i])
            i += 1
        out.append(lo | (hi << 4))
    return bytes(out)


def main() -> None:
    sample_rate = 16000
    freq = 1000
    count = 4096

    pcm = [
        int(round(12000 * math.sin(2 * math.pi * freq * n / sample_rate)))
        for n in range(count)
    ]

    encoded = encode_block(pcm)
    decoded = adpcm.decode_block(encoded)

    assert len(decoded) == count, f"sample count mismatch: {len(decoded)} != {count}"

    noise_energy = 0.0
    signal_energy = 0.0
    for orig, dec in zip(pcm, decoded):
        signal_energy += orig * orig
        err = orig - dec
        noise_energy += err * err

    snr_db = 10 * math.log10(signal_energy / max(noise_energy, 1e-9))
    compression_ratio = len(encoded) / (count * 2)

    print(f"samples={count} encoded_bytes={len(encoded)} "
          f"pcm_bytes={count * 2}")
    print(f"SNR = {snr_db:.2f} dB (target >= 20 dB)")
    print(f"compression ratio = {compression_ratio:.4f} (target <= 0.27)")

    assert snr_db >= 20.0, f"SNR too low: {snr_db:.2f} dB"
    assert compression_ratio <= 0.27, f"compression ratio too high: {compression_ratio:.4f}"

    print("PASS")


if __name__ == "__main__":
    main()
