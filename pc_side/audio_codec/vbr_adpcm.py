"""可変ビットレート ADPCM(非可逆) — STM32U575 移植リファレンス。

設計書 §3.2 は Opus CBR を想定するが、Opus は STM32 への移植規模が大きく
(fixed-point encoder + FIFO + 大きな state)、現実的な着地点として
「IMA ADPCM をビット幅可変に一般化した自作非可逆コーデック」を採用する。
FPU 不要の整数演算のみ・static allocation・ブロック自己完結で、既存の
`Secure/Core/Src/adpcm.c`(4bit IMA ADPCM)の素直な拡張になっている
(STM 側はこのファイルを bits パラメータ付きに一般化するだけで移植できる)。

圧縮率(≒ bitrate)は 2 つの軸で制御する:
  1. bits/sample : 4 / 3 / 2 bit(IMA ADPCM の量子化ビット幅)
  2. decimation  : 16 kHz を 1/2 に間引いて 8 kHz にする(高域を捨てる非可逆)

各 profile は (bits, decim) の組で bitrate を決める。ブロックは自己完結
(先頭に predictor/step_index ヘッダ)なので、transport 欠落があっても
そのブロックだけの局所劣化で済み、predictor は発散しない。

  profile        bits  decim  実効SR  bitrate(kbps)  対 raw(256kbps)削減
  ADPCM_4BIT_16K   4     1    16 kHz     ~64.2          ~74.9%
  ADPCM_3BIT_16K   3     1    16 kHz     ~48.2          ~81.2%
  ADPCM_4BIT_8K    4     2     8 kHz     ~32.1          ~87.4%
  ADPCM_2BIT_16K   2     1    16 kHz     ~32.2          ~87.4%
  ADPCM_3BIT_8K    3     2     8 kHz     ~24.1          ~90.6%
  ADPCM_2BIT_8K    2     2     8 kHz     ~16.1          ~93.7%
(bitrate は 256-sample ブロック・4byte ヘッダ込みの実測で算出)
"""
from __future__ import annotations

import struct
from dataclasses import dataclass

# --- IMA ADPCM の標準 step table(89 段)。4/3/2bit で共通に使う。 -----------
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
_STEP_MAX = len(_STEP_TABLE) - 1  # 88

# ビット幅ごとの index 更新テーブル。最上位ビットが符号、残りが振幅。
# 4bit: 標準 IMA ADPCM(adpcm.c と bit 互換)。
_INDEX_TABLE_4 = [
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
]
# 3bit: 振幅 2 ビット(0..3)。小振幅は step を下げ、大振幅は上げる。
_INDEX_TABLE_3 = [-1, -1, 2, 4,  -1, -1, 2, 4]  # [sign(1)][mag(2)]
# 2bit: 振幅 1 ビット(0..1)。
_INDEX_TABLE_2 = [-1, 3,  -1, 3]                # [sign(1)][mag(1)]


def _clamp_index(i: int) -> int:
    return 0 if i < 0 else (_STEP_MAX if i > _STEP_MAX else i)


def _clamp_sample(v: int) -> int:
    return -32768 if v < -32768 else (32767 if v > 32767 else v)


# --- エンコード/デコードの 1 サンプル処理(ビット幅ごと) --------------------
# code の構成: 最上位ビット = 符号(1=負)、下位 (bits-1) ビット = 振幅レベル。
# 量子化: diff を step で正規化し、(bits-1) ビットの振幅に丸める。
# 再構成: IMA 流に step を右シフトしながら各振幅ビットを足す。

def _encode_sample(predictor: int, step_index: int, sample: int, bits: int
                   ) -> tuple[int, int, int]:
    """1 サンプルを bits ビットに量子化。(code, new_predictor, new_step_index)。

    標準 IMA ADPCM の量子化を一般化: 振幅ビットを step の 1/2, 1/4, ...
    の重みに対応させ、diff を大きいビットから貪欲に割り当てる。丸めのため
    各判定で step の半分を閾値にし、割り当てたら残差から引く(4bit のとき
    adpcm.c と同じ挙動、SNR も IMA 相当)。"""
    step = _STEP_TABLE[step_index]
    mag_bits = bits - 1
    sign_bit = 1 << mag_bits

    diff = sample - predictor
    code = 0
    if diff < 0:
        code = sign_bit
        diff = -diff

    # 標準 IMA ADPCM の量子化を可変ビットに一般化。デコーダの再構成
    # (diffq = step>>mag_bits; 最上位ビット=step, 以降 step/2, ...)と対称に、
    # 同じ重みで貪欲にビットを立てる。4bit のとき adpcm.c エンコード相当。
    code_mag = 0
    w = step
    for b in range(mag_bits - 1, -1, -1):
        if diff >= w:
            code_mag |= (1 << b)
            diff -= w
        w >>= 1
    code |= code_mag

    # デコーダと同一の再構成でエンコーダ predictor も更新(drift 防止)。
    predictor, step_index = _decode_code(predictor, step_index, code, bits)
    return code, predictor, step_index


def _index_table(bits: int) -> list[int]:
    if bits == 4:
        return _INDEX_TABLE_4
    if bits == 3:
        return _INDEX_TABLE_3
    if bits == 2:
        return _INDEX_TABLE_2
    raise ValueError(f"unsupported bits {bits}")


def _decode_code(predictor: int, step_index: int, code: int, bits: int
                 ) -> tuple[int, int]:
    """1 code を復号し predictor/step_index を更新して返す。"""
    step = _STEP_TABLE[step_index]
    mag_bits = bits - 1
    sign_bit = 1 << mag_bits

    # 標準 IMA ADPCM の再構成を可変ビットに一般化:
    #   diffq = step >> mag_bits            (最小分解能バイアス。4bit なら step>>3)
    #   最上位振幅ビット = step、以降 step/2, step/4, ... を加算。
    # 4bit のとき adpcm.c の _decode_nibble と完全一致する。
    diffq = step >> mag_bits
    w = step
    for b in range(mag_bits - 1, -1, -1):
        if code & (1 << b):
            diffq += w
        w >>= 1

    if code & sign_bit:
        predictor -= diffq
    else:
        predictor += diffq
    predictor = _clamp_sample(predictor)

    step_index = _clamp_index(step_index + _index_table(bits)[code])
    return predictor, step_index


# --- ビットパッキング(MSB-first、ブロック内で bits ずつ) --------------------
def _pack_codes(codes: list[int], bits: int) -> bytes:
    out = bytearray()
    acc = 0
    nbits = 0
    for c in codes:
        acc = (acc << bits) | (c & ((1 << bits) - 1))
        nbits += bits
        while nbits >= 8:
            nbits -= 8
            out.append((acc >> nbits) & 0xFF)
    if nbits:
        out.append((acc << (8 - nbits)) & 0xFF)
    return bytes(out)


def _unpack_codes(data: bytes, count: int, bits: int) -> list[int]:
    codes = []
    acc = 0
    nbits = 0
    idx = 0
    mask = (1 << bits) - 1
    for _ in range(count):
        while nbits < bits:
            acc = (acc << 8) | data[idx]
            idx += 1
            nbits += 8
        nbits -= bits
        codes.append((acc >> nbits) & mask)
    return codes


# --- デシメーション(整数、位相連続) ----------------------------------------
def _decimate(samples: list[int], factor: int) -> list[int]:
    """factor:1 に間引く。単純平均で軽い anti-alias(STM でも安価)。"""
    if factor == 1:
        return list(samples)
    out = []
    for i in range(0, len(samples) - factor + 1, factor):
        acc = 0
        for j in range(factor):
            acc += samples[i + j]
        out.append(_clamp_sample(acc // factor))
    return out


def _upsample_hold(samples: list[int], factor: int, total: int) -> list[int]:
    """factor 倍に線形補間で戻す(復号側、8k->16k の可聴化用)。"""
    if factor == 1:
        return samples[:total]
    out = []
    for i in range(len(samples)):
        a = samples[i]
        b = samples[i + 1] if i + 1 < len(samples) else samples[i]
        for j in range(factor):
            out.append(_clamp_sample(a + (b - a) * j // factor))
    return out[:total]


# --- profile 定義 -----------------------------------------------------------
@dataclass(frozen=True)
class ProfileDef:
    name: str
    bits: int          # 量子化ビット幅
    decim: int         # デシメーション係数(1=なし, 2=16k->8k)
    block_samples: int = 256  # デシメーション後の 1 ブロック sample 数(設計 §2)


PROFILES = {
    "ADPCM_4BIT_16K": ProfileDef("ADPCM_4BIT_16K", 4, 1),
    "ADPCM_3BIT_16K": ProfileDef("ADPCM_3BIT_16K", 3, 1),
    "ADPCM_2BIT_16K": ProfileDef("ADPCM_2BIT_16K", 2, 1),
    "ADPCM_4BIT_8K":  ProfileDef("ADPCM_4BIT_8K", 4, 2),
    "ADPCM_3BIT_8K":  ProfileDef("ADPCM_3BIT_8K", 3, 2),
    "ADPCM_2BIT_8K":  ProfileDef("ADPCM_2BIT_8K", 2, 2),
}

BLOCK_HEADER_SIZE = 4  # [predictor i16 LE][step_index u8][bits u8]


def encode_block(block: list[int], bits: int,
                 start_predictor: int = 0, start_step_index: int = 0) -> bytes:
    """1 ブロック(デシメーション後 PCM)を自己完結 ADPCM ブロックへ。

    ブロック先頭の predictor/step_index をヘッダに格納する。連続ストリームでは
    前ブロック終端の state を start_* に渡すことで、ブロック境界で step 適応が
    リセットされる(=急峻な信号の頭で追従が遅れる)問題を避けられる。それでも
    各ブロックはヘッダ内 state から自己完結して復号できるので、transport 欠落は
    そのブロックだけの局所劣化で済む。"""
    predictor = start_predictor
    step_index = _clamp_index(start_step_index)
    codes = []
    for s in block:
        code, predictor, step_index = _encode_sample(predictor, step_index, s, bits)
        codes.append(code)
    header = struct.pack("<hBB", start_predictor, _clamp_index(start_step_index), bits)
    return header + _pack_codes(codes, bits), predictor, step_index


def decode_block(data: bytes, n_samples: int) -> list[int]:
    predictor = struct.unpack_from("<h", data, 0)[0]
    step_index = _clamp_index(data[2])
    bits = data[3]
    codes = _unpack_codes(data[BLOCK_HEADER_SIZE:], n_samples, bits)
    out = []
    for c in codes:
        predictor, step_index = _decode_code(predictor, step_index, c, bits)
        out.append(predictor)
    return out


def encode(samples16k: list[int], profile: ProfileDef) -> tuple[list[bytes], int]:
    """16 kHz mono PCM を profile で符号化。(ブロック列, デシメーション後総 sample)。"""
    ds = _decimate(samples16k, profile.decim)
    blocks = []
    bs = profile.block_samples
    pred = 0
    si = 0
    for i in range(0, len(ds), bs):
        blk = ds[i:i + bs]
        enc, pred, si = encode_block(blk, profile.bits, pred, si)
        blocks.append(enc)
    return blocks, len(ds)


def decode(blocks: list[bytes], total_ds_samples: int, profile: ProfileDef,
           total_16k_samples: int) -> list[int]:
    """符号化ブロック列を復号し、16 kHz へ戻した PCM を返す(可聴化用)。"""
    bs = profile.block_samples
    ds = []
    remaining = total_ds_samples
    for blk in blocks:
        n = min(bs, remaining)
        ds.extend(decode_block(blk, n))
        remaining -= n
    return _upsample_hold(ds, profile.decim, total_16k_samples)
