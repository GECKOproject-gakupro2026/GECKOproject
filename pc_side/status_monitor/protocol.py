"""Frame protocol (requirement spec section 11.2) shared with the firmware.

Frame: SOF(0xAA) | CMD | SEQ | LEN(2,LE) | PAYLOAD | CRC16(2,LE) | EOF(0x55)
CRC-16/CCITT-FALSE over SOF..PAYLOAD.

CMD 0x01 = full status (133 bytes, UART)
CMD 0x02 = compact status (12 bytes, BLE notification)
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import Iterator, Optional

SOF = 0xAA
EOF = 0x55
CMD_STATUS = 0x01
CMD_STATUS_MINI = 0x02
CMD_AUDIO = 0x03  # 512 x int16 PCM samples (16 kHz mono), little endian
CMD_FW_CHUNK = 0x04     # PC->board: offset u32 + data
CMD_FW_COMPLETE = 0x05  # PC->board: size u32 + crc16 u16
CMD_STATUS_REQ = 0x06   # PC->board: query OTA state
CMD_STATUS_RESP = 0x07  # board->PC: "<BIIHB" state/received/expected/crc/err
CMD_FW_APPLY = 0x08     # PC->board: copy staged NS image to Bank2 and run it
CMD_ACK = 0x7E          # "<BBI" orig_cmd/orig_seq/arg
CMD_NACK = 0x7F         # "<BBB" orig_cmd/orig_seq/error

NACK_ERRORS = {1: "BAD_OFFSET", 2: "ERASE", 3: "WRITE", 4: "VERIFY",
               5: "TOO_LARGE", 6: "BAD_STATE"}
OTA_STATES = {0: "Idle", 1: "Receiving", 2: "Staged", 3: "Error"}

FULL_FMT = "<BBIhHI3h3h3hIHBhh32hBB6I"
FULL_SIZE = struct.calcsize(FULL_FMT)  # 133 (v1)
FULL_FMT_V2 = FULL_FMT + "hHIIBBH3II"
FULL_SIZE_V2 = struct.calcsize(FULL_FMT_V2)  # 165 (v2: + MCU details)
MINI_FMT = "<BhHHHHB"
MINI_SIZE = struct.calcsize(MINI_FMT)  # 12 (v1)
MINI_FMT_V2 = "<BhHHHH3h3h3hhhHhBB"
MINI_SIZE_V2 = struct.calcsize(MINI_FMT_V2)  # 39 (v2: all sensors)

AUDIO_SAMPLE_RATE = 16000


def decode_audio(payload: bytes) -> list[int]:
    """CMD_AUDIO payload -> list of int16 PCM samples."""
    count = len(payload) // 2
    return list(struct.unpack(f"<{count}h", payload[: count * 2]))


def build_frame(cmd: int, seq: int, payload: bytes = b"") -> bytes:
    """Encode a command frame (same wire format the firmware emits)."""
    head = bytes([SOF, cmd, seq & 0xFF, len(payload) & 0xFF, len(payload) >> 8])
    body = head + payload
    crc = crc16_ccitt(body)
    return body + bytes([crc & 0xFF, crc >> 8, EOF])


def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


@dataclass
class Status:
    """Board status; populated fully by CMD_STATUS, partially by CMD_STATUS_MINI."""
    uptime_ms: int = 0
    button: bool = False
    temp_c: float = 0.0
    humidity: float = 0.0
    pressure_hpa: float = 0.0
    acc_mg: tuple = (0, 0, 0)
    gyro_dps: tuple = (0.0, 0.0, 0.0)
    mag_mgauss: tuple = (0, 0, 0)
    light_raw: int = 0
    tof_mm: int = 0
    tof_ok: bool = False
    audio_rms: int = 0
    audio_peak: int = 0
    wave: list = field(default_factory=list)
    ble_alive: bool = False
    wifi_alive: bool = False
    ram_used: int = 0
    ram_total: int = 0
    heap_used: int = 0
    heap_free: int = 0
    flash_used: int = 0
    flash_total: int = 0
    # v2 MCU details
    die_temp_c: float = 0.0
    vdda_mv: int = 0
    sysclk_hz: int = 0
    hclk_hz: int = 0
    reset_cause: int = 0
    cpu_load_pct: int = 0
    flash_kb: int = 0
    uid: str = ""
    idcode: int = 0
    compact: bool = False


def decode_status(cmd: int, payload: bytes) -> Optional[Status]:
    if cmd == CMD_STATUS and len(payload) == FULL_SIZE_V2:
        v = struct.unpack(FULL_FMT_V2, payload)
        st = _full_v1(v)
        st.die_temp_c = v[60] / 100.0
        st.vdda_mv = v[61]
        st.sysclk_hz = v[62]
        st.hclk_hz = v[63]
        st.reset_cause = v[64]
        st.cpu_load_pct = v[65]
        st.flash_kb = v[66]
        st.uid = f"{v[67]:08X}-{v[68]:08X}-{v[69]:08X}"
        st.idcode = v[70]
        return st
    if cmd == CMD_STATUS and len(payload) == FULL_SIZE:
        return _full_v1(struct.unpack(FULL_FMT, payload))
    if cmd == CMD_STATUS_MINI and len(payload) == MINI_SIZE_V2:
        v = struct.unpack(MINI_FMT_V2, payload)
        return Status(
            button=bool(v[0]),
            temp_c=v[1] / 100.0,
            humidity=v[2] / 100.0,
            pressure_hpa=v[3] / 10.0,
            light_raw=v[4],
            tof_mm=v[5],
            acc_mg=(v[6], v[7], v[8]),
            gyro_dps=(v[9] / 10.0, v[10] / 10.0, v[11] / 10.0),
            mag_mgauss=(v[12], v[13], v[14]),
            audio_rms=v[15],
            audio_peak=v[16],
            uptime_ms=v[17] * 1000,
            die_temp_c=v[18] / 100.0,
            ble_alive=bool(v[19] & 1),
            wifi_alive=bool(v[19] & 2),
            tof_ok=bool(v[19] & 4),
            cpu_load_pct=v[20],
            compact=True,
        )
    return _decode_v1_extra(cmd, payload)


def _full_v1(v: tuple) -> Status:
    return Status(
            uptime_ms=v[2],
            button=bool(v[1]),
            temp_c=v[3] / 100.0,
            humidity=v[4] / 100.0,
            pressure_hpa=v[5] / 100.0,
            acc_mg=(v[6], v[7], v[8]),
            gyro_dps=(v[9] / 10.0, v[10] / 10.0, v[11] / 10.0),
            mag_mgauss=(v[12], v[13], v[14]),
            light_raw=v[15],
            tof_mm=v[16],
            tof_ok=bool(v[17]),
            audio_rms=v[18],
            audio_peak=v[19],
            wave=list(v[20:52]),
            ble_alive=bool(v[52]),
            wifi_alive=bool(v[53]),
            ram_used=v[54],
            ram_total=v[55],
            heap_used=v[56],
            heap_free=v[57],
            flash_used=v[58],
            flash_total=v[59],
    )


def _decode_v1_extra(cmd: int, payload: bytes) -> Optional[Status]:
    if cmd == CMD_STATUS_MINI and len(payload) == MINI_SIZE:
        v = struct.unpack(MINI_FMT, payload)
        return Status(
            button=bool(v[0]),
            temp_c=v[1] / 100.0,
            humidity=v[2] / 100.0,
            pressure_hpa=v[3] / 10.0,
            light_raw=v[4],
            tof_mm=v[5],
            audio_rms=v[6] * 128,
            compact=True,
        )
    return None


class FrameParser:
    """Incremental parser: feed() raw bytes, iterate (cmd, seq, payload) frames.
    Non-frame bytes (e.g. firmware printf logs) are collected as text."""

    def __init__(self) -> None:
        self._buf = bytearray()
        self.crc_errors = 0
        self.frames = 0

    def feed(self, data: bytes) -> Iterator[tuple]:
        self._buf.extend(data)
        while True:
            start = self._buf.find(SOF)
            if start < 0:
                yield from self._flush_text(len(self._buf))
                return
            if start > 0:
                yield from self._flush_text(start)
            if len(self._buf) < 5:
                return
            length = self._buf[3] | (self._buf[4] << 8)
            if length > 1024:
                yield from self._flush_text(1)  # false SOF
                continue
            total = 8 + length
            if len(self._buf) < total:
                return
            frame = bytes(self._buf[:total])
            crc_rx = frame[5 + length] | (frame[6 + length] << 8)
            if frame[total - 1] == EOF and crc16_ccitt(frame[: 5 + length]) == crc_rx:
                del self._buf[:total]
                self.frames += 1
                yield ("frame", frame[1], frame[2], frame[5 : 5 + length])
            else:
                self.crc_errors += 1
                yield from self._flush_text(1)  # resync after the false SOF

    def _flush_text(self, n: int) -> Iterator[tuple]:
        if n <= 0:
            return
        chunk = bytes(self._buf[:n])
        del self._buf[:n]
        text = chunk.decode("utf-8", errors="ignore")
        printable = "".join(c for c in text if c.isprintable() or c in "\r\n\t")
        if printable:
            yield ("text", printable)
