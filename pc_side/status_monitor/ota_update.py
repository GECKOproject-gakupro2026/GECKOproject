"""OTA firmware upload tool for the B-U585I-IOT02A telemetry firmware.

Sends a binary image over UART (ST-LINK VCP) or Wi-Fi (TCP) using the
11.2-section command frames (FW_CHUNK / FW_COMPLETE with per-chunk ACK,
CRC-16 verified end to end). The board stages the image in its external
NOR flash firmware area B (0x200000); applying it to internal flash is
handled by the (future) bootloader step.

Usage:
  python ota_update.py firmware.bin --port COM9
  python ota_update.py firmware.bin --tcp 192.168.4.1:5000
  python ota_update.py --status --port COM9      (query staging state)

Notes:
  - Stop audio streaming first ('s') to keep the link quiet.
  - Chunk ACK timeout covers the on-demand 64 KB block erases (~0.4 s).
"""
from __future__ import annotations

import argparse
import pathlib
import struct
import sys
import time

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
sys.path.insert(0, str(pathlib.Path(__file__).parent))
import protocol  # noqa: E402

CHUNK_DATA = 1008          # payload = 4B offset + data -> 1012 <= 1024
ACK_TIMEOUT = 1.5          # covers a 64KB erase before the write
COMPLETE_TIMEOUT = 30.0    # full read-back CRC verify on the board
RETRIES = 3                # 11.2: resend limit


class Link:
    """Byte link over serial COM port or TCP socket."""

    def __init__(self, port: str | None, tcp: str | None) -> None:
        if port:
            import serial
            self._ser = serial.Serial(port, 921600, timeout=0.05)
            self._sock = None
            self.name = f"UART {port}"
            self._drain_startup_backlog()
        elif tcp:
            import socket
            host, _, p = tcp.partition(":")
            self._sock = socket.create_connection((host, int(p or "5000")), timeout=8)
            self._sock.settimeout(0.05)
            self._ser = None
            self.name = f"TCP {tcp}"
        else:
            raise SystemExit("--port か --tcp のどちらかを指定してください")

    def _drain_startup_backlog(self) -> None:
        """Windows' USB-CDC host buffering can hold telemetry bytes that
        reset_input_buffer() alone doesn't flush in one call - keep reading
        until the link has been quiet for a short stretch before sending
        the first OTA frame, or an early ACK gets lost in the backlog."""
        self._ser.reset_input_buffer()
        quiet_for = 0.0
        deadline = time.time() + 1.5
        drained = 0
        while time.time() < deadline and quiet_for < 0.3:
            d = self._ser.read(4096)
            drained += len(d)
            quiet_for = quiet_for + 0.05 if not d else 0.0
        print(f"  [drain] flushed {drained} startup backlog bytes")

    def write(self, data: bytes) -> None:
        if self._ser:
            self._ser.write(data)
        else:
            self._sock.sendall(data)

    def read(self) -> bytes:
        try:
            if self._ser:
                # Drain everything currently queued, not just one 4 KB
                # chunk: at 50 Hz telemetry (~8 KB/s) a single OTA ACK-wait
                # loop iteration can otherwise fall behind the link and
                # never catch up to the ACK sitting behind the backlog.
                n = self._ser.in_waiting
                return self._ser.read(max(n, 1))
            return self._sock.recv(65536)
        except TimeoutError:
            return b""
        except Exception:  # noqa: BLE001 - socket.timeout etc.
            return b""


def wait_reply(link: Link, parser: protocol.FrameParser, orig_cmd: int,
               orig_seq: int, timeout: float):
    """Waits for ACK/NACK matching (orig_cmd, orig_seq); telemetry/audio
    frames arriving in between are simply skipped."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        data = link.read()
        if not data:
            continue
        for item in parser.feed(data):
            if item[0] != "frame":
                continue
            cmd, _seq, body = item[1], item[2], item[3]
            if cmd == protocol.CMD_ACK and len(body) >= 6:
                o_cmd, o_seq, arg = struct.unpack("<BBI", body[:6])
                if o_cmd == orig_cmd and o_seq == orig_seq:
                    return ("ack", arg)
            elif cmd == protocol.CMD_NACK and len(body) >= 3:
                o_cmd, o_seq, err = struct.unpack("<BBB", body[:3])
                if o_cmd == orig_cmd and o_seq == orig_seq:
                    return ("nack", err)
            elif cmd == protocol.CMD_STATUS_RESP and orig_cmd == protocol.CMD_STATUS_REQ:
                return ("status", body)
    return ("timeout", None)


def send_with_retry(link: Link, parser: protocol.FrameParser, cmd: int,
                    seq: int, payload: bytes, timeout: float):
    for attempt in range(1 + RETRIES):
        link.write(protocol.build_frame(cmd, seq, payload))
        kind, arg = wait_reply(link, parser, cmd, seq, timeout)
        if kind == "ack" or kind == "status":
            return kind, arg
        if kind == "nack":
            name = protocol.NACK_ERRORS.get(arg, str(arg))
            raise SystemExit(f"\nNACK ({name}) at cmd=0x{cmd:02X} seq={seq}")
        if attempt < RETRIES:
            print(f"\n  timeout, retry {attempt + 1}/{RETRIES}")
    raise SystemExit(f"\nno reply after {RETRIES} retries (cmd=0x{cmd:02X})")


def query_status(link: Link, parser: protocol.FrameParser) -> None:
    kind, body = send_with_retry(link, parser, protocol.CMD_STATUS_REQ, 0, b"", 2.0)
    state, received, expected, crc, err = struct.unpack("<BIIHB", body[:12])
    print(f"OTA state   : {protocol.OTA_STATES.get(state, state)}")
    print(f"received    : {received} bytes")
    print(f"staged size : {expected} bytes")
    print(f"image crc16 : 0x{crc:04X}")
    print(f"last error  : {protocol.NACK_ERRORS.get(err, err) if err else '-'}")


def main() -> None:
    ap = argparse.ArgumentParser(description="B-U585I OTA firmware upload")
    ap.add_argument("image", nargs="?", help="firmware .bin to stage")
    ap.add_argument("--port", help="serial port (e.g. COM9)")
    ap.add_argument("--tcp", help="host:port (e.g. 192.168.4.1:5000)")
    ap.add_argument("--status", action="store_true", help="query staging state only")
    args = ap.parse_args()

    link = Link(args.port, args.tcp)
    parser = protocol.FrameParser()
    print(f"link: {link.name}")

    if args.status:
        query_status(link, parser)
        return
    if not args.image:
        raise SystemExit("firmware .bin を指定してください（または --status）")

    data = pathlib.Path(args.image).read_bytes()
    crc = protocol.crc16_ccitt(data)
    total = len(data)
    print(f"image: {args.image} ({total} bytes, crc16=0x{crc:04X})")

    t0 = time.time()
    seq = 0
    for offset in range(0, total, CHUNK_DATA):
        chunk = data[offset:offset + CHUNK_DATA]
        payload = struct.pack("<I", offset) + chunk
        send_with_retry(link, parser, protocol.CMD_FW_CHUNK, seq & 0xFF,
                        payload, ACK_TIMEOUT)
        seq += 1
        done = offset + len(chunk)
        pct = 100.0 * done / total
        rate = done / max(time.time() - t0, 0.001) / 1024
        print(f"\r  {done}/{total} bytes ({pct:5.1f}%)  {rate:6.1f} KB/s", end="")

    print("\nfinalizing (board verifies the staged image)...")
    send_with_retry(link, parser, protocol.CMD_FW_COMPLETE, seq & 0xFF,
                    struct.pack("<IH", total, crc), COMPLETE_TIMEOUT)
    dt = time.time() - t0
    print(f"OK: image staged and CRC-verified in {dt:.1f}s "
          f"({total / dt / 1024:.1f} KB/s)")
    query_status(link, parser)


if __name__ == "__main__":
    main()
