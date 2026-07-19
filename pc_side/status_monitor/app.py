"""B-U585I-IOT02A status monitor UI.

Shows the board status continuously streamed by the telemetry firmware.
Tabs:
  - ダッシュボード : main panels (button / sensors / audio level / radio / memory)
  - 全データ       : every decoded value in one table
  - オーディオ     : live audio spectrum (Hz vs dB, FFT) + WAV recording + playback

Connect via UART (ST-LINK VCP, 921600 baud), BLE (STM32WB5MMG) or Wi-Fi (TCP).

Wi-Fi (TCP)選択時は「ホットスポットON」でPCのモバイルホットスポットを起動
できる(ファーム側はこのSSID/パスワードに固定接続する設計、app_config.hの
CFG_WIFI_SSID/CFG_WIFI_PASSWORD参照)。ボードのIPはDHCP割当で毎回変わるため、
「ボードIP検出」でARPテーブル+TCP接続確認から自動的に見つける
(../find_board.ps1 を利用)。

Usage: python app.py
Deps : pip install -r requirements.txt (pyserial, bleak)
"""
from __future__ import annotations

import datetime as _dt
import pathlib
import queue
import struct
import subprocess
import threading
import tkinter as tk
import wave
from tkinter import messagebox, ttk
from typing import Optional

import numpy as np

import adpcm
import protocol
import transports

RECORD_DIR = pathlib.Path(__file__).with_name("recordings")
# wifi_hotspot.ps1 / find_board.ps1 は pc_side/ 直下(このファイルの1つ上の
# 階層)にある。status_monitor/ 単体で動くツールではないため相対配置。
PC_SIDE_DIR = pathlib.Path(__file__).resolve().parent.parent
HOTSPOT_SCRIPT = PC_SIDE_DIR / "wifi_hotspot.ps1"
FIND_BOARD_SCRIPT = PC_SIDE_DIR / "find_board.ps1"
BOARD_TCP_PORT = 5000


def list_serial_ports() -> list[str]:
    try:
        from serial.tools import list_ports
        return [p.device for p in list_ports.comports()]
    except Exception:  # noqa: BLE001
        return []


def run_powershell(script: pathlib.Path, args: list[str]) -> tuple[bool, str]:
    """Runs a PowerShell script and returns (success, stdout+stderr)."""
    try:
        proc = subprocess.run(
            ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
             "-File", str(script), *args],
            capture_output=True, text=True, timeout=30,
        )
        out = (proc.stdout or "") + (proc.stderr or "")
        return proc.returncode == 0, out.strip()
    except Exception as exc:  # noqa: BLE001
        return False, str(exc)


def find_board_ip(port: int = BOARD_TCP_PORT) -> Optional[str]:
    """Scans the mobile-hotspot subnet (192.168.137.0/24) for a host with the
    board's TCP status server open, via find_board.ps1 (ARP table + TCP
    connect probe). Returns "ip:port" or None if not found."""
    ok, out = run_powershell(FIND_BOARD_SCRIPT, ["-Port", str(port)])
    if not ok:
        return None
    for line in out.splitlines():
        line = line.strip()
        # find_board.ps1 prints a PS object table; look for an IP:port token
        # in the "Target" column output (e.g. "192.168.137.16:5000").
        for token in line.split():
            if token.count(".") == 3 and ":" in token:
                host, _, port_s = token.partition(":")
                if port_s.isdigit():
                    return token
    return None


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
        self.streaming = False
        self.recording = False
        self.record_samples: list[int] = []
        self.ble_rec_chunks: dict[int, bytes] = {}
        self.ble_rec_ended = False  # REC_END を受けたら True (二重処理防止)
        self.last_wav: pathlib.Path | None = None

        # Step D2: non-volatile state log (fetched page-by-page via LOG_REQ).
        self.log_records: list[tuple] = []
        self.log_fetch_next = 0

        # Phase E low-power support: the firmware idles (stops telemetry, slow-
        # blinks its red LED) after ~3 s with no host traffic. Keep it awake by
        # sending a lightweight keep-alive byte each second while "keep awake"
        # is on, and show ACTIVE/IDLE based on whether frames are arriving.
        self.keep_awake = True
        self.power_state = "?"

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

        # Wi-Fi (TCP)選択時だけ出す: モバイルホットスポットの起動/停止と、
        # ボードの実IP自動検出(DHCP割当のため固定IPでは繋がらないことがある)。
        self.hotspot_btn = ttk.Button(top, text="ホットスポットON",
                                      command=self._toggle_hotspot)
        self.find_ip_btn = ttk.Button(top, text="ボードIP検出",
                                      command=self._find_board_ip)

        self.connect_btn = ttk.Button(top, text="接続", command=self._toggle_connect)
        self.connect_btn.pack(side="left", padx=8)

        # Phase E: low-power state readout + keep-awake toggle.
        self.keep_awake_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(top, text="常時ACTIVE維持", variable=self.keep_awake_var,
                        command=self._on_keep_awake_toggle).pack(side="left", padx=8)
        self.power_var = tk.StringVar(value="電源: ?")
        ttk.Label(top, textvariable=self.power_var).pack(side="left", padx=4)

        # デバイス状態機械の表示。ACTIVE中はMiniStatus.flags bit3-4(BLE)/
        # decode_status(UART/TCP)から、IDLE中はCMD_IDLE_BEACON(全リンク共通)
        # から更新される。
        self.dev_state_var = tk.StringVar(value="状態: -")
        ttk.Label(top, textvariable=self.dev_state_var).pack(side="left", padx=4)

        # 時刻同期: PCのUnix時刻をボードへ送る(epochオフセット方式)。
        ttk.Button(top, text="時刻同期", command=self._sync_time).pack(side="left", padx=4)
        # 待機: アクティブなリンクをIDLEへ落とす(LINK_STANDBY)。
        ttk.Button(top, text="待機", command=self._send_standby).pack(side="left", padx=4)
        # 不揮発状態ログ(Step D2): 取得/保存/リセット。
        ttk.Button(top, text="ログ取得", command=self._fetch_log).pack(side="left", padx=4)
        ttk.Button(top, text="ログ保存", command=self._save_log).pack(side="left", padx=4)
        ttk.Button(top, text="ログリセット", command=self._reset_log).pack(side="left", padx=4)

        self.rate_var = tk.StringVar(value="0 fps")
        ttk.Label(top, textvariable=self.rate_var).pack(side="right")
        self._update_target_hint()

        self.notebook = ttk.Notebook(self.root)
        self.notebook.pack(fill="both", expand=True, padx=6, pady=4)

        self._build_dashboard_tab()
        self._build_alldata_tab()
        self._build_audio_tab()

        # --- センサー取得周期(Step 3: FRAME_CMD_SET_SENSOR_RATE) ---
        f_rate = ttk.LabelFrame(self.root, text="センサー取得周期 [ms]", padding=4)
        f_rate.pack(fill="x", padx=6, pady=2)
        self.sensor_rate_vars: dict[int, tk.StringVar] = {}
        for sensor_id, name, default_ms in (
            (0, "env(温湿度気圧)", 100),
            (1, "light(照度)", 100),
            (2, "tof(距離)", 100),
            (3, "motion(加速度等,0=毎回)", 0),
        ):
            ttk.Label(f_rate, text=name).pack(side="left", padx=(6, 2))
            var = tk.StringVar(value=str(default_ms))
            self.sensor_rate_vars[sensor_id] = var
            ttk.Entry(f_rate, textvariable=var, width=7).pack(side="left", padx=(0, 6))
        ttk.Button(f_rate, text="適用", command=self._apply_sensor_rates).pack(
            side="left", padx=6)

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
        ("die_temp_c", "MCUダイ温度 [°C] (内蔵ADC)"),
        ("vdda_mv", "VDDA電源電圧 [mV]"),
        ("sysclk_hz", "SYSCLK [Hz]"),
        ("hclk_hz", "HCLK [Hz]"),
        ("cpu_load_pct", "CPU負荷 [%]"),
        ("reset_cause", "リセット要因 (RCC_CSRフラグ)"),
        ("flash_kb", "Flashサイズ [KB] (工場値)"),
        ("uid", "デバイスUID (96bit)"),
        ("idcode", "IDCODE (デバイス/リビジョン)"),
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

        note = ("16kHz / 16bit / モノラル (MIC2, MDF1)。スペクトラム表示は"
                "周波数 [Hz] 対 音圧 [dB] (FFT)。録音は recordings/ にWAV保存。"
                "ボードへ 'a'(開始)/'s'(停止) を送信して制御します。")
        ttk.Label(body, text=note, foreground="gray40").pack(anchor="w")

    def _update_target_hint(self) -> None:
        link = self.link_var.get()
        # Wi-Fi専用ボタン(ホットスポットON/OFF・ボードIP検出)は
        # Wi-Fi (TCP)選択時のみ表示する。
        self.hotspot_btn.pack_forget()
        self.find_ip_btn.pack_forget()
        if link == "UART":
            ports = list_serial_ports()
            self.target_entry.configure(values=ports)
            self.target_var.set(ports[-1] if ports else "COM9")
        elif link == "BLE":
            self.target_entry.configure(values=["P2PSRV1", "STM32WB"])
            self.target_var.set("P2PSRV1")
        else:
            self.hotspot_btn.pack(side="left", padx=2, before=self.connect_btn)
            self.find_ip_btn.pack(side="left", padx=2, before=self.connect_btn)
            # ボードのIPはPCのモバイルホットスポットのDHCPが割り当てるため
            # 固定できない。「ボードIP検出」で自動的に見つける想定。
            self.target_entry.configure(values=[f"{BOARD_TCP_PORT}"])
            if not self.target_var.get():
                self.target_var.set(f"192.168.137.2:{BOARD_TCP_PORT}")

    # ---------------- Wi-Fi helpers (mobile hotspot + board discovery) -----
    def _toggle_hotspot(self) -> None:
        action = "Stop" if self.hotspot_btn["text"] == "ホットスポットOFF" else "Start"
        self.hotspot_btn.configure(state="disabled")
        self._log(f"モバイルホットスポットを{'起動' if action == 'Start' else '停止'}しています...")

        def worker() -> None:
            ok, out = run_powershell(HOTSPOT_SCRIPT, ["-Action", action])
            self.root.after(0, lambda: self._on_hotspot_done(action, ok, out))

        threading.Thread(target=worker, daemon=True).start()

    def _on_hotspot_done(self, action: str, ok: bool, out: str) -> None:
        self.hotspot_btn.configure(state="normal")
        if ok:
            self.hotspot_btn.configure(
                text="ホットスポットOFF" if action == "Start" else "ホットスポットON")
            self._log(f"モバイルホットスポット{'起動' if action == 'Start' else '停止'}完了: {out}")
        else:
            self._log(f"モバイルホットスポット操作に失敗: {out}")
            messagebox.showerror("ホットスポット", f"操作に失敗しました:\n{out}")

    def _find_board_ip(self) -> None:
        self.find_ip_btn.configure(state="disabled")
        self._log("ボードのIPをネットワーク上から検索しています...")

        def worker() -> None:
            target = find_board_ip()
            self.root.after(0, lambda: self._on_find_ip_done(target))

        threading.Thread(target=worker, daemon=True).start()

    def _on_find_ip_done(self, target: Optional[str]) -> None:
        self.find_ip_btn.configure(state="normal")
        if target:
            self.target_var.set(target)
            self._log(f"ボードを発見: {target}")
        else:
            self._log("ボードが見つかりませんでした"
                      "(モバイルホットスポットが起動しているか、ボードが接続済みか確認してください)")
            messagebox.showwarning(
                "ボードIP検出",
                "ボードが見つかりませんでした。\n"
                "・モバイルホットスポットが起動しているか\n"
                "・ボードがWi-Fiに接続済みか(UARTログの'Wi-Fi connected'を確認)\n"
                "を確認してください。")

    # ---------------- connection ----------------
    def _toggle_connect(self) -> None:
        if self.transport is not None:
            self._set_stream(False)
            self._send_stop_comm()
            self.transport.stop()
            self.transport = None
            self.connect_btn.configure(text="接続")
            self.record_btn.configure(state="disabled")
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
        if self.keep_awake:
            # 厳密FSM(Step C2): IDLEからの復帰は明示コマンドのみが根拠になる。
            # 接続直後に一度送り、以後は _update_rate の1Hzループが送り続ける。
            self._send_enter_comm()
        if link == "BLE":
            # BLE can't stream PCM live, but the record button drives the
            # SRAM-buffered record-then-send flow (_toggle_record_ble)
            # independently of self.streaming.
            self.record_btn.configure(state="normal")
            self.audio_info_var.set("BLE: 録音開始→停止で一括転送 (リアルタイム再生不可)")

    # ---------------- low-power (Phase E) ----------------
    def _on_keep_awake_toggle(self) -> None:
        self.keep_awake = self.keep_awake_var.get()
        if self.keep_awake:
            # 明示的に今すぐ起こす(次の1Hzループを待たない)。
            self._send_enter_comm()
        else:
            # 明示的なACTIVE->IDLEヒント。送らなくても無通信タイムアウトで
            # いずれIDLEに落ちるが、すぐに低電力へ倒したい場合のため。
            self._send_stop_comm()

    # ---------------- state-machine controls ----------------
    def _sync_time(self) -> None:
        """PCのUnix時刻(u32秒 LE)をボードへ送る。ボードはRTC非搭載なので
        offset = pc_epoch*1000 - uptime_ms を保持し、以降 wall = offset+uptime。
        既存の keep-alive(self.transport.write)と同じ送信経路を使う。"""
        import time as _time
        if self.transport is None:
            self._log("時刻同期: 未接続")
            return
        epoch = int(_time.time())
        frame = protocol.build_frame(protocol.CMD_TIME_SYNC, 0, struct.pack("<I", epoch))
        if self.transport.write(frame):
            self._log(f"時刻同期を送信: {epoch} "
                      f"({_time.strftime('%Y-%m-%d %H:%M:%S', _time.localtime(epoch))})")
        else:
            self._log("時刻同期: この接続方式では送信できません")

    def _send_standby(self) -> None:
        """アクティブなリンクをIDLEへ落とす(LINK_STANDBY)。次の通信で再Active。"""
        if self.transport is None:
            self._log("待機: 未接続")
            return
        frame = protocol.build_frame(protocol.CMD_LINK_STANDBY, 0)
        if self.transport.write(frame):
            self._log("待機コマンド(LINK_STANDBY)を送信")
        else:
            self._log("待機: この接続方式では送信できません")

    def _apply_sensor_rates(self) -> None:
        """センサー周期設定UIの値をそれぞれ FRAME_CMD_SET_SENSOR_RATE で送る
        (sensor_id 0=env,1=light,2=tof,3=motion)。ボード側で下限クランプされる
        (実装計画_統合.md §3)。"""
        if self.transport is None:
            self._log("センサー周期: 未接続")
            return
        for sensor_id, var in self.sensor_rate_vars.items():
            try:
                period_ms = int(var.get())
            except ValueError:
                self._log(f"センサー周期: sensor_id={sensor_id} の値が不正: {var.get()!r}")
                continue
            frame = protocol.build_frame(
                protocol.CMD_SET_SENSOR_RATE, 0,
                struct.pack("<BH", sensor_id, max(0, period_ms)))
            if self.transport.write(frame):
                self._log(f"センサー周期送信: id={sensor_id} period={period_ms}ms")
            else:
                self._log("センサー周期: この接続方式では送信できません")
                break

    def _send_enter_comm(self) -> None:
        """厳密FSM(Step C2)向け: ボードのIDLE->ACTIVE遷移を許可する唯一の根拠。
        任意のバイトでは復帰しなくなったため、接続直後と _update_rate の1Hz
        ループから送る(旧NULキープアライブの置き換え)。"""
        if self.transport is None:
            return
        frame = protocol.build_frame(protocol.CMD_ENTER_COMM, 0)
        self.transport.write(frame)

    def _send_stop_comm(self) -> None:
        """明示的なACTIVE->IDLEヒント。切断時と「常時ACTIVE維持」オフ時に送る。"""
        if self.transport is None:
            return
        frame = protocol.build_frame(protocol.CMD_STOP_COMM, 0)
        self.transport.write(frame)

    # ---------------- non-volatile state log (Step D2) ----------------
    def _fetch_log(self) -> None:
        """不揮発状態ログ(NOR)をLOG_REQのページングで全件取得する。"""
        if self.transport is None:
            self._log("ログ取得: 未接続")
            return
        self.log_records = []
        self.log_fetch_next = 0
        self._log("ログ取得を開始...")
        self._request_log_page(0)

    def _request_log_page(self, start_index: int) -> None:
        if self.transport is None:
            return
        frame = protocol.build_frame(protocol.CMD_LOG_REQ, 0, struct.pack("<I", start_index))
        self.transport.write(frame)

    def _on_idle_beacon(self, body: bytes) -> None:
        """CMD_IDLE_BEACON: IDLE中の生存確認+状態通知(センサー値は載らない)。
        state-machine rebuild後はIDLEでセンサーを取得も送信もしないので、
        代わりにこれで状態ラベルとfpsカウンタ用のframe_countを更新する。
        IDLE中は実際のセンサー値が更新され続けないので、前回ACTIVE時の値が
        古いまま表示され続けるのを避けるため、表示は "-" にリセットする。"""
        uptime_ms, device_state = protocol.decode_idle_beacon(body)
        self.frame_count += 1
        label = self.DEVICE_STATE_LABELS.get(device_state, f"?({device_state})")
        self.dev_state_var.set(f"状態: {label}")
        secs = uptime_ms // 1000
        self.uptime_var.set(f"{secs // 3600:02d}:{secs % 3600 // 60:02d}:{secs % 60:02d}")
        if device_state == 0:
            self._clear_sensor_display()

    def _clear_sensor_display(self) -> None:
        """IDLE中(実センサー値の更新なし)の表示を "-" にリセットする。
        dev_state_var/uptime_varは_on_idle_beaconが引き続き更新するので対象外。"""
        self.button_canvas.itemconfigure(self.button_led, fill="gray70")
        for var in self.env_vars.values():
            var.set("--")
        for var in self.mot_vars.values():
            var.set("--")
        self.radio_vars["ble"].set("--")
        self.radio_vars["wifi"].set("--")
        self.rms_bar["value"] = 0
        self.rms_var.set("--")
        self.peak_bar["value"] = 0
        self.peak_var.set("--")
        self.wave_canvas.delete("wave")
        # メモリ使用量(ram/heap/flash)とデバイス識別情報(uid/idcode/reset_cause等)
        # はマイコンの静的/準静的な状態でありIDLE中も本来の値が存在するので、
        # ダッシュボードのメモリ表示と同様に前回値を残す。
        keep = {"ram_used", "ram_total", "heap_used", "heap_free",
                "flash_used", "flash_total", "flash_kb", "uid", "idcode",
                "reset_cause", "sysclk_hz", "hclk_hz", "vdda_mv"}
        for key, _label in self.ALL_FIELDS:
            if key not in keep:
                self._tree_set(key, "--")

    def _on_log_resp(self, body: bytes) -> None:
        start_index, records = protocol.decode_log_resp(body)
        self.log_records.extend(records)
        if not records:
            self._log(f"ログ取得完了: {len(self.log_records)}件")
            return
        self.log_fetch_next = start_index + len(records)
        self._request_log_page(self.log_fetch_next)

    def _save_log(self) -> None:
        """取得済みの不揮発ログを.txtへ保存する(_save_wavのRECORD_DIRパターン)。"""
        import time as _time
        records = self.log_records
        if not records:
            self._log("ログ保存: 取得済みのログがありません(先に取得してください)")
            return
        RECORD_DIR.mkdir(parents=True, exist_ok=True)
        path = RECORD_DIR / _time.strftime("state_log_%Y%m%d_%H%M%S.txt")
        with open(path, "w", encoding="utf-8") as f:
            f.write("index\twall_ms\tevent\tret_val\n")
            for i, (wall_ms, event, ret_val) in enumerate(records):
                name = protocol.LOG_EVENT_NAMES.get(event, str(event))
                # ret_val==0xF は「本来の値が4bitに収まらなかった」印
                rv = "overflow" if ret_val == protocol.LOG_RET_OVERFLOW else ret_val
                f.write(f"{i}\t{wall_ms}\t{name}\t{rv}\n")
        self._log(f"ログ保存: {path}")

    def _reset_log(self) -> None:
        if self.transport is None:
            self._log("ログリセット: 未接続")
            return
        if not messagebox.askyesno("ログリセット", "不揮発状態ログを全消去します。よろしいですか?"):
            return
        frame = protocol.build_frame(protocol.CMD_LOG_RESET, 0)
        if self.transport.write(frame):
            self._log("ログリセットを送信")
            self.log_records = []
        else:
            self._log("ログリセット: この接続方式では送信できません")

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
        self._set_stream(not self.streaming)

    def _toggle_record(self) -> None:
        if self.link_var.get() == "BLE":
            self._toggle_record_ble()
            return
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
        self._save_wav(self.record_samples)

    def _save_wav(self, samples: list[int], sample_rate: int | None = None) -> None:
        RECORD_DIR.mkdir(exist_ok=True)
        name = _dt.datetime.now().strftime("rec_%Y%m%d_%H%M%S.wav")
        path = RECORD_DIR / name
        # BLE録音は 8 kHz(REC_SAMPLE_RATE)、UART/TCP音声は 16 kHz(AUDIO_SAMPLE_RATE)。
        # 秒数表示は WAV に書くレートと必ず同じ rate で割ること(以前ここが常に
        # 16 kHz 固定で、8 kHz 録音の長さが半分に表示されるバグがあった)。
        rate = sample_rate or protocol.AUDIO_SAMPLE_RATE
        with wave.open(str(path), "wb") as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(rate)
            import struct as _struct
            wav.writeframes(_struct.pack(f"<{len(samples)}h", *samples))
        secs = len(samples) / rate
        self.last_wav = path
        self.play_btn.configure(state="normal")
        self._log(f"録音保存: {path.name} ({secs:.1f}秒, {len(samples)}サンプル, {rate}Hz)")

    def _toggle_record_ble(self) -> None:
        # The board streams continuously: while REC_START..REC_STOP is
        # active it records and BLE-notifies each self-contained ADPCM
        # block (REC_CHUNK) as soon as it's encoded, draining its small
        # ring buffer as it goes (see recorder.cpp / comm_ble.cpp's
        # SendRecInfo). After REC_STOP it keeps streaming until the ring is
        # empty, then sends one REC_END with the final totals. This has no
        # RAM ceiling on recording length (unlike the old stop&wait/whole-
        # buffer approach), so multi-minute recordings work.
        if self.transport is None:
            return
        if not self.recording:
            self.recording = True
            self.ble_rec_chunks = {}
            self.ble_rec_ended = False
            self.record_btn.configure(text="録音停止")
            self.transport.write(protocol.build_frame(protocol.CMD_REC_START, 0))
            self._log("BLE録音開始")
            return
        self.recording = False
        self.record_btn.configure(text="録音開始")
        self.transport.write(protocol.build_frame(protocol.CMD_REC_STOP, 0))
        self._log("BLE録音停止、残りのチャンク転送待ち...")

    def _on_ble_rec_chunk(self, seq: int, payload: bytes) -> None:
        self.ble_rec_chunks[seq] = payload

    # BLE録音転送は連続ストリーミング方式(comm_ble.cpp参照)。
    #   REC_CHUNK:[cmd u8][seq u16 LE][adpcm block, self-contained] (board->PC, raw TLV)
    #   REC_END:  [cmd u8][total_samples u32 LE][total_blocks u16 LE] (board->PC, raw TLV)
    # REC_CHUNKは録音しながら逐次届き、REC_ENDは最後のブロックを送り切った後に
    # 一度だけ届く。BLE notifyは取りこぼしうるので、REC_END到着時に欠けている
    # seqがあれば「欠落あり」として記録するのみ(送信済みブロックはボード側の
    # リングから既に解放されているため再取得はできない)。
    def _handle_ble_rec_notify(self, payload: bytes) -> None:
        cmd = payload[0]
        if cmd == protocol.CMD_REC_CHUNK and len(payload) > 3:
            seq = payload[1] | (payload[2] << 8)
            self._on_ble_rec_chunk(seq, payload[3:])
        elif cmd == protocol.CMD_REC_END and len(payload) >= 7:
            total_samples = struct.unpack_from("<I", payload, 1)[0]
            total_blocks = struct.unpack_from("<H", payload, 5)[0]
            self._on_ble_rec_end(total_samples, total_blocks)

    def _on_ble_rec_end(self, total_samples: int, total_blocks: int) -> None:
        if getattr(self, "ble_rec_ended", False):
            return
        self.ble_rec_ended = True
        got = len(self.ble_rec_chunks)
        missing = total_blocks - got
        self._log(f"BLE録音: 転送完了 {got}/{total_blocks}ブロック"
                  f"{f' ({missing}個欠落)' if missing > 0 else ''}")
        stream = b"".join(self.ble_rec_chunks[seq]
                          for seq in sorted(self.ble_rec_chunks))
        samples = adpcm.decode_stream(stream)
        self.ble_rec_chunks = {}
        if total_samples and missing == 0 and len(samples) != total_samples:
            self._log(f"BLE録音: サンプル数不一致 got={len(samples)} expected={total_samples}")
        if not samples:
            self._log("BLE録音: データなし")
            return
        self._save_wav(samples, protocol.REC_SAMPLE_RATE)

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
                if (self.link_var.get() == "BLE" and payload
                        and payload[0] in (protocol.CMD_REC_CHUNK, protocol.CMD_REC_END)):
                    self._handle_ble_rec_notify(payload)
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
                        if cmd == protocol.CMD_LOG_RESP:
                            self._on_log_resp(body)
                            continue
                        if cmd == protocol.CMD_IDLE_BEACON:
                            self._on_idle_beacon(body)
                            continue
                        if cmd == protocol.CMD_ACK:
                            continue  # 個別のコマンド成功応答(現状は無視でよい)
                        if cmd == protocol.CMD_NACK:
                            orig_cmd, orig_seq, err = struct.unpack("<BBB", body[:3])
                            name = protocol.NACK_ERRORS.get(err, str(err))
                            self._log(f"NACK: cmd=0x{orig_cmd:02X} seq={orig_seq} err={name}")
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

        # Phase E / Step C2: keep the board awake and reflect its low-power
        # state. The strict flag-driven FSM only leaves IDLE on an explicit
        # ENTER_COMM command - a plain NUL byte no longer has wake power, so
        # send ENTER_COMM once per second while "keep awake" is on.
        if self.transport is not None and self.keep_awake:
            self._send_enter_comm()
        if self.transport is None:
            self.power_state = "?"
        elif rate > 0:
            self.power_state = "ACTIVE"
        else:
            self.power_state = "IDLE (低消費電力)"
        if hasattr(self, "power_var"):
            self.power_var.set(f"電源: {self.power_state}")

        self.root.after(1000, self._update_rate)

    # ---------------- rendering ----------------
    def _tree_set(self, key: str, value: str) -> None:
        if self.tree.exists(key):
            self.tree.set(key, "value", value)

    DEVICE_STATE_LABELS = {0: "IDLE", 1: "ACTIVE(取得)", 2: "ACTIVE(通信)", 3: "ACTIVE(BLE音声送信)"}

    def _apply(self, st: protocol.Status) -> None:
        self.button_canvas.itemconfigure(
            self.button_led, fill="lime green" if st.button else "gray70")

        if st.device_state is not None:
            label = self.DEVICE_STATE_LABELS.get(st.device_state, f"?({st.device_state})")
            self.dev_state_var.set(f"状態: {label}")
        # uptime は MiniStatus v2(BLE)にも uptime_s として含まれる(秒粒度)ので
        # BLE接続時も更新する。以前は `if not st.compact:` で囲われており、BLE
        # 接続時にダッシュボードの多くの項目(uptime/加速度/ジャイロ/地磁気/
        # BLE・WiFi状態)が一切更新されないバグがあった。MiniStatus v2 は
        # これらを全て含む(protocol.decode_status の MINI_FMT_V2 参照)ので、
        # ここで更新してよい。MiniStatus に含まれないもの(メモリ使用量・波形)
        # だけを compact 時にスキップする。
        secs = st.uptime_ms // 1000
        self.uptime_var.set(f"{secs // 3600:02d}:{secs % 3600 // 60:02d}:{secs % 60:02d}")

        self.env_vars["temp"].set(f"{st.temp_c:.2f}")
        self.env_vars["hum"].set(f"{st.humidity:.1f}")
        self.env_vars["press"].set(f"{st.pressure_hpa:.1f}")
        self.env_vars["light"].set(str(st.light_raw))
        self.env_vars["tof"].set(str(st.tof_mm) if (st.tof_ok or st.compact) else "--")

        # 加速度/ジャイロ/地磁気/無線状態は MiniStatus v2 にも含まれるので
        # BLE接続時(compact)も更新する。
        self.mot_vars["acc"].set("({: d}, {: d}, {: d})".format(*st.acc_mg))
        self.mot_vars["gyro"].set("({:.1f}, {:.1f}, {:.1f})".format(*st.gyro_dps))
        self.mot_vars["mag"].set("({: d}, {: d}, {: d})".format(*st.mag_mgauss))
        self.radio_vars["ble"].set("OK" if st.ble_alive else "NG")
        self.radio_vars["wifi"].set("OK" if st.wifi_alive else "NG")

        if not st.compact:
            # メモリ使用量と生波形は FullStatus(UART/TCP)にしか無い。
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
        self._draw_spectrum(self.live_canvas, self.audio_view,
                            protocol.AUDIO_SAMPLE_RATE)

    # 周波数 [Hz] 対 音圧 [dB] のスペクトラム。直近の PCM サンプルに窓を掛けて
    # rFFT し、大きさを dB (20*log10) にして周波数軸に沿った縦棒で描く。
    _SPEC_FFT = 1024       # FFT長(2^n)。分解能 = fs/この値
    _SPEC_DB_FLOOR = -20.0  # 表示下限 [dB]
    _SPEC_DB_CEIL = 80.0    # 表示上限 [dB]

    def _draw_spectrum(self, canvas: tk.Canvas, samples: list[int],
                       sample_rate: int) -> None:
        canvas.delete("wave")
        n = self._SPEC_FFT
        if len(samples) < n:
            return
        width = canvas.winfo_width() or int(canvas.cget("width"))
        height = canvas.winfo_height() or int(canvas.cget("height"))
        pad_l, pad_b, pad_t = 44, 18, 8  # 軸ラベルの余白

        x = np.asarray(samples[-n:], dtype=np.float64)
        x -= x.mean()
        window = np.hanning(n)
        spec = np.abs(np.fft.rfft(x * window))
        # 振幅を dB に(窓のゲインで正規化、0除算回避に微小値を足す)
        db = 20.0 * np.log10(spec / (n * 0.25) + 1e-6)
        freqs = np.fft.rfftfreq(n, d=1.0 / sample_rate)  # 0 .. fs/2

        plot_w = width - pad_l - 6
        plot_h = height - pad_b - pad_t
        fmax = sample_rate / 2.0
        db_floor, db_ceil = self._SPEC_DB_FLOOR, self._SPEC_DB_CEIL

        def sx(f: float) -> float:
            return pad_l + (f / fmax) * plot_w

        def sy(d: float) -> float:
            d = max(db_floor, min(db_ceil, d))
            return pad_t + (1.0 - (d - db_floor) / (db_ceil - db_floor)) * plot_h

        # 周波数グリッド(2kHz刻み)と Hz ラベル
        step_hz = 2000
        f = 0
        while f <= fmax + 1:
            gx = sx(f)
            canvas.create_line(gx, pad_t, gx, pad_t + plot_h,
                               fill="gray25", tags="wave")
            canvas.create_text(gx, height - pad_b + 9, text=f"{f // 1000}k",
                               fill="gray55", font=("", 7), tags="wave")
            f += step_hz
        # dB グリッド(20dB刻み)と dB ラベル
        d = db_floor
        while d <= db_ceil + 1:
            gy = sy(d)
            canvas.create_line(pad_l, gy, pad_l + plot_w, gy,
                               fill="gray20", tags="wave")
            canvas.create_text(pad_l - 6, gy, text=f"{int(d)}", anchor="e",
                               fill="gray55", font=("", 7), tags="wave")
            d += 20.0

        # スペクトラム本体(塗りつぶしの縦棒風ポリライン)
        base_y = pad_t + plot_h
        pts = [pad_l, base_y]
        for fr, dv in zip(freqs, db):
            pts.extend((sx(fr), sy(dv)))
        pts.extend((pad_l + plot_w, base_y))
        canvas.create_polygon(*pts, fill="#0a3d1a", outline="", tags="wave")
        line_pts = []
        for fr, dv in zip(freqs, db):
            line_pts.extend((sx(fr), sy(dv)))
        canvas.create_line(*line_pts, fill="spring green", tags="wave")

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
