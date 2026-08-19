#!/usr/bin/env python3
"""One-click firmware flasher for Marauder Eternal Mini V3 devices."""

from __future__ import annotations

import contextlib
import hashlib
import os
import queue
import re
import sys
import threading
import traceback
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterable

import tkinter as tk
from tkinter import filedialog, font as tkfont, messagebox, ttk

try:
    from serial.tools import list_ports
except ImportError:  # Shown as a useful GUI error instead of failing at import.
    list_ports = None


APP_NAME = "Marauder Eternal Flasher"
APP_VERSION = "1.1.0"
FIRMWARE_VERSION = "1.14.3"
INCLUDED_FILENAME = "Marauder_Eternal_1.14.3_MiniV3_ESP32-C5.bin"
INCLUDED_SHA256 = "4933f510e8b7bb6d71ece62285606e30733621bb245f2b32ea478c469a80dec1"
FLASH_SIZE = 8 * 1024 * 1024
APPLICATION_OFFSET = 0x10000
APPLICATION_LIMIT = 0x3D0000 - APPLICATION_OFFSET
ESP_IMAGE_MAGIC = 0xE9
ESP32_C5_IMAGE_ID = 0x17
PARTITION_MAGIC = b"\xaa\x50"
FLASH_BAUD = "460800"
MIN_ACTIVITY_LINES = 7
DEFAULT_WINDOW_WIDTH = 720
DEFAULT_WINDOW_HEIGHT = 720

ANSI_ESCAPE_RE = re.compile(r"\x1b(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~])")
PERCENT_RE = re.compile(r"(?<![\d.])(100(?:\.0+)?|\d{1,2}(?:\.\d+)?)\s*%")


@dataclass(frozen=True)
class PortEntry:
    device: str
    description: str
    hwid: str
    vid: int | None = None
    pid: int | None = None

    @property
    def label(self) -> str:
        description = self.description.strip() if self.description else "Serial device"
        return f"{self.device}  —  {description}"


@dataclass(frozen=True)
class FirmwareImage:
    path: Path
    image_type: str
    offset: int
    size: int
    sha256: str
    included: bool = False

    @property
    def offset_text(self) -> str:
        return f"0x{self.offset:x}"

    @property
    def size_text(self) -> str:
        return f"{self.size / (1024 * 1024):.2f} MB"

    @property
    def detail(self) -> str:
        return f"{self.image_type}  •  {self.size_text}  •  writes at {self.offset_text}"


def resource_path() -> Path:
    """Return the included image path in source and bundled builds."""
    bundle_root = getattr(sys, "_MEIPASS", None)
    if bundle_root:
        return Path(bundle_root) / "firmware" / INCLUDED_FILENAME
    return Path(__file__).resolve().parents[1] / "release" / INCLUDED_FILENAME


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as firmware:
        for chunk in iter(lambda: firmware.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_firmware(
    path: Path,
    expected_size: int = FLASH_SIZE,
    expected_sha256: str = INCLUDED_SHA256,
) -> None:
    if not path.is_file():
        raise FileNotFoundError(f"Embedded firmware is missing: {path}")
    actual_size = path.stat().st_size
    if actual_size != expected_size:
        raise ValueError(
            f"Firmware size is invalid: expected {expected_size} bytes, found {actual_size}."
        )
    actual_hash = sha256_file(path)
    if actual_hash.lower() != expected_sha256.lower():
        raise ValueError(
            "Firmware integrity check failed. Re-download or rebuild the flasher application."
        )


def _image_chip_id(header: bytes) -> int | None:
    if len(header) < 14 or header[0] != ESP_IMAGE_MAGIC:
        return None
    return int.from_bytes(header[12:14], "little")


def inspect_firmware(path: Path, included: bool = False) -> FirmwareImage:
    """Validate a C5 firmware image and safely determine its flash offset."""
    path = path.expanduser().resolve()
    if not path.is_file():
        raise FileNotFoundError(f"Firmware file was not found: {path}")
    if path.suffix.lower() != ".bin":
        raise ValueError("Select a firmware file with a .bin extension.")

    size = path.stat().st_size
    if size < 32:
        raise ValueError("The selected firmware file is empty or too small to be valid.")
    if size > FLASH_SIZE:
        raise ValueError("The selected firmware is larger than the device's 8 MB flash.")

    digest = sha256_file(path)
    if included:
        validate_firmware(path)

    with path.open("rb") as firmware:
        prefix = firmware.read(min(size, 0x8010))

    is_full_image = (
        len(prefix) >= 0x8002
        and prefix[0x2000] == ESP_IMAGE_MAGIC
        and prefix[0x8000:0x8002] == PARTITION_MAGIC
    )
    if is_full_image:
        chip_id = _image_chip_id(prefix[0x2000:0x2020])
        if chip_id != ESP32_C5_IMAGE_ID:
            raise ValueError("The selected full-device image is not built for an ESP32-C5.")
        return FirmwareImage(path, "Full-device image", 0x0, size, digest, included)

    chip_id = _image_chip_id(prefix[:32])
    if chip_id is None:
        raise ValueError(
            "The selected file is not a recognized ESP32-C5 full-device or application image."
        )
    if chip_id != ESP32_C5_IMAGE_ID:
        raise ValueError("The selected application image is not built for an ESP32-C5.")
    component_name = path.name.lower()
    if any(name in component_name for name in ("bootloader", "partition", "boot_app0")):
        raise ValueError(
            "Select a complete full-device image or an application image, not an individual boot component."
        )
    if size > APPLICATION_LIMIT:
        raise ValueError(
            f"The application image is too large for the {APPLICATION_LIMIT}-byte application partition."
        )
    return FirmwareImage(path, "Application image", APPLICATION_OFFSET, size, digest, included)


def strip_ansi(value: str) -> str:
    return ANSI_ESCAPE_RE.sub("", value).replace("\x08", "")


def extract_percent(value: str) -> float | None:
    matches = PERCENT_RE.findall(strip_ansi(value))
    if not matches:
        return None
    return min(100.0, max(0.0, float(matches[-1])))


def friendly_failure(details: str) -> str:
    text = strip_ansi(details).lower()
    if "permission denied" in text or "access is denied" in text:
        return (
            "The serial port could not be opened because access was denied. Close serial "
            "monitors and check your serial-port permissions."
        )
    if any(
        phrase in text
        for phrase in (
            "could not open port",
            "no such file or directory",
            "the system cannot find the file",
            "device disconnected",
        )
    ):
        return "The selected serial device is unavailable or was disconnected. Reconnect it and refresh the port list."
    if any(
        phrase in text
        for phrase in (
            "failed to connect",
            "no serial data received",
            "wrong boot mode",
            "timed out waiting for packet header",
            "invalid head of packet",
        )
    ):
        return (
            "The ESP32-C5 did not enter download mode. Reconnect it and try again; if the "
            "board exposes BOOT and RESET, hold BOOT while pressing RESET, then retry."
        )
    if any(
        phrase in text
        for phrase in (
            "wrong chip",
            "unexpected chip",
            "not an esp32-c5",
            "not esp32-c5",
            "not esp32c5",
            "esp32c5 expected",
        )
    ):
        return "The connected device is not an ESP32-C5 Marauder Mini V3. Nothing was written."
    if any(
        phrase in text
        for phrase in ("hash of data does not match", "verify failed", "checksum failed")
    ):
        return (
            "Flash verification failed. Try a shorter USB data cable and a direct USB port, "
            "then flash again."
        )
    if "no module named" in text and "esptool" in text:
        return "The flasher package is incomplete because its esptool component is missing."
    if "firmware integrity check failed" in text or "firmware size is invalid" in text:
        return "The selected firmware image is damaged or incomplete. Choose a valid image and retry."
    if "not built for an esp32-c5" in text or "not a recognized esp32-c5" in text:
        return "The selected file is not compatible with the ESP32-C5. Choose a valid C5 firmware image."
    if "serial" in text and ("timeout" in text or "read failed" in text):
        return "Communication with the device was interrupted. Check USB power and the data cable, then retry."
    return "Flashing did not complete. Review the technical details below for the reported error."


def port_preference(entry: PortEntry) -> tuple[int, str]:
    searchable = f"{entry.description} {entry.hwid}".lower()
    score = 0
    if entry.vid == 0x10C4 or "cp210" in searchable or "silicon labs" in searchable:
        score += 100
    if "esp32" in searchable or "espressif" in searchable:
        score += 80
    if entry.vid is not None:
        score += 10
    return (-score, entry.device.lower())


def discover_ports() -> list[PortEntry]:
    if list_ports is None:
        return []
    entries = [
        PortEntry(
            device=port.device,
            description=port.description or "Serial device",
            hwid=port.hwid or "",
            vid=port.vid,
            pid=port.pid,
        )
        for port in list_ports.comports()
    ]
    return sorted(entries, key=port_preference)


class OutputCapture:
    """File-like stream that emits complete CR/LF-delimited esptool updates."""

    def __init__(self, callback: Callable[[str], None]) -> None:
        self.callback = callback
        self.buffer = ""
        self.encoding = "utf-8"

    def write(self, value: str) -> int:
        if not value:
            return 0
        self.buffer += str(value)
        while True:
            cr = self.buffer.find("\r")
            lf = self.buffer.find("\n")
            positions = [position for position in (cr, lf) if position >= 0]
            if not positions:
                break
            split_at = min(positions)
            line = self.buffer[:split_at]
            self.buffer = self.buffer[split_at + 1 :]
            if line.strip():
                self.callback(line)
        return len(value)

    def flush(self) -> None:
        if self.buffer.strip():
            self.callback(self.buffer)
        self.buffer = ""

    def isatty(self) -> bool:
        # esptool emits live percentage updates when output behaves like a terminal.
        return True


def invoke_esptool(arguments: list[str], output: Callable[[str], None]) -> None:
    try:
        import esptool
    except ImportError as error:
        raise RuntimeError("No module named esptool; install the flasher dependencies.") from error

    capture = OutputCapture(output)
    try:
        with contextlib.redirect_stdout(capture), contextlib.redirect_stderr(capture):
            esptool.main(arguments)
    except SystemExit as error:
        code = error.code if isinstance(error.code, int) else 1
        if code != 0:
            raise RuntimeError(f"esptool exited with status {code}.") from error
    finally:
        capture.flush()


class FlasherApp:
    BACKGROUND = "#0b0f14"
    PANEL = "#141b22"
    PANEL_ALT = "#1b242d"
    TEXT = "#f4f7fa"
    MUTED = "#94a3b8"
    ACCENT = "#7565e8"
    ACCENT_ACTIVE = "#897bf2"
    SUCCESS = "#35c78b"
    ERROR = "#ef6461"

    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.events: queue.Queue[tuple[str, object]] = queue.Queue()
        self.ports: list[PortEntry] = []
        self.port_by_label: dict[str, PortEntry] = {}
        self.flashing = False
        self.raw_output: list[str] = []
        self.firmware = inspect_firmware(resource_path(), included=True)

        self.port_var = tk.StringVar()
        self.firmware_path_var = tk.StringVar(value="Included Marauder Eternal 1.14.3")
        self.firmware_detail_var = tk.StringVar(value=self.firmware.detail)
        self.firmware_warning_var = tk.StringVar()
        self.status_var = tk.StringVar(value="Connect a Marauder Mini V3 to begin")
        self.percent_var = tk.StringVar(value="0%")
        self.progress_var = tk.DoubleVar(value=0.0)
        self._update_firmware_warning()

        self._configure_window()
        self._build_styles()
        self._build_ui()
        self._ensure_activity_lines()
        self.refresh_ports()
        self.root.after(100, self._drain_events)
        self.root.after(1800, self._automatic_refresh)
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

    def _configure_window(self) -> None:
        self.root.title(APP_NAME)
        self.root.geometry(f"{DEFAULT_WINDOW_WIDTH}x{DEFAULT_WINDOW_HEIGHT}")
        self.root.minsize(680, 680)
        self.root.configure(background=self.BACKGROUND)

    def _build_styles(self) -> None:
        style = ttk.Style(self.root)
        if "clam" in style.theme_names():
            style.theme_use("clam")
        style.configure("App.TFrame", background=self.BACKGROUND)
        style.configure("Panel.TFrame", background=self.PANEL)
        style.configure(
            "Port.TCombobox",
            fieldbackground=self.PANEL_ALT,
            background=self.PANEL_ALT,
            foreground=self.TEXT,
            arrowcolor=self.TEXT,
            bordercolor="#334155",
            lightcolor="#334155",
            darkcolor="#334155",
            padding=8,
        )
        style.map(
            "Port.TCombobox",
            fieldbackground=[("readonly", self.PANEL_ALT)],
            foreground=[("readonly", self.TEXT)],
            selectbackground=[("readonly", self.PANEL_ALT)],
            selectforeground=[("readonly", self.TEXT)],
        )
        style.configure(
            "Flash.Horizontal.TProgressbar",
            troughcolor="#26313d",
            background=self.ACCENT,
            bordercolor="#26313d",
            lightcolor=self.ACCENT,
            darkcolor=self.ACCENT,
            thickness=18,
        )
        style.configure(
            "Success.Horizontal.TProgressbar",
            troughcolor="#26313d",
            background=self.SUCCESS,
            bordercolor="#26313d",
            lightcolor=self.SUCCESS,
            darkcolor=self.SUCCESS,
            thickness=18,
        )
        style.configure(
            "Error.Horizontal.TProgressbar",
            troughcolor="#26313d",
            background=self.ERROR,
            bordercolor="#26313d",
            lightcolor=self.ERROR,
            darkcolor=self.ERROR,
            thickness=18,
        )

    def _label(
        self,
        parent: tk.Misc,
        text: str | None = None,
        size: int = 11,
        color: str | None = None,
        weight: str = "normal",
        textvariable: tk.StringVar | None = None,
        **kwargs: object,
    ) -> tk.Label:
        try:
            parent_style = str(parent.cget("style"))
        except tk.TclError:
            parent_style = ""
        default_background = self.PANEL if "Panel" in parent_style else self.BACKGROUND
        return tk.Label(
            parent,
            text=text,
            textvariable=textvariable,
            background=kwargs.pop("background", default_background),
            foreground=color or self.TEXT,
            font=("TkDefaultFont", size, weight),
            **kwargs,
        )

    def _build_ui(self) -> None:
        outer = ttk.Frame(self.root, style="App.TFrame", padding=(28, 24))
        outer.pack(fill="both", expand=True)

        self._label(outer, "ESP32 MARAUDER ETERNAL", 11, self.ACCENT, "bold").pack(anchor="w")
        self._label(outer, "Flasher", 25, self.TEXT, "bold").pack(anchor="w", pady=(2, 2))
        self._label(
            outer,
            f"Mini V3 • ESP32-C5 • Firmware {FIRMWARE_VERSION}",
            10,
            self.MUTED,
        ).pack(anchor="w", pady=(0, 18))

        panel = ttk.Frame(outer, style="Panel.TFrame", padding=18)
        panel.pack(fill="x")
        self._label(panel, "Device", 10, self.MUTED, "bold").pack(anchor="w")

        port_row = ttk.Frame(panel, style="Panel.TFrame")
        port_row.pack(fill="x", pady=(7, 14))
        self.port_combo = ttk.Combobox(
            port_row,
            textvariable=self.port_var,
            state="readonly",
            style="Port.TCombobox",
            font=("TkDefaultFont", 10),
        )
        self.port_combo.pack(side="left", fill="x", expand=True)
        self.refresh_button = tk.Button(
            port_row,
            text="Refresh",
            command=self.refresh_ports,
            background=self.PANEL_ALT,
            foreground=self.TEXT,
            activebackground="#2a3744",
            activeforeground=self.TEXT,
            relief="flat",
            padx=14,
            pady=8,
            cursor="hand2",
        )
        self.refresh_button.pack(side="left", padx=(9, 0))

        self._label(panel, "Firmware", 10, self.MUTED, "bold").pack(anchor="w")
        firmware_row = ttk.Frame(panel, style="Panel.TFrame")
        firmware_row.pack(fill="x", pady=(7, 5))
        self.firmware_entry = tk.Entry(
            firmware_row,
            textvariable=self.firmware_path_var,
            state="readonly",
            readonlybackground=self.PANEL_ALT,
            foreground=self.TEXT,
            relief="flat",
            font=("TkDefaultFont", 10),
        )
        self.firmware_entry.pack(side="left", fill="x", expand=True, ipady=8)
        self.browse_button = tk.Button(
            firmware_row,
            text="Choose BIN",
            command=self.choose_firmware,
            background=self.PANEL_ALT,
            foreground=self.TEXT,
            activebackground="#2a3744",
            activeforeground=self.TEXT,
            relief="flat",
            padx=12,
            pady=8,
            cursor="hand2",
        )
        self.browse_button.pack(side="left", padx=(9, 0))
        self.included_button = tk.Button(
            firmware_row,
            text="Use Included",
            command=self.use_included_firmware,
            background=self.PANEL_ALT,
            foreground=self.TEXT,
            activebackground="#2a3744",
            activeforeground=self.TEXT,
            relief="flat",
            padx=12,
            pady=8,
            cursor="hand2",
        )
        self.included_button.pack(side="left", padx=(7, 0))
        self._label(
            panel,
            textvariable=self.firmware_detail_var,
            size=9,
            color=self.MUTED,
        ).pack(anchor="w", pady=(0, 13))

        self.flash_button = tk.Button(
            panel,
            text="Connect & Flash",
            command=self.start_flash,
            background=self.ACCENT,
            foreground="white",
            activebackground=self.ACCENT_ACTIVE,
            activeforeground="white",
            disabledforeground="#7c8491",
            relief="flat",
            font=("TkDefaultFont", 12, "bold"),
            padx=18,
            pady=12,
            cursor="hand2",
        )
        self.flash_button.pack(fill="x")
        self._label(
            panel,
            textvariable=self.firmware_warning_var,
            size=9,
            color=self.MUTED,
        ).pack(anchor="w", pady=(9, 0))

        progress_header = ttk.Frame(outer, style="App.TFrame")
        progress_header.pack(fill="x", pady=(22, 7))
        self.status_label = self._label(
            progress_header,
            textvariable=self.status_var,
            size=10,
            color=self.MUTED,
            weight="bold",
        )
        self.status_label.pack(side="left", anchor="w")
        self._label(
            progress_header,
            textvariable=self.percent_var,
            size=10,
            color=self.TEXT,
            weight="bold",
        ).pack(side="right")

        self.progress = ttk.Progressbar(
            outer,
            variable=self.progress_var,
            maximum=100,
            style="Flash.Horizontal.TProgressbar",
        )
        self.progress.pack(fill="x")

        self._label(outer, "Activity", 10, self.MUTED, "bold").pack(anchor="w", pady=(20, 7))
        log_frame = tk.Frame(outer, background=self.PANEL, highlightthickness=1, highlightbackground="#283541")
        log_frame.pack(fill="both", expand=True)
        self.log = tk.Text(
            log_frame,
            width=1,
            height=10,
            wrap="word",
            state="disabled",
            background=self.PANEL,
            foreground="#c8d2dc",
            insertbackground=self.TEXT,
            selectbackground=self.ACCENT,
            relief="flat",
            padx=12,
            pady=10,
            font=("TkFixedFont", 9),
        )
        scrollbar = ttk.Scrollbar(log_frame, orient="vertical", command=self.log.yview)
        self.log.configure(yscrollcommand=scrollbar.set)
        self.log.pack(side="left", fill="both", expand=True)
        scrollbar.pack(side="right", fill="y")

        self._label(
            outer,
            f"Flasher {APP_VERSION}  •  Included image plus selectable .bin support",
            8,
            "#657384",
        ).pack(anchor="e", pady=(10, 0))

    def _ensure_activity_lines(self) -> None:
        """Size the initial window so at least seven activity rows are visible."""
        self.root.update_idletasks()
        line_height = tkfont.Font(font=self.log.cget("font")).metrics("linespace")
        log_padding = int(self.log.cget("pady")) * 2
        required_log_height = line_height * MIN_ACTIVITY_LINES + log_padding + 4
        non_log_height = self.root.winfo_reqheight() - self.log.winfo_reqheight()
        desired_height = max(DEFAULT_WINDOW_HEIGHT, non_log_height + required_log_height)
        desired_height = min(desired_height, self.root.winfo_screenheight() - 80)
        desired_width = max(DEFAULT_WINDOW_WIDTH, self.root.winfo_reqwidth())
        desired_width = min(desired_width, self.root.winfo_screenwidth() - 80)
        self.root.geometry(f"{desired_width}x{desired_height}")

    def visible_activity_lines(self) -> int:
        line_height = tkfont.Font(font=self.log.cget("font")).metrics("linespace")
        log_padding = int(self.log.cget("pady")) * 2
        return max(0, (self.log.winfo_height() - log_padding) // line_height)

    def _update_firmware_warning(self) -> None:
        if self.firmware.offset == 0:
            message = "Full-device images replace boot data, partitions, the application, and saved settings."
        else:
            message = "Application-only flashing preserves other regions and requires a compatible partition table."
        self.firmware_warning_var.set(message)

    def _select_firmware(self, firmware: FirmwareImage, display_path: str) -> None:
        self.firmware = firmware
        self.firmware_path_var.set(display_path)
        self.firmware_detail_var.set(firmware.detail)
        self._update_firmware_warning()
        self.raw_output.clear()
        if self.ports:
            self.status_var.set("Ready to connect and flash")

    def choose_firmware(self) -> None:
        if self.flashing:
            return
        selected = filedialog.askopenfilename(
            parent=self.root,
            title="Choose firmware BIN",
            initialdir=str(self.firmware.path.parent),
            filetypes=(("Firmware BIN files", "*.bin"), ("All files", "*.*")),
        )
        if not selected:
            return
        try:
            firmware = inspect_firmware(Path(selected))
        except Exception as error:
            messagebox.showerror(APP_NAME, str(error))
            return
        self._select_firmware(firmware, str(firmware.path))

    def use_included_firmware(self) -> None:
        if self.flashing:
            return
        try:
            firmware = inspect_firmware(resource_path(), included=True)
        except Exception as error:
            messagebox.showerror(APP_NAME, str(error))
            return
        self._select_firmware(firmware, "Included Marauder Eternal 1.14.3")

    def refresh_ports(self) -> None:
        if self.flashing:
            return
        selected_device = self.selected_port()
        self.ports = discover_ports()
        self.port_by_label = {entry.label: entry for entry in self.ports}
        values = list(self.port_by_label)
        self.port_combo.configure(values=values)

        selected = next((entry for entry in self.ports if entry.device == selected_device), None)
        if selected:
            self.port_var.set(selected.label)
        elif self.ports:
            self.port_var.set(self.ports[0].label)
        else:
            self.port_var.set("")

        self.flash_button.configure(state="normal" if self.ports else "disabled")
        if not self.ports:
            self.status_var.set("No serial device detected — connect the device and refresh")
        elif not self.raw_output:
            self.status_var.set("Ready to connect and flash")

    def selected_port(self) -> str | None:
        entry = self.port_by_label.get(self.port_var.get())
        return entry.device if entry else None

    def _automatic_refresh(self) -> None:
        if not self.flashing:
            self.refresh_ports()
        self.root.after(1800, self._automatic_refresh)

    def _append_log(self, message: str) -> None:
        clean = strip_ansi(message).strip()
        if not clean:
            return
        self.log.configure(state="normal")
        self.log.insert("end", clean + "\n")
        self.log.see("end")
        self.log.configure(state="disabled")

    def _set_progress(self, value: float) -> None:
        bounded = min(100.0, max(0.0, value))
        self.progress_var.set(bounded)
        self.percent_var.set(f"{int(round(bounded))}%")

    def _set_flashing_controls(self, active: bool) -> None:
        self.flashing = active
        state = "disabled" if active else "normal"
        self.port_combo.configure(state="disabled" if active else "readonly")
        self.firmware_entry.configure(state="disabled" if active else "readonly")
        self.refresh_button.configure(state=state)
        self.browse_button.configure(state=state)
        self.included_button.configure(state=state)
        self.flash_button.configure(state=state if self.ports else "disabled")

    def start_flash(self) -> None:
        if self.flashing:
            return
        port = self.selected_port()
        if not port:
            messagebox.showerror(APP_NAME, "Select a serial device before flashing.")
            return
        try:
            firmware = inspect_firmware(self.firmware.path, included=self.firmware.included)
        except Exception as error:
            messagebox.showerror(APP_NAME, str(error))
            return
        self.firmware = firmware
        self.firmware_detail_var.set(firmware.detail)
        self._update_firmware_warning()

        self.raw_output.clear()
        self.log.configure(state="normal")
        self.log.delete("1.0", "end")
        self.log.configure(state="disabled")
        self.progress.configure(style="Flash.Horizontal.TProgressbar")
        self._set_progress(0)
        self.status_label.configure(foreground=self.MUTED)
        self.status_var.set("Checking selected firmware…")
        self.flash_button.configure(text="Flashing…")
        self._set_flashing_controls(True)

        thread = threading.Thread(target=self._flash_worker, args=(port, firmware), daemon=True)
        thread.start()

    def _flash_worker(self, port: str, firmware: FirmwareImage) -> None:

        def output(line: str) -> None:
            clean = strip_ansi(line).strip()
            if not clean:
                return
            self.raw_output.append(clean)
            self.events.put(("log", clean))
            raw_percent = extract_percent(clean)
            if raw_percent is not None:
                # Reserve 0–6% for validation/connection and 99–100% for verification/reset.
                self.events.put(("progress", 6.0 + raw_percent * 0.93))

        try:
            if sha256_file(firmware.path) != firmware.sha256:
                raise ValueError("The selected firmware file changed after it was checked. Choose it again.")
            self.events.put(("progress", 2.0))
            self.events.put(("status", f"Connecting to {port}…"))
            self.events.put(("log", f"Selected firmware: {firmware.path}"))
            self.events.put(("log", f"Image type: {firmware.image_type}"))
            self.events.put(("log", f"Flash address: {firmware.offset_text}"))
            self.events.put(("log", f"Firmware SHA-256: {firmware.sha256}"))
            self.events.put(("log", f"Opening serial port: {port}"))

            invoke_esptool(
                ["--chip", "esp32c5", "--port", port, "chip-id"],
                output,
            )
            self.events.put(("progress", 6.0))
            self.events.put(("status", "ESP32-C5 connected — writing selected image…"))

            invoke_esptool(
                [
                    "--chip",
                    "esp32c5",
                    "--port",
                    port,
                    "--baud",
                    FLASH_BAUD,
                    "--before",
                    "default-reset",
                    "--after",
                    "hard-reset",
                    "write-flash",
                    firmware.offset_text,
                    str(firmware.path),
                ],
                output,
            )
            self.events.put(("done", (True, "Firmware flashed and verified successfully.")))
        except Exception as error:  # Error details are surfaced in the activity panel.
            details = "\n".join(self.raw_output + [f"{type(error).__name__}: {error}"])
            self.events.put(("log", f"ERROR: {type(error).__name__}: {error}"))
            self.events.put(("done", (False, friendly_failure(details))))

    def _drain_events(self) -> None:
        try:
            while True:
                kind, payload = self.events.get_nowait()
                if kind == "log":
                    self._append_log(str(payload))
                elif kind == "progress":
                    self._set_progress(float(payload))
                elif kind == "status":
                    self.status_var.set(str(payload))
                elif kind == "done":
                    success, message = payload  # type: ignore[misc]
                    self._finish(bool(success), str(message))
        except queue.Empty:
            pass
        self.root.after(100, self._drain_events)

    def _finish(self, success: bool, message: str) -> None:
        self._set_flashing_controls(False)
        self.flash_button.configure(text="Flash Another Device" if success else "Try Again")
        if success:
            self.progress.configure(style="Success.Horizontal.TProgressbar")
            self._set_progress(100)
            self.status_label.configure(foreground=self.SUCCESS)
            self.status_var.set("SUCCESS — device is ready")
            messagebox.showinfo(APP_NAME, message)
        else:
            self.progress.configure(style="Error.Horizontal.TProgressbar")
            self.status_label.configure(foreground=self.ERROR)
            self.status_var.set("FAILED — no successful firmware verification")
            messagebox.showerror(APP_NAME, message)

    def _on_close(self) -> None:
        if self.flashing:
            messagebox.showwarning(
                APP_NAME,
                "Flashing is still in progress. Wait for success or failure before closing the app.",
            )
            return
        self.root.destroy()


def self_test() -> int:
    try:
        firmware = inspect_firmware(resource_path(), included=True)
        import esptool
        import serial
        esptool_output: list[str] = []
        invoke_esptool(["version"], esptool_output.append)
        if not any("5.1.0" in line for line in esptool_output):
            raise RuntimeError("The embedded esptool command did not run correctly.")
        discovered_count = len(discover_ports())
    except Exception as error:
        print(f"SELF-TEST FAILED: {error}", file=sys.stderr)
        return 1
    print(f"{APP_NAME} {APP_VERSION}")
    print(f"Firmware: {firmware.path}")
    print(f"Firmware type: {firmware.image_type} at {firmware.offset_text}")
    print(f"Firmware SHA-256: {firmware.sha256}")
    print(f"esptool: {getattr(esptool, '__version__', 'available')}")
    print(f"pyserial: {getattr(serial, 'VERSION', 'available')}")
    print(f"Serial enumeration: available ({discovered_count} port(s) found)")
    print("SELF-TEST PASSED")
    return 0


def ui_smoke_test() -> int:
    root = tk.Tk()
    app = FlasherApp(root)
    root.update()
    visible_lines = app.visible_activity_lines()
    if visible_lines < MIN_ACTIVITY_LINES:
        root.destroy()
        raise RuntimeError(
            f"Activity log shows only {visible_lines} lines; at least {MIN_ACTIVITY_LINES} are required."
        )
    root.destroy()
    print(f"UI SMOKE TEST PASSED ({visible_lines} activity lines visible)")
    return 0


def main(arguments: Iterable[str] | None = None) -> int:
    args = list(arguments if arguments is not None else sys.argv[1:])
    if args == ["--self-test"]:
        return self_test()
    if args == ["--ui-smoke-test"]:
        return ui_smoke_test()
    if args:
        print(f"Unknown arguments: {' '.join(args)}", file=sys.stderr)
        return 2
    root = tk.Tk()
    FlasherApp(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception:
        traceback.print_exc()
        if os.environ.get("DISPLAY") or sys.platform.startswith("win"):
            try:
                messagebox.showerror(APP_NAME, "The flasher could not start. See the console for details.")
            except Exception:
                pass
        raise
