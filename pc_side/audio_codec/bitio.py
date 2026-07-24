"""MSB-first bit writer / reader and exact Rice coding primitives.

STM32U575 移植のリファレンス実装。設計書
`docs/STM32U575_音声圧縮アルゴリズム設計.md` §3 の要件に忠実に:
 - signed residual は符号を fold して unsigned に写像してから Rice 符号化する
 - 符号長計算・bit writer は負数 shift や signed overflow に依存しない明示的な
   uint32 演算を使う(組込みの int32/uint32 と 1:1 に対応させるため)
 - すべて MSB-first(FLAC の bitstream 規約に合わせる)

このモジュールは "限定 FLAC" のコアであり、C への移植時はここが
bit_writer.c / rice.c にほぼそのまま対応する。
"""
from __future__ import annotations

U32 = 0xFFFFFFFF


class BitWriter:
    """MSB-first ビットライター。内部は bytearray。"""

    def __init__(self) -> None:
        self._buf = bytearray()
        self._acc = 0          # 未確定ビットを溜める(最大 <8 ビット)
        self._nbits = 0        # _acc に入っている有効ビット数

    def write_bits(self, value: int, nbits: int) -> None:
        """value の下位 nbits ビットを MSB-first で書く。0 <= nbits <= 32。"""
        if nbits == 0:
            return
        assert 0 <= nbits <= 32
        value &= (1 << nbits) - 1
        # 上位ビットから 1 ビットずつ詰める(移植性優先の明示ループ)。
        for i in range(nbits - 1, -1, -1):
            bit = (value >> i) & 1
            self._acc = (self._acc << 1) | bit
            self._nbits += 1
            if self._nbits == 8:
                self._buf.append(self._acc & 0xFF)
                self._acc = 0
                self._nbits = 0

    def write_unary(self, q: int) -> None:
        """q 個の 0 のあとに 1 を書く(Rice の商部)。"""
        # q ビットの 0 + 1 ビットの 1。大きな q でもまとめて書く。
        while q >= 32:
            self.write_bits(0, 32)
            q -= 32
        # q 個の 0 と終端 1 = 値 1 を (q+1) ビットで表現。
        self.write_bits(1, q + 1)

    def write_rice(self, u: int, k: int) -> None:
        """非負整数 u を Rice(k) で書く: 商を unary、剰余を k ビット。"""
        q = u >> k
        self.write_unary(q)
        if k > 0:
            self.write_bits(u & ((1 << k) - 1), k)

    def align_to_byte(self, pad_bit: int = 0) -> None:
        """バイト境界まで pad_bit で埋める。"""
        while self._nbits != 0:
            self.write_bits(pad_bit & 1, 1)

    def bit_length(self) -> int:
        return len(self._buf) * 8 + self._nbits

    def getvalue(self) -> bytes:
        """バイト境界に 0 埋めして確定バイト列を返す(元の内部状態は保つ)。"""
        out = bytearray(self._buf)
        if self._nbits != 0:
            out.append((self._acc << (8 - self._nbits)) & 0xFF)
        return bytes(out)


class BitReader:
    """MSB-first ビットリーダー。"""

    def __init__(self, data: bytes) -> None:
        self._data = data
        self._pos = 0  # ビット位置

    def read_bits(self, nbits: int) -> int:
        if nbits == 0:
            return 0
        assert nbits <= 32
        v = 0
        for _ in range(nbits):
            byte = self._data[self._pos >> 3]
            bit = (byte >> (7 - (self._pos & 7))) & 1
            v = (v << 1) | bit
            self._pos += 1
        return v

    def read_unary(self) -> int:
        q = 0
        while True:
            byte = self._data[self._pos >> 3]
            bit = (byte >> (7 - (self._pos & 7))) & 1
            self._pos += 1
            if bit == 1:
                return q
            q += 1

    def read_rice(self, k: int) -> int:
        q = self.read_unary()
        r = self.read_bits(k) if k > 0 else 0
        return (q << k) | r

    def align_to_byte(self) -> None:
        if self._pos & 7:
            self._pos = (self._pos + 7) & ~7

    def bit_pos(self) -> int:
        return self._pos


def zigzag_encode(x: int) -> int:
    """signed -> unsigned の fold(FLAC の Rice 前処理と同じ zigzag)。
    0,-1,1,-2,2,... -> 0,1,2,3,4,...  負数 shift に依存しない実装。"""
    if x >= 0:
        return (x << 1) & U32 if x < (1 << 31) else (x << 1)
    return ((-x) << 1) - 1


def zigzag_decode(u: int) -> int:
    if (u & 1) == 0:
        return u >> 1
    return -((u + 1) >> 1)


def rice_bits_exact(residual: list[int], k: int) -> int:
    """residual(signed)を Rice(k) で符号化したときの正確な総ビット数。
    各値: unary 商部 (u>>k)+1 ビット + 剰余 k ビット。"""
    total = 0
    for x in residual:
        u = zigzag_encode(x)
        total += (u >> k) + 1 + k
    return total


def best_rice_k(residual: list[int], k_max: int = 14) -> tuple[int, int]:
    """residual に対して最小ビット数となる k と、そのビット数を返す。"""
    best_k = 0
    best_bits = None
    for k in range(0, k_max + 1):
        bits = rice_bits_exact(residual, k)
        if best_bits is None or bits < best_bits:
            best_bits = bits
            best_k = k
    return best_k, best_bits
