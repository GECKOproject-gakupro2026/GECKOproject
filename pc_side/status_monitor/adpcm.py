"""IMA ADPCM 4-bit block codec, bit-compatible with Secure/Core/Src/adpcm.c.

Block layout: [predictor int16 LE][step_index u8][pad u8][packed nibbles...]
Each block is independently decodable.
"""
from __future__ import annotations

import struct

BLOCK_HEADER_SIZE = 4

_STEP_TABLE = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
]

_INDEX_TABLE = [
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
]


def _clamp_index(idx: int) -> int:
    return max(0, min(88, idx))


def _clamp_sample(v: int) -> int:
    return max(-32768, min(32767, v))


def _decode_nibble(predictor: int, step_index: int, nibble: int) -> tuple[int, int]:
    step = _STEP_TABLE[step_index]
    diffq = step >> 3
    if nibble & 4:
        diffq += step
    step >>= 1
    if nibble & 2:
        diffq += step
    step >>= 1
    if nibble & 1:
        diffq += step

    predictor += -diffq if (nibble & 8) else diffq
    predictor = _clamp_sample(predictor)
    step_index = _clamp_index(step_index + _INDEX_TABLE[nibble])
    return predictor, step_index


def decode_block(block: bytes) -> list[int]:
    """Decodes one self-contained ADPCM block into PCM int16 samples."""
    if len(block) < BLOCK_HEADER_SIZE:
        return []

    predictor = struct.unpack_from("<h", block, 0)[0]
    step_index = _clamp_index(block[2])

    samples: list[int] = []
    for byte in block[BLOCK_HEADER_SIZE:]:
        predictor, step_index = _decode_nibble(predictor, step_index, byte & 0x0F)
        samples.append(predictor)
        predictor, step_index = _decode_nibble(predictor, step_index, (byte >> 4) & 0x0F)
        samples.append(predictor)
    return samples
