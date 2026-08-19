# -*- mode: python ; coding: utf-8 -*-

from pathlib import Path
import sys

from PyInstaller.utils.hooks import collect_data_files, collect_submodules


project_root = Path(SPECPATH).resolve().parent
firmware = project_root / "release" / "Marauder_Eternal_1.14.3_MiniV3_ESP32-C5.bin"
notices = project_root / "flasher" / "THIRD_PARTY_NOTICES.md"

if not firmware.is_file():
    raise SystemExit(f"Firmware is missing: {firmware}")

datas = [
    (str(firmware), "firmware"),
    (str(notices), "."),
]
datas += collect_data_files("esptool")

if sys.platform == "win32":
    serial_backend = "serial.tools.list_ports_windows"
elif sys.platform == "darwin":
    serial_backend = "serial.tools.list_ports_osx"
else:
    serial_backend = "serial.tools.list_ports_linux"

hiddenimports = collect_submodules("esptool") + [serial_backend]

a = Analysis(
    [str(project_root / "flasher" / "marauder_eternal_flasher.py")],
    pathex=[str(project_root)],
    binaries=[],
    datas=datas,
    hiddenimports=hiddenimports,
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
    optimize=1,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name="MarauderEternalFlasher",
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)
