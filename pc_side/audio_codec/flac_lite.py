"""限定 FLAC エンコーダ / デコーダ(STM32U575 移植リファレンス)。

設計書 `docs/STM32U575_音声圧縮アルゴリズム設計.md` §3 のアルゴリズムを
Python で忠実に実装する。完全な RFC 9639 コンテナではなく、同 §4 の
transport payload に載せる「自己完結した 256-sample サブフレーム」を対象とする
(STM 側 encoder が生成し、PC assembler が最終 .flac を組み立てる想定。ここでは
その中核である fixed-predictor + Rice のロスレス性を round-trip で検証する)。

各ブロックの符号化方式は 3 種:
  CONSTANT : 全 sample 同値。値のみ格納。
  VERBATIM : PCM16 をそのまま格納(圧縮できないとき / white noise の fallback)。
  FIXED    : fixed predictor(order 0..4) の残差を Rice(k) 符号化。

profile は探索範囲だけを変える。復号 PCM はどの profile でも完全に同一
(可逆・音質差 0)。設計 §3.1 の 3 profile を実装する。
"""
from __future__ import annotations

from dataclasses import dataclass
from enum import IntEnum

from bitio import (
    BitReader,
    BitWriter,
    best_rice_k,
    rice_bits_exact,
)

BLOCK_SIZE = 256  # 設計 §2: 256 sample 固定
SAMPLE_BITS = 16  # PCM16
RICE_K_MAX = 14

# サブフレーム種別(2 ビットで格納)。
class SubframeType(IntEnum):
    CONSTANT = 0
    VERBATIM = 1
    FIXED = 2


class Profile(IntEnum):
    LOSSLESS_FAST = 0      # predictor order 0..1
    LOSSLESS_BALANCED = 1  # predictor order 0..2
    LOSSLESS_MAX = 2       # predictor order 0..4


# profile -> 探索する fixed predictor order の範囲(上限)。
_PROFILE_MAX_ORDER = {
    Profile.LOSSLESS_FAST: 1,
    Profile.LOSSLESS_BALANCED: 2,
    Profile.LOSSLESS_MAX: 4,
}


def _fixed_residual(block: list[int], order: int) -> list[int]:
    """order 次の fixed predictor 残差を int で返す(先頭 order 個は warm-up)。
    FLAC の固定予測係数:
      0: r[i] = x[i]
      1: r[i] = x[i] - x[i-1]
      2: r[i] = x[i] - 2 x[i-1] + x[i-2]
      3: r[i] = x[i] - 3 x[i-1] + 3 x[i-2] - x[i-3]
      4: r[i] = x[i] - 4 x[i-1] + 6 x[i-2] - 4 x[i-3] + x[i-4]
    warm-up の order 個は残差を作らず、生値を別途格納する。"""
    n = len(block)
    if order == 0:
        return list(block)
    res = []
    if order == 1:
        for i in range(1, n):
            res.append(block[i] - block[i - 1])
    elif order == 2:
        for i in range(2, n):
            res.append(block[i] - 2 * block[i - 1] + block[i - 2])
    elif order == 3:
        for i in range(3, n):
            res.append(block[i] - 3 * block[i - 1] + 3 * block[i - 2] - block[i - 3])
    elif order == 4:
        for i in range(4, n):
            res.append(block[i] - 4 * block[i - 1] + 6 * block[i - 2]
                       - 4 * block[i - 3] + block[i - 4])
    else:
        raise ValueError(f"unsupported order {order}")
    return res


def _reconstruct_fixed(warmup: list[int], residual: list[int], order: int) -> list[int]:
    """warm-up と残差から原 PCM を復元する(エンコーダの逆演算)。"""
    if order == 0:
        return list(residual)
    x = list(warmup)
    if order == 1:
        for r in residual:
            x.append(r + x[-1])
    elif order == 2:
        for r in residual:
            x.append(r + 2 * x[-1] - x[-2])
    elif order == 3:
        for r in residual:
            x.append(r + 3 * x[-1] - 3 * x[-2] + x[-3])
    elif order == 4:
        for r in residual:
            x.append(r + 4 * x[-1] - 6 * x[-2] + 4 * x[-3] - x[-4])
    else:
        raise ValueError(f"unsupported order {order}")
    return x


# --- サブフレーム header のビット数 --------------------------------------
# [type:2][order:3] の 5 ビット固定 header。VERBATIM/CONSTANT では order=0。
_SUBFRAME_HEADER_BITS = 2 + 3


def _warmup_bits(order: int) -> int:
    return order * SAMPLE_BITS


def _fixed_cost_bits(block: list[int], order: int) -> tuple[int, int]:
    """order の fixed 符号化コスト(header 込み総ビット数)と最良 k を返す。"""
    residual = _fixed_residual(block, order)
    k, rbits = best_rice_k(residual, RICE_K_MAX)
    total = _SUBFRAME_HEADER_BITS + _warmup_bits(order) + 4 + rbits  # +4 = k を 4 ビットで格納
    return total, k


def _verbatim_cost_bits(block: list[int]) -> int:
    return _SUBFRAME_HEADER_BITS + len(block) * SAMPLE_BITS


def _all_equal(block: list[int]) -> bool:
    first = block[0]
    for v in block:
        if v != first:
            return False
    return True


@dataclass
class BlockStats:
    subframe_type: SubframeType
    order: int
    rice_k: int
    bits: int


def encode_block(block: list[int], profile: Profile = Profile.LOSSLESS_BALANCED
                 ) -> tuple[bytes, BlockStats]:
    """256 sample のブロックを 1 サブフレームへ符号化。(bytes, stats) を返す。
    bytes は byte-aligned(各ブロック自己完結、transport で扱いやすくするため)。"""
    n = len(block)
    if n == 0:
        raise ValueError("empty block")
    for v in block:
        if not (-32768 <= v <= 32767):
            raise ValueError(f"sample out of PCM16 range: {v}")

    bw = BitWriter()

    # 1. CONSTANT
    if _all_equal(block):
        bw.write_bits(int(SubframeType.CONSTANT), 2)
        bw.write_bits(0, 3)
        bw.write_bits(block[0] & 0xFFFF, SAMPLE_BITS)
        bw.align_to_byte()
        return bw.getvalue(), BlockStats(SubframeType.CONSTANT, 0, 0, bw.bit_length())

    # 2. 各候補のコストを比較(verbatim を暫定 best)。
    best_bits = _verbatim_cost_bits(block)
    best_type = SubframeType.VERBATIM
    best_order = 0
    best_k = 0

    for order in range(0, _PROFILE_MAX_ORDER[profile] + 1):
        if order >= n:
            break
        cost, k = _fixed_cost_bits(block, order)
        if cost < best_bits:
            best_bits = cost
            best_type = SubframeType.FIXED
            best_order = order
            best_k = k

    # 3. 出力。
    if best_type == SubframeType.VERBATIM:
        bw.write_bits(int(SubframeType.VERBATIM), 2)
        bw.write_bits(0, 3)
        for v in block:
            bw.write_bits(v & 0xFFFF, SAMPLE_BITS)
        bw.align_to_byte()
        return bw.getvalue(), BlockStats(SubframeType.VERBATIM, 0, 0, bw.bit_length())

    # FIXED
    bw.write_bits(int(SubframeType.FIXED), 2)
    bw.write_bits(best_order, 3)
    for i in range(best_order):
        bw.write_bits(block[i] & 0xFFFF, SAMPLE_BITS)  # warm-up 生値
    bw.write_bits(best_k, 4)
    residual = _fixed_residual(block, best_order)
    for x in residual:
        bw.write_rice(_zz(x), best_k)
    bw.align_to_byte()
    return bw.getvalue(), BlockStats(SubframeType.FIXED, best_order, best_k, bw.bit_length())


def _zz(x: int) -> int:
    """zigzag(bitio と同じだが Rice writer は非負を要求するのでここで fold)。"""
    return (x << 1) if x >= 0 else (((-x) << 1) - 1)


def _unzz(u: int) -> int:
    return (u >> 1) if (u & 1) == 0 else -((u + 1) >> 1)


def _to_signed16(v: int) -> int:
    return v - 0x10000 if v & 0x8000 else v


def decode_block(data: bytes, block_size: int = BLOCK_SIZE) -> list[int]:
    """encode_block の逆。block_size = そのブロックの sample 数(末尾ブロックは短い)。"""
    br = BitReader(data)
    stype = SubframeType(br.read_bits(2))
    order = br.read_bits(3)

    if stype == SubframeType.CONSTANT:
        v = _to_signed16(br.read_bits(SAMPLE_BITS))
        return [v] * block_size

    if stype == SubframeType.VERBATIM:
        return [_to_signed16(br.read_bits(SAMPLE_BITS)) for _ in range(block_size)]

    # FIXED
    warmup = [_to_signed16(br.read_bits(SAMPLE_BITS)) for _ in range(order)]
    k = br.read_bits(4)
    n_res = block_size - order
    residual = [_unzz(br.read_rice(k)) for _ in range(n_res)]
    return _reconstruct_fixed(warmup, residual, order)


# --- ストリーム(複数ブロック)ヘルパ ------------------------------------
def encode_stream(samples: list[int], profile: Profile = Profile.LOSSLESS_BALANCED
                  ) -> tuple[list[bytes], list[BlockStats]]:
    """PCM16 列を 256-sample ブロックに切って符号化。ブロック列と統計を返す。"""
    blocks = []
    stats = []
    for i in range(0, len(samples), BLOCK_SIZE):
        blk = samples[i:i + BLOCK_SIZE]
        enc, st = encode_block(blk, profile)
        blocks.append(enc)
        stats.append(st)
    return blocks, stats


def decode_stream(blocks: list[bytes], total_samples: int) -> list[int]:
    """encode_stream の逆。total_samples で末尾ブロックの長さを決める。"""
    out = []
    remaining = total_samples
    for enc in blocks:
        bs = min(BLOCK_SIZE, remaining)
        out.extend(decode_block(enc, bs))
        remaining -= bs
    return out
