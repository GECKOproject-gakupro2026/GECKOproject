"""Connection transports for the status monitor: UART / BLE / Wi-Fi(TCP).

Every transport delivers raw bytes to a callback; the caller runs them
through protocol.FrameParser. All transports are started/stopped from the
UI thread and run their I/O on a background thread.
"""
from __future__ import annotations

import queue
import socket
import threading
from typing import Callable, Optional

BytesCallback = Callable[[bytes], None]
EventCallback = Callable[[str], None]

# ST P2P service exposed by the STM32WB5MMG AT-server firmware
BLE_NOTIFY_CHAR_UUID = "0000fe42-8e22-4541-9d4c-21edae82ed19"
BLE_WRITE_CHAR_UUID = "0000fe41-8e22-4541-9d4c-21edae82ed19"


class Transport:
    name = "base"

    def __init__(self, on_bytes: BytesCallback, on_event: EventCallback) -> None:
        self.on_bytes = on_bytes
        self.on_event = on_event
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None

    def start(self) -> None:
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=3)
            self._thread = None

    def _run(self) -> None:
        raise NotImplementedError

    def write(self, data: bytes) -> bool:
        """Send bytes to the board (console commands). Default: unsupported."""
        return False


class UartTransport(Transport):
    name = "UART"

    def __init__(self, port: str, baud: int, on_bytes: BytesCallback,
                 on_event: EventCallback) -> None:
        super().__init__(on_bytes, on_event)
        self.port = port
        self.baud = baud
        self._ser = None

    def write(self, data: bytes) -> bool:
        ser = self._ser
        if ser is None:
            return False
        try:
            ser.write(data)
            return True
        except Exception:  # noqa: BLE001
            return False

    def _run(self) -> None:
        import serial
        try:
            with serial.Serial(self.port, self.baud, timeout=0.2) as ser:
                self._ser = ser
                self.on_event(f"UART connected: {self.port} @ {self.baud}")
                while not self._stop.is_set():
                    try:
                        data = ser.read(4096)
                    except serial.SerialException:
                        self.on_event("UART port lost, retrying...")
                        break
                    if data:
                        self.on_bytes(data)
        except Exception as exc:  # noqa: BLE001 - surface everything to the UI
            self.on_event(f"UART error: {exc}")
        finally:
            self._ser = None
        self.on_event("UART disconnected")


class TcpTransport(Transport):
    """Wi-Fi transport: expects the same frame stream over a TCP connection
    (board-side TCP server, or a serial-over-TCP bridge such as ser2net)."""
    name = "Wi-Fi (TCP)"

    def __init__(self, host: str, port: int, on_bytes: BytesCallback,
                 on_event: EventCallback) -> None:
        super().__init__(on_bytes, on_event)
        self.host = host
        self.port = port
        self._sock = None

    def write(self, data: bytes) -> bool:
        sock = self._sock
        if sock is None:
            return False
        try:
            sock.sendall(data)
            return True
        except Exception:  # noqa: BLE001
            return False

    def _run(self) -> None:
        try:
            with socket.create_connection((self.host, self.port), timeout=5) as sock:
                self._sock = sock
                sock.settimeout(0.2)
                self.on_event(f"TCP connected: {self.host}:{self.port}")
                while not self._stop.is_set():
                    try:
                        data = sock.recv(4096)
                    except socket.timeout:
                        continue
                    except OSError as exc:
                        self.on_event(f"TCP error: {exc}")
                        break
                    if not data:
                        self.on_event("TCP closed by peer")
                        break
                    self.on_bytes(data)
        except Exception as exc:  # noqa: BLE001
            self.on_event(f"TCP error: {exc}")
        finally:
            self._sock = None
        self.on_event("TCP disconnected")


class BleTransport(Transport):
    """Bluetooth LE transport: subscribes to the P2P notify characteristic of
    the on-board STM32WB5MMG AT server (compact status frames, 1 Hz)."""
    name = "BLE"

    def __init__(self, device: str, on_bytes: BytesCallback,
                 on_event: EventCallback) -> None:
        super().__init__(on_bytes, on_event)
        self.device = device  # name substring or MAC address
        self._write_queue: "queue.Queue[bytes]" = queue.Queue()

    def write(self, data: bytes) -> bool:
        # bleak's client only runs inside the asyncio loop owned by _run()'s
        # background thread, so a synchronous write() from the UI thread
        # can't call it directly - queue the bytes and let session()'s loop
        # drain them.
        self._write_queue.put(data)
        return True

    def _run(self) -> None:
        import asyncio
        try:
            from bleak import BleakClient, BleakScanner
        except ImportError:
            self.on_event("BLE error: bleak not installed (pip install bleak)")
            return

        async def session() -> None:
            self.on_event(f"BLE scanning for '{self.device}'...")
            wanted = self.device.lower()
            devices = await BleakScanner.discover(timeout=6.0)
            target = None
            for dev in devices:
                name = (dev.name or "").lower()
                if wanted in name or wanted == dev.address.lower():
                    target = dev
                    break
            if target is None:
                found = ", ".join(sorted({d.name for d in devices if d.name})) or "(none)"
                self.on_event(f"BLE device not found. Seen: {found}")
                return

            self.on_event(f"BLE connecting to {target.name} [{target.address}]...")
            async with BleakClient(target) as client:
                self.on_event("BLE connected, subscribing to notifications")

                def handle(_char, data: bytearray) -> None:
                    self.on_bytes(bytes(data))

                await client.start_notify(BLE_NOTIFY_CHAR_UUID, handle)
                while not self._stop.is_set() and client.is_connected:
                    try:
                        data = self._write_queue.get_nowait()
                    except queue.Empty:
                        pass
                    else:
                        try:
                            await client.write_gatt_char(BLE_WRITE_CHAR_UUID, data,
                                                          response=False)
                        except Exception as exc:  # noqa: BLE001
                            self.on_event(f"BLE write error: {exc}")
                    await asyncio.sleep(0.05)
                try:
                    await client.stop_notify(BLE_NOTIFY_CHAR_UUID)
                except Exception:  # noqa: BLE001 - already disconnecting
                    pass

        try:
            asyncio.run(session())
        except Exception as exc:  # noqa: BLE001
            self.on_event(f"BLE error: {exc}")
        self.on_event("BLE disconnected")
