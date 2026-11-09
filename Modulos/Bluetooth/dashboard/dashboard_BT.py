#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
@file    bt_dashboard.py
@author  Mamani Flores Carlos (UTN FRT)
@brief   Dashboard de control y pruebas por Bluetooth / Serial para STM32 (HC-05).
@details Funciones principales:
           - Conexión en hilo aparte (la GUI nunca se congela) con reconexión automática.
           - Detección de puertos Bluetooth y actualización de la lista al desplegarla.
           - Indicador de enlace con el STM32 (según llegada de datos del firmware).
           - Indicador del estado real del LED (confirmado por el ACK del STM32).
           - Terminal con colores, filtro de telemetría, pausa y guardado.
           - Comandos A / B, comando libre con historial y terminador configurable.
           - Panel de PRUEBAS: alternar, parpadeo, ráfagas, stress, comando inválido, etc.
           - Macros personalizables (6 ranuras) guardadas entre sesiones.
           - Persistencia de la configuración entre sesiones.
@version 2.1
@date    2026

Protocolo esperado (firmware main.c):
    PC -> STM32 : caracteres 'A' (LED ON) y 'B' (LED OFF)
    STM32 -> PC : "ADC_VAL:<n> | TICK:<ms>\\n"                (cada 200 ms, solo se usa
                                                                para saber que hay enlace)
                  "[STM32] Comando A ejecutado: LED ON\\n"     (ACK)

Dependencias:  pip install pyserial   (tkinter viene con Python)
"""

from __future__ import annotations

import json
import queue
import re
import threading
import time
from collections import deque
from datetime import datetime, timedelta
from pathlib import Path

import tkinter as tk
from tkinter import filedialog, messagebox, ttk
from tkinter.scrolledtext import ScrolledText

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    raise SystemExit("Falta la librería pyserial. Instalala con:  pip install pyserial")


# ============================================================================
#                              CONSTANTES
# ============================================================================

APP_TITLE = "Dashboard Embebido STM32 · UTN FRT"
CONFIG_PATH = Path.home() / ".bt_dashboard.json"

MAX_TERM_LINES = 5000      # Líneas máximas en la terminal
MAX_PENDING = 20000        # Líneas retenidas mientras la terminal está en pausa
STALE_AFTER_S = 2.0        # Sin datos durante este tiempo => enlace "sin datos"
RECONNECT_DELAY_MS = 2000  # Espera entre reintentos de conexión
N_MACROS = 6               # Cantidad de macros personalizables

TELE_RE = re.compile(r"ADC_VAL:\s*(-?\d+)\s*\|\s*TICK:\s*(\d+)")
ACK_RE = re.compile(r"Comando\s+([AB])\s+ejecutado", re.IGNORECASE)

ENDINGS = {
    "Ninguno": "",
    "LF  (\\n)": "\n",
    "CR  (\\r)": "\r",
    "CR+LF  (\\r\\n)": "\r\n",
}
BAUDS = ["9600", "19200", "38400", "57600", "115200"]

C = {
    "bg": "#14161a", "panel": "#1d2026", "panel2": "#262a32", "border": "#323843",
    "fg": "#e6e9ef", "muted": "#8b93a1", "accent": "#4da3ff",
    "green": "#3ddc84", "red": "#ff5d5d", "yellow": "#ffc857",
    "cyan": "#5ce1e6", "orange": "#ff9f43", "term_bg": "#0d0f12",
}

PREFIX = {"tx": "TX ▶", "rx": "RX ◀", "tele": "RX ◀", "sys": "SYS  ", "err": "ERR  "}


# ============================================================================
#                      HILO DE COMUNICACIÓN SERIE
# ============================================================================

class SerialWorker(threading.Thread):
    """Abre el puerto y lee en segundo plano, armando líneas completas.

    Se comunica con la GUI únicamente a través de una cola de eventos
    ``(worker_id, tipo, dato)``, así Tkinter nunca se toca desde este hilo.

    Tipos de evento: ``opened``, ``open_error``, ``bytes``, ``line``, ``lost``.
    """

    def __init__(self, wid: int, port: str, baud: int, out_q: queue.Queue):
        super().__init__(daemon=True)
        self.wid = wid
        self.port = port
        self.baud = baud
        self.q = out_q
        self._halt = threading.Event()   # (no usar el nombre _stop: lo usa threading.Thread)
        self._lock = threading.Lock()
        self._ser: serial.Serial | None = None

    def _emit(self, kind: str, payload=None) -> None:
        self.q.put((self.wid, kind, payload))

    def stop(self) -> None:
        """Pide al hilo que termine y cierre el puerto."""
        self._halt.set()

    def write(self, data: bytes) -> None:
        """Escribe en el puerto (puede llamarse desde el hilo de la GUI)."""
        with self._lock:
            ser = self._ser
            if ser is None or not ser.is_open:
                raise serial.SerialException("El puerto no está abierto")
            ser.write(data)

    def run(self) -> None:
        try:
            ser = serial.Serial(self.port, self.baud, timeout=0.05, write_timeout=1.0)
        except (serial.SerialException, OSError, ValueError) as exc:
            self._emit("open_error", str(exc))
            return

        if self._halt.is_set():          # el usuario canceló mientras se abría
            ser.close()
            return

        with self._lock:
            self._ser = ser
        self._emit("opened")

        buf = bytearray()
        last_data = time.monotonic()
        try:
            while not self._halt.is_set():
                chunk = ser.read(ser.in_waiting or 1)
                now = time.monotonic()
                if chunk:
                    last_data = now
                    self._emit("bytes", len(chunk))
                    buf.extend(chunk)
                    # Separar por '\n' (el firmware termina cada mensaje con '\n')
                    while True:
                        i = buf.find(b"\n")
                        if i < 0:
                            break
                        line = bytes(buf[:i + 1])
                        del buf[:i + 1]
                        self._emit("line", line.decode("utf-8", "replace").rstrip("\r\n"))
                    if len(buf) > 4096:  # protección ante basura sin '\n'
                        self._emit("line", bytes(buf).decode("utf-8", "replace"))
                        buf.clear()
                elif buf and (now - last_data) > 0.5:
                    # Resto sin terminador: se publica igual para no perder datos
                    self._emit("line", bytes(buf).decode("utf-8", "replace").rstrip("\r\n"))
                    buf.clear()
        except (serial.SerialException, OSError) as exc:
            if not self._halt.is_set():
                self._emit("lost", str(exc))
        finally:
            with self._lock:
                self._ser = None
            try:
                ser.close()
            except Exception:
                pass


# ============================================================================
#                           APLICACIÓN PRINCIPAL
# ============================================================================

class Dashboard:
    """Ventana principal del dashboard."""

    POLL_MS = 30

    # ------------------------------------------------------------------ init
    def __init__(self, root: tk.Tk):
        self.root = root
        self.cfg = self._load_config()
        self.q: queue.Queue = queue.Queue()

        # Conexión
        self.worker: SerialWorker | None = None
        self.worker_id = 0
        self.state = "idle"          # idle | connecting | connected | reconnecting
        self.user_disconnect = True
        self.reconnect_job = None
        self.reconnect_attempt = 0
        self._port = ""
        self._baud = 9600
        self.port_map: dict[str, str] = {}
        self.bt_labels: list[str] = []

        # Contadores
        self.rx_bytes = 0
        self.tx_bytes = 0
        self.connected_at: float | None = None
        self.last_rx: float | None = None
        self.led_on: bool | None = None

        # Secuencias de prueba
        self.seq_cmds: list[str] = []
        self.seq_i = 0
        self.seq_job = None
        self.test_buttons: list[ttk.Button] = []
        self.macro_vars: list[tk.StringVar] = []
        self.macro_buttons: list[ttk.Button] = []

        # Registro
        self.full_log: deque[str] = deque(maxlen=50000)
        self.pending: list[tuple[str, str, str]] = []

        # Historial de comandos
        self.history: list[str] = []
        self.hist_idx = 0

        self._setup_style()
        self._build_ui()
        self._bind_keys()
        self.refresh_ports(select_saved=True)
        self._set_state("idle")
        self._set_led(None)

        self.root.protocol("WM_DELETE_WINDOW", self.on_close)
        self.root.after(self.POLL_MS, self._poll_queue)
        self.root.after(500, self._tick_status)

    # ---------------------------------------------------------- configuración
    @staticmethod
    def _load_config() -> dict:
        try:
            return json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
        except Exception:
            return {}

    def _save_config(self) -> None:
        data = {
            "port": self.port_map.get(self.port_var.get(), self.port_var.get().split(" — ")[0].strip()),
            "baud": self.baud_var.get(),
            "ending": self.ending_var.get(),
            "auto_reconnect": self.reconnect_var.get(),
            "autoscroll": self.autoscroll_var.get(),
            "timestamp": self.ts_var.get(),
            "hide_tele": self.hide_tele_var.get(),
            "test_count": self.count_var.get(),
            "test_interval": self.interval_var.get(),
            "macros": [v.get() for v in self.macro_vars],
            "geometry": f"{self.root.winfo_width()}x{self.root.winfo_height()}",
        }
        try:
            CONFIG_PATH.write_text(json.dumps(data, indent=2), encoding="utf-8")
        except OSError:
            pass

    # ----------------------------------------------------------------- estilo
    def _setup_style(self) -> None:
        r = self.root
        r.title(APP_TITLE)
        r.configure(bg=C["bg"])
        r.geometry(self.cfg.get("geometry", "1120x760"))
        r.minsize(900, 640)

        s = ttk.Style()
        s.theme_use("clam")
        s.configure(".", background=C["panel"], foreground=C["fg"],
                    fieldbackground=C["panel2"], bordercolor=C["border"],
                    lightcolor=C["panel"], darkcolor=C["panel"], font=("Segoe UI", 10))
        s.configure("TFrame", background=C["panel"])
        s.configure("TLabel", background=C["panel"], foreground=C["fg"])
        s.configure("Muted.TLabel", foreground=C["muted"])
        s.configure("TCheckbutton", background=C["panel"], foreground=C["fg"])
        s.map("TCheckbutton", background=[("active", C["panel"])],
              indicatorcolor=[("selected", C["accent"]), ("!selected", C["panel2"])])
        s.configure("TCombobox", fieldbackground=C["panel2"], background=C["panel2"],
                    foreground=C["fg"], arrowcolor=C["fg"], bordercolor=C["border"])
        s.map("TCombobox",
              fieldbackground=[("readonly", C["panel2"]), ("disabled", C["panel"])],
              foreground=[("readonly", C["fg"]), ("disabled", C["muted"])],
              selectbackground=[("readonly", C["panel2"])],
              selectforeground=[("readonly", C["fg"])])
        s.configure("TEntry", fieldbackground=C["panel2"], foreground=C["fg"],
                    insertcolor=C["fg"], bordercolor=C["border"])
        s.configure("TSpinbox", fieldbackground=C["panel2"], foreground=C["fg"],
                    background=C["panel2"], arrowcolor=C["fg"], insertcolor=C["fg"],
                    bordercolor=C["border"])
        s.configure("TPanedwindow", background=C["bg"])
        for name, col, hov in (("Green", "#2e9e5b", "#37b86a"), ("Red", "#c0392b", "#d64a3b"),
                               ("Blue", "#2b7de0", "#3b8df0"), ("Gray", "#3a404b", "#4a515e"),
                               ("Purple", "#7b4fc4", "#8d62d6"), ("Amber", "#b9770e", "#d38a17")):
            s.configure(f"{name}.TButton", background=col, foreground="white",
                        font=("Segoe UI", 10, "bold"), padding=(10, 6),
                        borderwidth=0, focusthickness=0)
            s.map(f"{name}.TButton",
                  background=[("disabled", "#2a2e36"), ("active", hov)],
                  foreground=[("disabled", "#6b7280")])

        r.option_add("*TCombobox*Listbox.background", C["panel2"])
        r.option_add("*TCombobox*Listbox.foreground", C["fg"])
        r.option_add("*TCombobox*Listbox.selectBackground", C["accent"])
        r.option_add("*TCombobox*Listbox.selectForeground", "white")

    # ------------------------------------------------------------------ UI
    def _card(self, parent, title: str):
        """Crea un panel con título. Devuelve (contenedor_externo, cuerpo)."""
        outer = tk.Frame(parent, bg=C["panel"], highlightbackground=C["border"],
                         highlightthickness=1)
        tk.Label(outer, text=title, bg=C["panel"], fg=C["accent"],
                 font=("Segoe UI", 10, "bold"), anchor="w").pack(fill="x", padx=10, pady=(8, 2))
        body = ttk.Frame(outer, padding=(10, 4, 10, 10))
        body.pack(fill="both", expand=True)
        return outer, body

    def _build_ui(self) -> None:
        cfg = self.cfg

        # ---- Barra de estado (abajo) ----
        sb = tk.Frame(self.root, bg=C["panel2"])
        sb.pack(side="bottom", fill="x")
        self.lbl_sb_left = tk.Label(sb, text="", bg=C["panel2"], fg=C["fg"], anchor="w")
        self.lbl_sb_left.pack(side="left", padx=10, pady=3)
        self.lbl_sb_right = tk.Label(sb, text="", bg=C["panel2"], fg=C["muted"], anchor="e")
        self.lbl_sb_right.pack(side="right", padx=10, pady=3)
        self.lbl_sb_mid = tk.Label(sb, text="", bg=C["panel2"], fg=C["muted"])
        self.lbl_sb_mid.pack(side="right", padx=20, pady=3)

        # ---- Barra de conexión ----
        bar_outer = tk.Frame(self.root, bg=C["panel"], highlightbackground=C["border"],
                             highlightthickness=1)
        bar_outer.pack(fill="x", padx=10, pady=(10, 6))
        bar = ttk.Frame(bar_outer, padding=8)
        bar.pack(fill="x")

        ttk.Label(bar, text="Puerto").pack(side="left")
        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(bar, textvariable=self.port_var, width=38,
                                       postcommand=self.refresh_ports)
        self.port_combo.pack(side="left", padx=(6, 4))
        ttk.Button(bar, text="⟳", width=3, style="Gray.TButton",
                   command=lambda: self.refresh_ports()).pack(side="left", padx=(0, 12))

        ttk.Label(bar, text="Baudios").pack(side="left")
        self.baud_var = tk.StringVar(value=str(cfg.get("baud", "9600")))
        self.baud_combo = ttk.Combobox(bar, textvariable=self.baud_var, values=BAUDS, width=8)
        self.baud_combo.pack(side="left", padx=(6, 12))

        self.btn_connect = ttk.Button(bar, text="Conectar", style="Green.TButton",
                                      width=12, command=self.toggle_connection)
        self.btn_connect.pack(side="left")

        self.reconnect_var = tk.BooleanVar(value=cfg.get("auto_reconnect", True))
        ttk.Checkbutton(bar, text="Reconexión automática",
                        variable=self.reconnect_var).pack(side="left", padx=14)

        self.lbl_state = tk.Label(bar, text="● Desconectado", bg=C["panel"], fg=C["red"],
                                  font=("Segoe UI", 10, "bold"))
        self.lbl_state.pack(side="right", padx=6)

        # ---- Área principal (terminal | panel derecho) ----
        paned = ttk.PanedWindow(self.root, orient="horizontal")
        paned.pack(fill="both", expand=True, padx=10, pady=(0, 8))

        left = ttk.Frame(paned)
        right = ttk.Frame(paned)
        paned.add(left, weight=3)
        paned.add(right, weight=2)

        self._build_terminal(left)
        self._build_side_panel(right)

    # ---- Terminal ----
    def _build_terminal(self, parent) -> None:
        cfg = self.cfg
        outer, body = self._card(parent, "TERMINAL")
        outer.pack(fill="both", expand=True)

        opts = ttk.Frame(body)
        opts.pack(fill="x", pady=(0, 6))
        self.autoscroll_var = tk.BooleanVar(value=cfg.get("autoscroll", True))
        self.ts_var = tk.BooleanVar(value=cfg.get("timestamp", True))
        self.hide_tele_var = tk.BooleanVar(value=cfg.get("hide_tele", False))
        self.pause_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(opts, text="Autoscroll", variable=self.autoscroll_var).pack(side="left")
        ttk.Checkbutton(opts, text="Hora", variable=self.ts_var).pack(side="left", padx=8)
        ttk.Checkbutton(opts, text="Ocultar telemetría",
                        variable=self.hide_tele_var).pack(side="left")
        ttk.Checkbutton(opts, text="Pausa", variable=self.pause_var,
                        command=self._on_pause_toggle).pack(side="left", padx=8)
        ttk.Button(opts, text="Guardar", style="Gray.TButton",
                   command=self.save_log).pack(side="right")
        ttk.Button(opts, text="Limpiar", style="Gray.TButton",
                   command=self.clear_terminal).pack(side="right", padx=6)

        self.terminal = ScrolledText(body, bg=C["term_bg"], fg=C["fg"],
                                     insertbackground=C["fg"], relief="flat", bd=0,
                                     font=("Consolas", 10), wrap="word", state="disabled",
                                     highlightthickness=1, highlightbackground=C["border"])
        self.terminal.pack(fill="both", expand=True)
        for tag, color in (("ts", C["muted"]), ("tx", C["cyan"]), ("rx", C["green"]),
                           ("tele", "#6b8f78"), ("sys", C["yellow"]), ("err", C["red"])):
            self.terminal.tag_configure(tag, foreground=color)

        # Línea de comandos
        cmd = ttk.Frame(body)
        cmd.pack(fill="x", pady=(8, 0))
        ttk.Label(cmd, text="▶", style="Muted.TLabel").pack(side="left", padx=(0, 6))
        self.cmd_var = tk.StringVar()
        self.cmd_entry = ttk.Entry(cmd, textvariable=self.cmd_var)
        self.cmd_entry.pack(side="left", fill="x", expand=True)
        self.ending_var = tk.StringVar(value=cfg.get("ending", "LF  (\\n)"))
        if self.ending_var.get() not in ENDINGS:
            self.ending_var.set("LF  (\\n)")
        ttk.Combobox(cmd, textvariable=self.ending_var, values=list(ENDINGS), width=13,
                     state="readonly").pack(side="left", padx=6)
        self.btn_send = ttk.Button(cmd, text="Enviar", style="Blue.TButton",
                                   command=self.send_custom)
        self.btn_send.pack(side="left")

    # ---- Panel derecho ----
    def _build_side_panel(self, parent) -> None:
        cfg = self.cfg

        # --- Control ---
        k_outer, k = self._card(parent, "CONTROL DEL STM32")
        k_outer.pack(fill="x")

        led_row = ttk.Frame(k)
        led_row.pack(fill="x", pady=(0, 8))
        self.led_canvas = tk.Canvas(led_row, width=26, height=26, bg=C["panel"],
                                    highlightthickness=0)
        self.led_canvas.pack(side="left")
        self.led_dot = self.led_canvas.create_oval(3, 3, 23, 23, fill="#555", outline="")
        self.lbl_led = tk.Label(led_row, text="LED LD3: desconocido", bg=C["panel"],
                                fg=C["fg"], font=("Segoe UI", 11))
        self.lbl_led.pack(side="left", padx=8)
        self.lbl_link = tk.Label(led_row, text="● Sin enlace", bg=C["panel"], fg=C["muted"],
                                 font=("Segoe UI", 10, "bold"))
        self.lbl_link.pack(side="right")

        btns = ttk.Frame(k)
        btns.pack(fill="x")
        btns.columnconfigure((0, 1), weight=1)
        self.btn_a = ttk.Button(btns, text="A · LED ON   (F1)", style="Green.TButton",
                                command=lambda: self.send_cmd("A"))
        self.btn_a.grid(row=0, column=0, sticky="ew", padx=(0, 4))
        self.btn_b = ttk.Button(btns, text="B · LED OFF   (F2)", style="Red.TButton",
                                command=lambda: self.send_cmd("B"))
        self.btn_b.grid(row=0, column=1, sticky="ew", padx=(4, 0))

        # --- Pruebas ---
        p_outer, p = self._card(parent, "PRUEBAS")
        p_outer.pack(fill="x", pady=(8, 0))

        params = ttk.Frame(p)
        params.pack(fill="x", pady=(0, 8))
        ttk.Label(params, text="Repeticiones", style="Muted.TLabel").pack(side="left")
        self.count_var = tk.StringVar(value=str(cfg.get("test_count", 5)))
        ttk.Spinbox(params, from_=1, to=1000, width=6,
                    textvariable=self.count_var).pack(side="left", padx=(6, 16))
        ttk.Label(params, text="Intervalo (ms)", style="Muted.TLabel").pack(side="left")
        self.interval_var = tk.StringVar(value=str(cfg.get("test_interval", 300)))
        ttk.Spinbox(params, from_=20, to=10000, increment=50, width=7,
                    textvariable=self.interval_var).pack(side="left", padx=6)

        grid = ttk.Frame(p)
        grid.pack(fill="x")
        grid.columnconfigure((0, 1), weight=1)

        tests = [
            ("⇄ Alternar LED",       "Purple.TButton", self.test_toggle),
            ("✦ Parpadeo ×N",        "Purple.TButton", self.test_blink),
            ("A ×N  (ráfaga ON)",    "Blue.TButton",   lambda: self.test_burst("A")),
            ("B ×N  (ráfaga OFF)",   "Blue.TButton",   lambda: self.test_burst("B")),
            ("⚡ Stress A/B  5 s",    "Amber.TButton",  self.test_stress),
            ("ABAB  (cadena)",       "Blue.TButton",   self.test_string),
            ("? Comando inválido (X)", "Amber.TButton", lambda: self.send_cmd("X")),
            ("↵ Solo terminador",    "Gray.TButton",   self.test_terminator_only),
        ]
        for i, (text, style, cmd) in enumerate(tests):
            b = ttk.Button(grid, text=text, style=style, command=cmd)
            b.grid(row=i // 2, column=i % 2, sticky="ew",
                   padx=(0, 4) if i % 2 == 0 else (4, 0), pady=3)
            self.test_buttons.append(b)

        self.btn_stop = ttk.Button(grid, text="■ DETENER PRUEBA", style="Red.TButton",
                                   command=self._stop_sequence)
        self.btn_stop.grid(row=len(tests) // 2, column=0, columnspan=2,
                           sticky="ew", pady=(6, 0))
        self.btn_stop.state(["disabled"])

        # --- Macros ---
        m_outer, m = self._card(parent, "MACROS  (texto editable)")
        m_outer.pack(fill="x", pady=(8, 0))
        m.columnconfigure(1, weight=1)
        saved = cfg.get("macros", [])
        defaults = ["A", "B", "AB", "BA", "?", "HOLA"]
        for i in range(N_MACROS):
            val = saved[i] if i < len(saved) else defaults[i % len(defaults)]
            var = tk.StringVar(value=val)
            self.macro_vars.append(var)
            ttk.Label(m, text=f"M{i + 1}", style="Muted.TLabel").grid(
                row=i, column=0, padx=(0, 6), pady=2)
            ttk.Entry(m, textvariable=var).grid(row=i, column=1, sticky="ew", pady=2)
            b = ttk.Button(m, text="Enviar", style="Gray.TButton",
                           command=lambda idx=i: self.send_macro(idx))
            b.grid(row=i, column=2, padx=(6, 0), pady=2)
            self.macro_buttons.append(b)

    def _bind_keys(self) -> None:
        r = self.root
        r.bind("<F5>", lambda _e: self.refresh_ports())
        r.bind("<F1>", lambda _e: self.send_cmd("A"))
        r.bind("<F2>", lambda _e: self.send_cmd("B"))
        r.bind("<F3>", lambda _e: self.test_toggle())
        r.bind("<Escape>", lambda _e: self._stop_sequence())
        r.bind("<Control-l>", lambda _e: self.clear_terminal())
        r.bind("<Control-s>", lambda _e: self.save_log())
        self.cmd_entry.bind("<Return>", lambda _e: self.send_custom())
        self.cmd_entry.bind("<Up>", lambda _e: self._history_move(-1))
        self.cmd_entry.bind("<Down>", lambda _e: self._history_move(1))

    # ================================================================ PUERTOS
    @staticmethod
    def _is_bluetooth(p) -> bool:
        text = f"{p.description} {p.hwid} {p.device}".lower()
        return any(k in text for k in ("bluetooth", "bthenum", "rfcomm"))

    def refresh_ports(self, select_saved: bool = False) -> None:
        """Actualiza la lista de puertos (marca los Bluetooth con [BT])."""
        try:
            ports = sorted(list_ports.comports(), key=lambda p: p.device)
        except Exception as exc:  # noqa: BLE001
            self.log(f"No se pudo listar los puertos: {exc}", "err")
            return
        self.port_map = {}
        self.bt_labels = []
        items = []
        for p in ports:
            desc = p.description if p.description and p.description != "n/a" else ""
            label = f"{p.device} — {desc}" if desc else p.device
            if self._is_bluetooth(p):
                label += "  [BT]"
                self.bt_labels.append(label)
            self.port_map[label] = p.device
            items.append(label)
        self.port_combo["values"] = items

        if select_saved or not self.port_var.get().strip():
            self._pick_default_port(items)

    def _pick_default_port(self, items: list[str]) -> None:
        saved = self.cfg.get("port")
        for label, dev in self.port_map.items():
            if dev == saved:
                self.port_var.set(label)
                return
        if self.bt_labels:
            self.port_var.set(self.bt_labels[0])
        elif items:
            self.port_var.set(items[0])

    # ============================================================== CONEXIÓN
    def toggle_connection(self) -> None:
        if self.state == "idle":
            self.connect()
        else:
            self.disconnect()

    def connect(self) -> None:
        text = self.port_var.get().strip()
        if not text:
            messagebox.showwarning("Puerto", "Seleccioná un puerto primero.")
            return
        port = self.port_map.get(text) or text.split(" — ")[0].split()[0]
        try:
            baud = int(self.baud_var.get())
        except ValueError:
            messagebox.showwarning("Baudios", "La velocidad debe ser un número entero.")
            return
        self.user_disconnect = False
        self.reconnect_attempt = 0
        self._cancel_reconnect()
        self._start_worker(port, baud)

    def _start_worker(self, port: str, baud: int) -> None:
        self.worker_id += 1
        self._port, self._baud = port, baud
        self.worker = SerialWorker(self.worker_id, port, baud, self.q)
        self._set_state("connecting")
        if self.reconnect_attempt == 0:
            self.log(f"Conectando a {port} @ {baud} bauds…", "sys")
        self.worker.start()

    def disconnect(self) -> None:
        self.user_disconnect = True
        self._cancel_reconnect()
        worker, self.worker = self.worker, None
        if worker:
            worker.stop()
        self._on_link_closed("Desconectado del puerto.")

    def _on_link_closed(self, message: str, reconnecting: bool = False) -> None:
        self._stop_sequence(silent=True)
        self.connected_at = None
        self.last_rx = None
        self._set_led(None)
        self._set_state("reconnecting" if reconnecting else "idle")
        if message:
            self.log(message, "sys")

    def _schedule_reconnect(self) -> None:
        self.reconnect_attempt += 1
        self._set_state("reconnecting")
        self._cancel_reconnect()
        self.reconnect_job = self.root.after(RECONNECT_DELAY_MS, self._do_reconnect)

    def _cancel_reconnect(self) -> None:
        if self.reconnect_job is not None:
            self.root.after_cancel(self.reconnect_job)
            self.reconnect_job = None

    def _do_reconnect(self) -> None:
        self.reconnect_job = None
        if self.user_disconnect:
            return
        self._start_worker(self._port, self._baud)

    def _set_state(self, state: str) -> None:
        self.state = state
        look = {
            "idle":         ("● Desconectado", C["red"], "Conectar", "Green.TButton"),
            "connecting":   ("● Conectando…", C["yellow"], "Cancelar", "Red.TButton"),
            "connected":    ("● Conectado", C["green"], "Desconectar", "Red.TButton"),
            "reconnecting": ("● Reconectando…", C["orange"], "Cancelar", "Red.TButton"),
        }[state]
        self.lbl_state.config(text=look[0], fg=look[1])
        self.btn_connect.config(text=look[2], style=look[3])

        connected = state == "connected"
        flag = ["!disabled"] if connected else ["disabled"]
        for b in (self.btn_a, self.btn_b, self.btn_send,
                  *self.test_buttons, *self.macro_buttons):
            b.state(flag)
        editable = state == "idle"
        self.port_combo.config(state="normal" if editable else "disabled")
        self.baud_combo.config(state="normal" if editable else "disabled")

    # ======================================================== EVENTOS DEL HILO
    def _poll_queue(self) -> None:
        try:
            for _ in range(500):
                wid, kind, payload = self.q.get_nowait()
                if wid != self.worker_id or self.worker is None:
                    continue              # evento de un hilo ya descartado
                self._handle_event(kind, payload)
        except queue.Empty:
            pass
        self.root.after(self.POLL_MS, self._poll_queue)

    def _handle_event(self, kind: str, payload) -> None:
        if kind == "opened":
            self.reconnect_attempt = 0
            self.rx_bytes = self.tx_bytes = 0
            self.connected_at = time.monotonic()
            self._set_state("connected")
            self.log(f"Conectado a {self._port} @ {self._baud} bauds.", "sys")
            self._save_config()
            self.cmd_entry.focus_set()

        elif kind == "bytes":
            self.rx_bytes += payload

        elif kind == "line":
            self._process_line(payload)

        elif kind == "open_error":
            self.worker = None
            if not self.user_disconnect and self.reconnect_var.get():
                if self.reconnect_attempt == 0:
                    self.log(f"No se pudo abrir el puerto: {payload}", "err")
                self._on_link_closed("", reconnecting=True)
                self._schedule_reconnect()
                self.log(f"Reintento #{self.reconnect_attempt} en "
                         f"{RECONNECT_DELAY_MS // 1000} s…", "sys")
            else:
                self._on_link_closed("")
                self.log(f"No se pudo abrir el puerto: {payload}", "err")

        elif kind == "lost":
            self.worker = None
            self.log(f"Se perdió la conexión: {payload}", "err")
            if not self.user_disconnect and self.reconnect_var.get():
                self._on_link_closed("", reconnecting=True)
                self._schedule_reconnect()
                self.log("Reconectando automáticamente…", "sys")
            else:
                self._on_link_closed("Conexión cerrada.")

    # ========================================================== PROCESAMIENTO
    def _process_line(self, text: str) -> None:
        self.last_rx = time.monotonic()
        if TELE_RE.search(text):
            self.log(text, "tele", telemetry=True)
            return
        ack = ACK_RE.search(text)
        if ack:
            self._set_led(ack.group(1).upper() == "A")
        self.log(text, "rx")

    def _set_led(self, on: bool | None) -> None:
        self.led_on = on
        if on is None:
            color, text = "#555555", "LED LD3: desconocido"
        elif on:
            color, text = C["green"], "LED LD3: ENCENDIDO"
        else:
            color, text = "#7a2a2a", "LED LD3: APAGADO"
        self.led_canvas.itemconfig(self.led_dot, fill=color)
        self.lbl_led.config(text=text)

    # ================================================================ ENVÍO
    def _ending(self) -> str:
        return ENDINGS.get(self.ending_var.get(), "")

    def send_raw(self, text: str) -> bool:
        if self.state != "connected" or self.worker is None:
            self.log("No hay conexión activa: no se envió nada.", "err")
            return False
        data = text.encode("utf-8")
        try:
            self.worker.write(data)
        except (serial.SerialException, OSError) as exc:
            self.log(f"Error de TX: {exc}", "err")
            return False
        self.tx_bytes += len(data)
        shown = text.replace("\r", "\\r").replace("\n", "\\n")
        self.log(shown, "tx")
        return True

    def send_cmd(self, char: str) -> bool:
        return self.send_raw(char + self._ending())

    def send_custom(self) -> None:
        text = self.cmd_var.get()
        if not text:
            return
        if self.send_raw(text + self._ending()):
            if not self.history or self.history[-1] != text:
                self.history.append(text)
            self.hist_idx = len(self.history)
            self.cmd_var.set("")

    def send_macro(self, idx: int) -> None:
        text = self.macro_vars[idx].get()
        if text:
            self.send_raw(text + self._ending())

    def _history_move(self, step: int) -> str:
        if not self.history:
            return "break"
        self.hist_idx = max(0, min(len(self.history), self.hist_idx + step))
        self.cmd_var.set(self.history[self.hist_idx] if self.hist_idx < len(self.history) else "")
        self.cmd_entry.icursor("end")
        return "break"

    # ============================================================== PRUEBAS
    def _test_count(self) -> int:
        try:
            return max(1, min(1000, int(self.count_var.get())))
        except ValueError:
            return 5

    def _test_interval(self) -> int:
        try:
            return max(20, min(10000, int(self.interval_var.get())))
        except ValueError:
            return 300

    def test_toggle(self) -> None:
        """Envía lo contrario del estado actual del LED (si no se sabe, 'A')."""
        if self.state != "connected":
            return
        self.send_cmd("B" if self.led_on else "A")

    def test_blink(self) -> None:
        n = self._test_count()
        self._start_sequence(["A", "B"] * n, f"parpadeo ×{n}")

    def test_burst(self, char: str) -> None:
        n = self._test_count()
        self._start_sequence([char] * n, f"ráfaga '{char}' ×{n}")

    def test_stress(self) -> None:
        interval = self._test_interval()
        n = max(2, min(2000, 5000 // interval))
        cmds = ["A" if i % 2 == 0 else "B" for i in range(n)]
        self._start_sequence(cmds, f"stress A/B ({n} envíos, ~5 s)")

    def test_string(self) -> None:
        self.send_raw("ABAB" + self._ending())

    def test_terminator_only(self) -> None:
        end = self._ending()
        if not end:
            self.log("El terminador está en 'Ninguno': no hay nada que enviar.", "err")
            return
        self.send_raw(end)

    def _start_sequence(self, cmds: list[str], label: str) -> None:
        if self.state != "connected":
            self.log("No hay conexión activa: no se inició la prueba.", "err")
            return
        self._stop_sequence(silent=True)
        self.seq_cmds = cmds
        self.seq_i = 0
        self.log(f"Prueba: {label}, cada {self._test_interval()} ms "
                 f"(Esc para detener).", "sys")
        self.btn_stop.state(["!disabled"])
        self._seq_step()

    def _seq_step(self) -> None:
        self.seq_job = None
        if self.seq_i >= len(self.seq_cmds):
            self._stop_sequence(finished=True)
            return
        cmd = self.seq_cmds[self.seq_i]
        self.seq_i += 1
        if not self.send_cmd(cmd):
            self._stop_sequence()
            return
        self.seq_job = self.root.after(self._test_interval(), self._seq_step)

    def _stop_sequence(self, silent: bool = False, finished: bool = False) -> None:
        running = self.seq_job is not None or bool(self.seq_cmds)
        if self.seq_job is not None:
            self.root.after_cancel(self.seq_job)
            self.seq_job = None
        self.seq_cmds = []
        self.btn_stop.state(["disabled"])
        if running and not silent:
            self.log("Prueba finalizada." if finished else "Prueba detenida.", "sys")

    # ============================================================== TERMINAL
    def log(self, text: str, tag: str = "sys", telemetry: bool = False) -> None:
        """Agrega una línea al registro y (si corresponde) a la terminal."""
        ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        self.full_log.append(f"{ts}  {PREFIX[tag]} {text}")
        if telemetry and self.hide_tele_var.get():
            return
        if self.pause_var.get():
            if len(self.pending) < MAX_PENDING:
                self.pending.append((ts, tag, text))
            return
        self._write_term(ts, tag, text)

    def _write_term(self, ts: str, tag: str, text: str) -> None:
        t = self.terminal
        t.configure(state="normal")
        if self.ts_var.get():
            t.insert("end", ts + "  ", "ts")
        t.insert("end", f"{PREFIX[tag]} {text}\n", tag)
        total = int(t.index("end-1c").split(".")[0])
        if total > MAX_TERM_LINES:
            t.delete("1.0", f"{total - MAX_TERM_LINES}.0")
        if self.autoscroll_var.get():
            t.see("end")
        t.configure(state="disabled")

    def _on_pause_toggle(self) -> None:
        if not self.pause_var.get():
            for ts, tag, text in self.pending:
                self._write_term(ts, tag, text)
            self.pending.clear()

    def clear_terminal(self) -> None:
        self.terminal.configure(state="normal")
        self.terminal.delete("1.0", "end")
        self.terminal.configure(state="disabled")
        self.full_log.clear()
        self.pending.clear()

    def save_log(self) -> None:
        if not self.full_log:
            messagebox.showinfo("Guardar", "No hay nada para guardar todavía.")
            return
        path = filedialog.asksaveasfilename(
            defaultextension=".txt",
            initialfile=f"log_{datetime.now():%Y%m%d_%H%M%S}.txt",
            filetypes=[("Texto", "*.txt"), ("Todos", "*.*")])
        if not path:
            return
        try:
            Path(path).write_text("\n".join(self.full_log) + "\n", encoding="utf-8")
            self.log(f"Registro guardado en {path}", "sys")
        except OSError as exc:
            messagebox.showerror("Guardar", f"No se pudo guardar:\n{exc}")

    # ============================================================ ESTADO (1/2 s)
    @staticmethod
    def _fmt_bytes(n: int) -> str:
        if n < 1024:
            return f"{n} B"
        return f"{n / 1024:.1f} KB"

    def _tick_status(self) -> None:
        now = time.monotonic()
        if self.state == "connected":
            fresh = self.last_rx is not None and (now - self.last_rx) < STALE_AFTER_S
            if fresh:
                self.lbl_link.config(text="● STM32 enlazado", fg=C["green"])
            elif self.last_rx is None:
                self.lbl_link.config(text="● Esperando datos…", fg=C["yellow"])
            else:
                self.lbl_link.config(
                    text=f"● Sin datos hace {int(now - self.last_rx)} s", fg=C["orange"])
            up = str(timedelta(seconds=int(now - (self.connected_at or now))))
            self.lbl_sb_left.config(text=f"{self._port} @ {self._baud}")
            self.lbl_sb_right.config(text=f"Conectado hace {up}")
        else:
            self.lbl_link.config(text="● Sin enlace", fg=C["muted"])
            self.lbl_sb_left.config(text="Sin conexión")
            self.lbl_sb_right.config(text="")
        self.lbl_sb_mid.config(
            text=f"RX {self._fmt_bytes(self.rx_bytes)}   ·   TX {self._fmt_bytes(self.tx_bytes)}")
        self.root.after(500, self._tick_status)

    # ================================================================ CIERRE
    def on_close(self) -> None:
        self.user_disconnect = True
        self._cancel_reconnect()
        self._stop_sequence(silent=True)
        if self.worker:
            self.worker.stop()
        self._save_config()
        self.root.destroy()


# ============================================================================
#                                  MAIN
# ============================================================================

def main() -> None:
    root = tk.Tk()
    Dashboard(root)
    root.mainloop()


if __name__ == "__main__":
    main()