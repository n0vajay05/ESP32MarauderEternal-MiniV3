"""Stage a portable flasher update and replace it after the launcher exits."""

from __future__ import annotations

import contextlib
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time


def file_hash(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def validate_executable(path: Path, platform: str) -> None:
    with path.open('rb') as source:
        header = source.read(64)
        if platform == 'linux-x86_64':
            if (len(header) < 64 or header[:7] != b'\x7fELF\x02\x01\x01' or
                    int.from_bytes(header[18:20], 'little') != 62 or
                    int.from_bytes(header[16:18], 'little') not in (2, 3)):
                raise ValueError('The update is not a Linux x86-64 executable.')
        elif platform == 'windows-x86_64':
            if len(header) < 64 or header[:2] != b'MZ':
                raise ValueError('The update is not a Windows executable.')
            offset = int.from_bytes(header[60:64], 'little')
            if not 64 <= offset <= 1024 * 1024:
                raise ValueError('Invalid Windows executable header.')
            source.seek(offset)
            pe = source.read(24)
            if len(pe) != 24 or pe[:6] != b'PE\0\0\x64\x86' or not (int.from_bytes(pe[22:24], 'little') & 2):
                raise ValueError('The update is not a Windows x86-64 application.')
        else:
            raise ValueError('Automatic flasher updates are unavailable for this platform.')


def independent_environment() -> dict[str, str]:
    environment = dict(os.environ)
    environment['PYINSTALLER_RESET_ENVIRONMENT'] = '1'
    if 'LD_LIBRARY_PATH_ORIG' in environment:
        environment['LD_LIBRARY_PATH'] = environment['LD_LIBRARY_PATH_ORIG']
    else:
        environment.pop('LD_LIBRARY_PATH', None)
    return environment


def process_options() -> dict:
    options = {'env': independent_environment(), 'close_fds': True}
    if os.name == 'nt':
        options['creationflags'] = subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.CREATE_NO_WINDOW
    else:
        options['start_new_session'] = True
    return options


def verify_flasher(path: Path, version: str, platform: str) -> None:
    validate_executable(path, platform)
    if os.name != 'nt':
        path.chmod(0o700)
    with tempfile.TemporaryDirectory(prefix='.flasher-health-', dir=path.parent) as directory:
        report = Path(directory) / 'health.json'
        result = subprocess.run([str(path), '--update-health', str(report)],
                                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                timeout=30, **process_options())
        if result.returncode != 0 or not report.is_file() or report.stat().st_size > 4096:
            raise ValueError('The downloaded flasher failed its startup check.')
        health = json.loads(report.read_text(encoding='utf-8'))
        if (health.get('product') != 'Marauder Eternal Flasher' or
                health.get('version') != version or health.get('platform') != platform or
                health.get('update_protocol') != 1 or health.get('status') != 'ok'):
            raise ValueError('The downloaded flasher failed its version/startup check.')


def create_stage(target: Path) -> Path:
    return Path(tempfile.mkdtemp(prefix='.marauder-flasher-update-', dir=target.parent))


def prepare_handoff(stage: Path, target: Path, version: str, digest: str,
                    platform: str, firmware: Path | None = None) -> Path:
    """Copy the current helper so Windows can release the original EXE lock."""
    target = target.resolve()
    source = stage / 'download'
    if file_hash(source) != digest:
        raise ValueError('The downloaded flasher changed before installation.')
    frozen = bool(getattr(sys, 'frozen', False))
    helper = stage / ('update-helper.exe' if os.name == 'nt' else 'update-helper')
    if frozen:
        shutil.copy2(sys.executable, helper)
        command = [str(helper), '--apply-flasher-update']
    else:
        helper = stage / 'update-helper.py'
        shutil.copy2(__file__, helper)
        command = [sys.executable, str(helper)]
    job = {'schema': 1, 'parent_pid': os.getpid(), 'source': str(source.resolve()),
           'target': str(target), 'old_sha256': file_hash(target) if target.exists() else None,
           'sha256': digest, 'version': version, 'platform': platform,
           'firmware': str(firmware.resolve()) if firmware is not None else None}
    job_path = stage / 'job.json'
    job_path.write_text(json.dumps(job), encoding='utf-8')
    job_path.chmod(0o600)
    subprocess.Popen([*command, str(job_path)], stdin=subprocess.DEVNULL,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                     **process_options())
    return job_path


def wait_for_parent(pid: int, timeout: float = 45.0) -> None:
    if type(pid) is not int or pid <= 0 or pid == os.getpid():
        raise ValueError('Invalid updater parent process.')
    if os.name == 'nt':
        import ctypes
        from ctypes import wintypes
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        handle = kernel.OpenProcess(0x00100000, False, pid)  # SYNCHRONIZE
        if not handle:
            if ctypes.get_last_error() == 87:  # Process already exited.
                return
            raise OSError('Could not wait for the launcher to exit.')
        try:
            if kernel.WaitForSingleObject(handle, int(timeout * 1000)) != 0:
                raise TimeoutError('The launcher did not exit before the update timeout.')
        finally:
            kernel.CloseHandle(handle)
        return
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return
        time.sleep(.1)
    raise TimeoutError('The launcher did not exit before the update timeout.')


def _replace_with_retry(source: Path, target: Path) -> None:
    deadline = time.monotonic() + 15
    while True:
        try:
            os.replace(source, target)
            return
        except PermissionError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(.2)  # Allow the old one-file bootloader to finish cleanup.


def apply_update(job_path: Path) -> None:
    job_path = job_path.resolve()
    job = json.loads(job_path.read_text(encoding='utf-8'))
    if job.get('schema') != 1:
        raise ValueError('Invalid flasher update job.')
    source, target = Path(job['source']), Path(job['target'])
    if source.resolve() != job_path.parent / 'download' or target.parent != job_path.parent.parent:
        raise ValueError('Invalid flasher update paths.')
    wait_for_parent(job['parent_pid'])
    if file_hash(source) != job['sha256']:
        raise ValueError('Flasher checksum changed before replacement.')
    validate_executable(source, job['platform'])
    existed = target.exists()
    if existed != (job['old_sha256'] is not None) or (existed and file_hash(target) != job['old_sha256']):
        raise ValueError('The existing launcher changed during the download; it was preserved.')
    backup = target.with_name(target.name + '.previous')
    if existed:
        shutil.copy2(target, backup)
    if os.name != 'nt':
        source.chmod((target.stat().st_mode & 0o777) | 0o700 if existed else 0o700)
    replaced = False
    try:
        _replace_with_retry(source, target)
        replaced = True
        arguments = [str(target)]
        if job.get('firmware'):
            arguments += ['--firmware', job['firmware']]
        subprocess.Popen(arguments, stdin=subprocess.DEVNULL,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         **process_options())
    except Exception:
        if replaced:
            if existed:
                _replace_with_retry(backup, target)
            else:
                target.unlink(missing_ok=True)
        raise
    (job_path.parent / 'completed').write_text('1', encoding='ascii')
    # Linux permits deleting a running helper. Windows cleans it on a later launch.
    if os.name != 'nt':
        with contextlib.suppress(OSError):
            shutil.rmtree(job_path.parent)


def cleanup_completed(target: Path) -> None:
    for directory in target.parent.glob('.marauder-flasher-update-*'):
        try:
            if directory.is_symlink() or not (directory / 'completed').is_file():
                continue
            job = json.loads((directory / 'job.json').read_text(encoding='utf-8'))
            if job.get('target') == str(target.resolve()):
                # Remove the potentially locked helper first, retaining the job
                # metadata for a later launch if Windows still has it open.
                for name in ('update-helper.exe', 'update-helper'):
                    (directory / name).unlink(missing_ok=True)
                shutil.rmtree(directory)
        except (OSError, ValueError):
            pass


def helper_main(job_path: Path) -> int:
    try:
        apply_update(job_path)
        return 0
    except Exception as error:
        with contextlib.suppress(OSError):
            (job_path.parent / 'error.txt').write_text(str(error), encoding='utf-8')
        with contextlib.suppress(Exception):
            import tkinter as tk
            from tkinter import messagebox
            root = tk.Tk(); root.withdraw()
            messagebox.showerror('Marauder Eternal Flasher',
                                 f'Flasher update failed. The previous version was preserved.\n\n{error}', parent=root)
            root.destroy()
        return 1


if __name__ == '__main__':
    raise SystemExit(helper_main(Path(sys.argv[1])))
