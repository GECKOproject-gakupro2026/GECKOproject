"""B-U585I-IOT02A status monitor UI.

Shows the board status continuously streamed by the telemetry firmware.
Tabs:
  - ダッシュボード : main panels (button / sensors / audio level / radio / memory)
  - 全データ       : every decoded value in one table
  - オーディオ     : live PCM streaming view + WAV recording + playback

Connect via UART (ST-LINK VCP, 921600 baud), BLE (STM32WB5MMG) or Wi-Fi (TCP).

Usage: python app.py
Deps : pip install -r requirements.txt (pyserial, bleak)
"""
from __future__ import annotations

import datetime as _dt
import pathlib
import queue
import tkinter as tk
import wave
from tkinter import ttk

import protocol
import transports

RECORD_DIR = pathlib.Path(__file__).with_name("recordings")


def list_serial_ports() -> list[str]:
    try:
        from serial.tools import list_ports
        return [p.device for p in list_ports.comports()]
    except Exception:  # noqa: BLE001
        return []


class StatusMonitorApp:
    POLL_MS = 50
    WAVE_KEEP = 4096  # samples kept for the realtime audio view

    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        root.title("B-U585I-IOT02A Status Monitor")
        root.geometry("900x700")

        self.rx_queue: "queue.Queue[tuple]" = queue.Queue()
        self.parser = protocol.FrameParser()
        self.transport: transports.Transport | None = None
        self.frame_count = 0
        self.last_rate_count = 0
        self.audio_frames = 0
        self.last_status: protocol.Status | None = None

        self.audio_view: list[int] = []
        self.recording = False
        self.record_samples: list[int] = []
        self.last_wav: pathlib.Path | None = None

        self._build_ui()
        root.after(self.POLL_MS, self._poll_queue)
        root.after(1000, self._update_rate)

    # ---------------- UI construction ----------------
    def _build_ui(self) -> None:
        top = ttk.Frame(self.root, padding=6)
        top.pack(fill="x")

        ttk.Label(top, text="接続方式:").pack(side="left")
        self.link_var = tk.StringVar(value="UART")
        link = ttk.Combobox(top, textvariable=self.link_var, width=10, state="readonly",
                            values=["UART", "BLE", "Wi-Fi (TCP)"])
        link.pack(side="left", padx=4)
        link.bind("<<ComboboxSelected>>", lambda _e: self._update_target_hint())

        self.target_var = tk.StringVar()
        self.target_entry = ttk.Combobox(top, textvariable=self.target_var, width=28)
        self.target_entry.pack(side="left", padx=4)

        self.connect_btn = ttk.Button(top, text="接続", command=self._toggle_connect)
        self.connect_btn.pack(side="left", padx=8)

        self.rate_var = tk.StringVar(value="0 fps")
        ttk.Label(top, textvariable=self.rate_var).pack(side="right")
        self._update_target_hint()

        self.notebook = ttk.Notebook(self.root)
        self.notebook.pack(fill="both", expand=True, padx=6, pady=4)

        self._build_dashboard_tab()
        self._build_alldata_tab()
        self._build_audio_tab()

        # --- ログ ---
        f_log = ttk.LabelFrame(self.root, text="ファームウェアログ", padding=4)
        f_log.pack(fill="x", padx=6, pady=4)
        self.log = tk.Text(f_log, height=5, state="disabled", font=("Consolas", 9))
        self.log.pack(fill="both", expand=True)

    def _build_dashboard_tab(self) -> None:
        body = ttk.Frame(self.notebook, padding=6)
        self.notebook.add(body, text=" ダッシュボード ")
        body.columnconfigure((0, 1, 2), weight=1)

        f_state = ttk.LabelFrame(body, text="ボード状態", padding=6)
        f_state.grid(row=0, column=0, sticky="nsew", padx=3, pady=3)
        ttk.Label(f_state, text="USERボタン:").grid(row=0, column=0, sticky="w")
        self.button_canvas = tk.Canvas(f_state, width=26, height=26, highlightthickness=0)
        self.button_canvas.grid(row=0, column=1, sticky="w")
        self.button_led = self.button_canvas.create_oval(4, 4, 22, 22, fill="gray70")
        self.uptime_var = tk.StringVar(value="--")
        ttk.Label(f_state, text="稼働時間:").grid(row=1, column=0, sticky="w")
        ttk.Label(f_state, textvariable=self.uptime_var).grid(row=1, column=1, sticky="w")

        f_env = ttk.LabelFrame(body, text="環境センサー", padding=6)
        f_env.grid(row=0, column=1, sticky="nsew", padx=3, pady=3)
        self.env_vars = {}
        for i, (key, label) in enumerate([
                ("temp", "温度 [°C]"), ("hum", "湿度 [%RH]"),
                ("press", "気圧 [hPa]"), ("light", "照度 raw"),
                ("tof", "ToF距離 [mm]")]):
            ttk.Label(f_env, text=label + ":").grid(row=i, column=0, sticky="w")
            var = tk.StringVar(value="--")
            ttk.Label(f_env, textvariable=var, width=10, anchor="e").grid(row=i, column=1)
            self.env_vars[key] = var

        f_mot = ttk.LabelFrame(body, text="モーションセンサー", padding=6)
        f_mot.grid(row=0, column=2, sticky="nsew", padx=3, pady=3)
        self.mot_vars = {}
        for i, (key, label) in enumerate([
                ("acc", "加速度 [mg]"), ("gyro", "角速度 [dps]"),
                ("mag", "磁気 [mG]")]):
            ttk.Label(f_mot, text=label + ":").grid(row=2 * i, column=0, sticky="w")
            var = tk.StringVar(value="--")
            ttk.Label(f_mot, textvariable=var).grid(row=2 * i + 1, column=0, sticky="w")
            self.mot_vars[key] = var

        f_audio = ttk.LabelFrame(body, text="オーディオ (MIC2/MDF1)", padding=6)
        f_audio.grid(row=1, column=0, columnspan=2, sticky="nsew", padx=3, pady=3)
        ttk.Label(f_audio, text="RMS:").grid(row=0, column=0, sticky="w")
        self.rms_bar = ttk.Progressbar(f_audio, maximum=5000, length=220)
        self.rms_bar.grid(row=0, column=1, sticky="w", padx=4)
        self.rms_var = tk.StringVar(value="--")
        ttk.Label(f_audio, textvariable=self.rms_var, width=8).grid(row=0, column=2)
        ttk.Label(f_audio, text="Peak:").grid(row=1, column=0, sticky="w")
        self.peak_bar = ttk.Progressbar(f_audio, maximum=32767, length=220)
        self.peak_bar.grid(row=1, column=1, sticky="w", padx=4)
        self.peak_var = tk.StringVar(value="--")
        ttk.Label(f_audio, textvariable=self.peak_var, width=8).grid(row=1, column=2)
        self.wave_canvas = tk.Canvas(f_audio, width=430, height=80, bg="black",
                                     highlightthickness=0)
        self.wave_canvas.grid(row=2, column=0, columnspan=3, pady=4, sticky="w")

        right = ttk.Frame(body)
        right.grid(row=1, column=2, sticky="nsew", padx=3, pady=3)

        f_radio = ttk.LabelFrame(right, text="電波状況", padding=6)
        f_radio.pack(fill="x")
        self.radio_vars = {}
        for i, (key, label) in enumerate([
                ("ble", "BLE (STM32WB5MMG)"), ("wifi", "Wi-Fi (EMW3080)"),
                ("link", "受信リンク"), ("crc", "CRCエラー")]):
            ttk.Label(f_radio, text=label + ":").grid(row=i, column=0, sticky="w")
            var = tk.StringVar(value="--")
            ttk.Label(f_radio, textvariable=var).grid(row=i, column=1, sticky="e")
            self.radio_vars[key] = var

        f_mem = ttk.LabelFrame(right, text="メモリ使用状況", padding=6)
        f_mem.pack(fill="x", pady=4)
        self.mem_bars = {}
        self.mem_vars = {}
        for i, (key, label) in enumerate([("ram", "RAM"), ("heap", "Heap"),
                                          ("flash", "Flash")]):
            ttk.Label(f_mem, text=label + ":").grid(row=2 * i, column=0, sticky="w")
            bar = ttk.Progressbar(f_mem, maximum=100, length=170)
            bar.grid(row=2 * i, column=1, padx=4)
            var = tk.StringVar(value="--")
            ttk.Label(f_mem, textvariable=var).grid(row=2 * i + 1, column=1, sticky="e")
            self.mem_bars[key] = bar
            self.mem_vars[key] = var

    ALL_FIELDS = [
        ("uptime_ms", "稼働時間 [ms]"),
        ("button", "USERボタン"),
        ("temp_c", "温度 [°C]"),
        ("humidity", "湿度 [%RH]"),
        ("pressure_hpa", "気圧 [hPa]"),
        ("light_raw", "照度 raw (VEML)"),
        ("tof_mm", "ToF距離 [mm] (VL53L5CX)"),
        ("tof_ok", "ToF有効"),
        ("acc_mg", "加速度 XYZ [mg] (ISM330DHCX)"),
        ("gyro_dps", "角速度 XYZ [dps] (ISM330DHCX)"),
        ("mag_mgauss", "磁気 XYZ [mGauss] (IIS2MDC)"),
        ("audio_rms", "音声RMS"),
        ("audio_peak", "音声ピーク"),
        ("ble_alive", "BLEモジュール生存"),
        ("wifi_alive", "Wi-Fiモジュール生存"),
        ("ram_used", "RAM使用 [byte]"),
        ("ram_total", "RAM総量 [byte]"),
        ("heap_used", "Heap使用 [byte]"),
        ("heap_free", "Heap空き [byte]"),
        ("flash_used", "Flash使用 [byte]"),
        ("flash_total", "Flash総量 [byte]"),
    ]

    def _build_alldata_tab(self) -> None:
        body = ttk.Frame(self.notebook, padding=6)
        self.notebook.add(body, text=" 全データ ")
        cols = ("name", "value")
        self.tree = ttk.Treeview(body, columns=cols, show="headings", height=24)
        self.tree.heading("name", text="項目")
        self.tree.heading("value", text="値")
        self.tree.column("name", width=320, anchor="w")
        self.tree.column("value", width=380, anchor="w")
        self.tree.pack(fill="both", expand=True)
        for key, label in self.ALL_FIELDS:
            self.tree.insert("", "end", iid=key, values=(label, "--"))
        for key, label in [("_fps", "受信レート [フレーム/秒]"),
                           ("_frames", "累計フレーム数"),
                           ("_crc", "CRCエラー数"),
                           ("_audio_frames", "音声フレーム数")]:
            self.tree.insert("", "end", iid=key, values=(label, "--"))

    def _build_audio_tab(self) -> None:
        body = ttk.Frame(self.notebook, padding=6)
        self.notebook.add(body, text=" オーディオ ")

        ctrl = ttk.Frame(body)
        ctrl.pack(fill="x", pady=4)
        self.stream_btn = ttk.Button(ctrl, text="ストリーミング開始",
                                     command=self._toggle_stream)
        self.stream_btn.pack(side="left", padx=4)
        self.record_btn = ttk.Button(ctrl, text="録音開始", command=self._toggle_record,
                                     state="disabled")
        self.record_btn.pack(side="left", padx=4)
        self.play_btn = ttk.Button(ctrl, text="再生", command=self._play_last,
                                   state="disabled")
        self.play_btn.pack(side="left", padx=4)
        self.audio_info_var = tk.StringVar(value="ストリーミング停止中 (UART接続時のみ利用可)")
        ttk.Label(ctrl, textvariable=self.audio_info_var).pack(side="left", padx=12)

        self.live_canvas = tk.Canvas(body, width=860, height=260, bg="black",
                                     highlightthickness=0)
        self.live_canvas.pack(fill="both", expand=True, pady=4)

        note = ("16kHz / 16bit / モノラル (MIC2, MDF1)。録音は recordings/ にWAV保存。"
                "ボードへ 'a'(開始)/'s'(停止) を送信して制御します。")
        ttk.Label(body, text=note, foreground="gray40").pack(anchor="w")

    def _update_target_hint(self) -> None:
        link = self.link_var.get()
        if link == "UART":
            ports = list_serial_ports()
            self.target_entry.configure(values=ports)
            self.target_var.set(ports[-1] if ports else "COM9")
        elif link == "BLE":
            self.target_entry.configure(values=["P2PSRV1", "STM32WB"])
            self.target_var.set("P2PSRV1")
        else:
            self.target_entry.configure(values=["192.168.11.48:5000"])
            self.target_var.set("192.168.11.48:5000")

    # ---------------- connection ----------------
    def _toggle_connect(self) -> None:
        if self.transport is not None:
            self._set_stream(False)
            self.transport.stop()
            self.transport = None
            self.connect_btn.configure(text="接続")
            self._log("disconnected")
            return

        link = self.link_var.get()
        target = self.target_var.get().strip()
        on_bytes = lambda data: self.rx_queue.put(("bytes", data))  # noqa: E731
        on_event = lambda msg: self.rx_queue.put(("event", msg))  # noqa: E731
        try:
            if link == "UART":
                self.transport = transports.UartTransport(target, 921600, on_bytes, on_event)
            elif link == "BLE":
                self.transport = transports.BleTransport(target, on_bytes, on_event)
            else:
                host, _, port = target.partition(":")
                self.transport = transports.TcpTransport(host, int(port or "5000"),
                                                         on_bytes, on_event)
        except Exception as exc:  # noqa: BLE001
            self._log(f"connection setup error: {exc}")
            self.transport = None
            return
        self.radio_vars["link"].set(link)
        self.transport.start()
        self.connect_btn.configure(text="切断")

    # ---------------- audio controls ----------------
    def _set_stream(self, enable: bool) -> None:
        if self.transport is None:
            return
        ok = self.transport.write(b"a" if enable else b"s")
        if not ok:
            self.audio_info_var.set("この接続方式ではストリーミング制御を送れません")
            return
        self.streaming = enable
        self.stream_btn.configure(text="ストリーミング停止" if enable else "ストリーミング開始")
        self.record_btn.configure(state="normal" if enable else "disabled")
        self.audio_info_var.set("ストリーミング中 (16kHz)" if enable else "ストリーミング停止中")
        if not enable and self.recording:
            self._toggle_record()

    def _toggle_stream(self) -> None:
        self._set_stream(not getattr(self, "streaming", False))

    def _toggle_record(self) -> None:
        if not self.recording:
            self.recording = True
            self.record_samples = []
            self.record_btn.configure(text="録音停止")
            self._log("録音開始")
            return
        self.recording = False
        self.record_btn.configure(text="録音開始")
        if not self.record_samples:
            self._log("録音データなし")
            return
        RECORD_DIR.mkdir(exist_ok=True)
        name = _dt.datetime.now().strftime("rec_%Y%m%d_%H%M%S.wav")
        path = RECORD_DIR / name
        with wave.open(str(path), "wb") as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(protocol.AUDIO_SAMPLE_RATE)
            import struct as _struct
            wav.writeframes(_struct.pack(f"<{len(self.record_samples)}h",
                                         *self.record_samples))
        secs = len(self.record_samples) / protocol.AUDIO_SAMPLE_RATE
        self.last_wav = path
        self.play_btn.configure(state="normal")
        self._log(f"録音保存: {path.name} ({secs:.1f}秒, {len(self.record_samples)}サンプル)")

    def _play_last(self) -> None:
        if self.last_wav is None:
            return
        try:
            import winsound
            winsound.PlaySound(str(self.last_wav),
                               winsound.SND_FILENAME | winsound.SND_ASYNC)
            self._log(f"再生: {self.last_wav.name}")
        except Exception as exc:  # noqa: BLE001
            self._log(f"再生エラー: {exc}")

    # ---------------- data pump ----------------
    def _poll_queue(self) -> None:
        audio_dirty = False
        try:
            while True:
                kind, payload = self.rx_queue.get_nowait()
                if kind == "event":
                    self._log(payload)
                    continue
                for item in self.parser.feed(payload):
                    if item[0] == "frame":
                        _tag, cmd, _seq, body = item
                        if cmd == protocol.CMD_AUDIO:
                            samples = protocol.decode_audio(body)
                            self.audio_frames += 1
                            self.audio_view.extend(samples)
                            self.audio_view = self.audio_view[-self.WAVE_KEEP:]
                            if self.recording:
                                self.record_samples.extend(samples)
                            audio_dirty = True
                            continue
                        status = protocol.decode_status(cmd, body)
                        if status is not None:
                            self.frame_count += 1
                            self.last_status = status
                            self._apply(status)
                    else:
                        self._log(item[1], raw=True)
        except queue.Empty:
            pass
        if audio_dirty:
            self._draw_live_audio()
        self.root.after(self.POLL_MS, self._poll_queue)

    def _update_rate(self) -> None:
        rate = self.frame_count - self.last_rate_count
        self.last_rate_count = self.frame_count
        self.rate_var.set(f"{rate} fps / CRCerr {self.parser.crc_errors}")
        self.radio_vars["crc"].set(str(self.parser.crc_errors))
        self._tree_set("_fps", str(rate))
        self._tree_set("_frames", str(self.parser.frames))
        self._tree_set("_crc", str(self.parser.crc_errors))
        self._tree_set("_audio_frames", str(self.audio_frames))
        self.root.after(1000, self._update_rate)

    # ---------------- rendering ----------------
    def _tree_set(self, key: str, value: str) -> None:
        if self.tree.exists(key):
            self.tree.set(key, "value", value)

    def _apply(self, st: protocol.Status) -> None:
        self.button_canvas.itemconfigure(
            self.button_led, fill="lime green" if st.button else "gray70")
        if not st.compact:
            secs = st.uptime_ms // 1000
            self.uptime_var.set(f"{secs // 3600:02d}:{secs % 3600 // 60:02d}:{secs % 60:02d}")

        self.env_vars["temp"].set(f"{st.temp_c:.2f}")
        self.env_vars["hum"].set(f"{st.humidity:.1f}")
        self.env_vars["press"].set(f"{st.pressure_hpa:.1f}")
        self.env_vars["light"].set(str(st.light_raw))
        self.env_vars["tof"].set(str(st.tof_mm) if (st.tof_ok or st.compact) else "--")

        if not st.compact:
            self.mot_vars["acc"].set("({: d}, {: d}, {: d})".format(*st.acc_mg))
            self.mot_vars["gyro"].set("({:.1f}, {:.1f}, {:.1f})".format(*st.gyro_dps))
            self.mot_vars["mag"].set("({: d}, {: d}, {: d})".format(*st.mag_mgauss))
            self.radio_vars["ble"].set("OK" if st.ble_alive else "NG")
            self.radio_vars["wifi"].set("OK" if st.wifi_alive else "NG")
            self._apply_memory(st)
            self._draw_wave(st.wave)

        self.rms_bar["value"] = min(st.audio_rms, 5000)
        self.rms_var.set(str(st.audio_rms))
        self.peak_bar["value"] = min(st.audio_peak, 32767)
        self.peak_var.set(str(st.audio_peak))

        # 全データタブ
        for key, _label in self.ALL_FIELDS:
            value = getattr(st, key)
            if isinstance(value, float):
                text = f"{value:.2f}"
            elif isinstance(value, tuple):
                text = str(value)
            else:
                text = str(value)
            self._tree_set(key, text)

    def _apply_memory(self, st: protocol.Status) -> None:
        def fmt(used: int, total: int) -> str:
            return f"{used / 1024:.1f} / {total / 1024:.0f} KB"

        if st.ram_total:
            self.mem_bars["ram"]["value"] = 100 * st.ram_used / st.ram_total
            self.mem_vars["ram"].set(fmt(st.ram_used, st.ram_total))
        heap_total = st.heap_used + st.heap_free
        if heap_total:
            self.mem_bars["heap"]["value"] = 100 * st.heap_used / heap_total
            self.mem_vars["heap"].set(fmt(st.heap_used, heap_total))
        if st.flash_total:
            self.mem_bars["flash"]["value"] = 100 * st.flash_used / st.flash_total
            self.mem_vars["flash"].set(fmt(st.flash_used, st.flash_total))

    def _draw_wave(self, wave_samples: list[int]) -> None:
        self._draw_polyline(self.wave_canvas, wave_samples, 32768.0)

    def _draw_live_audio(self) -> None:
        self._draw_polyline(self.live_canvas, self.audio_view, 8192.0)

    @staticmethod
    def _draw_polyline(canvas: tk.Canvas, samples: list[int], full_scale: float) -> None:
        canvas.delete("wave")
        if len(samples) < 2:
            return
        width = canvas.winfo_width() or int(canvas.cget("width"))
        height = canvas.winfo_height() or int(canvas.cget("height"))
        mid = height / 2
        scale = mid / full_scale
        step = width / (len(samples) - 1)
        points = []
        for i, sample in enumerate(samples):
            y = mid - sample * scale
            y = max(0, min(height, y))
            points.extend((i * step, y))
        canvas.create_line(*points, fill="spring green", tags="wave")
        canvas.create_line(0, mid, width, mid, fill="gray30", tags="wave")

    def _log(self, message: str, raw: bool = False) -> None:
        self.log.configure(state="normal")
        if raw:
            self.log.insert("end", message)
        else:
            self.log.insert("end", f"[app] {message}\n")
        self.log.see("end")
        if int(self.log.index("end-1c").split(".")[0]) > 500:
            self.log.delete("1.0", "100.0")
        self.log.configure(state="disabled")


def main() -> None:
    root = tk.Tk()
    StatusMonitorApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
