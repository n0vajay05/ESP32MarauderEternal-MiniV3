# -*- mode: python ; coding: utf-8 -*-

from pathlib import Path
import json
import sys

from PyInstaller.utils.hooks import collect_data_files, collect_submodules


project_root = Path(SPECPATH).resolve().parent
manifest = project_root / "release" / "manifest.json"
try:
    release = json.loads(manifest.read_text(encoding="utf-8"))
    filename = release["full_device_image"]["file"]
    firmware = project_root / "release" / filename if Path(filename).name == filename else None
except (OSError, ValueError, KeyError, TypeError):
    firmware = None
notices = project_root / "flasher" / "THIRD_PARTY_NOTICES.md"

datas = [
    (str(notices), "."),
]
if firmware is not None and firmware.is_file():
    datas.append((str(firmware), "firmware"))
if manifest.is_file():
    datas.append((str(manifest), "firmware"))
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
