"""Read and download verified firmware releases from the Eternal Mini V3 repo."""

from __future__ import annotations

import contextlib
import hashlib
from http.client import HTTPException
import json
import os
import platform
import re
import ssl
import sys
import tempfile
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable
from urllib.error import URLError
from urllib.parse import urlsplit
from urllib.request import HTTPRedirectHandler, HTTPSHandler, Request, build_opener

REPOSITORY = "n0vajay05/ESP32MarauderEternal-MiniV3"
LATEST_URL = f"https://api.github.com/repos/{REPOSITORY}/releases/latest"
RELEASES_URL = f"https://api.github.com/repos/{REPOSITORY}/releases?per_page=100"
DOWNLOAD_PREFIX = f"https://github.com/{REPOSITORY}/releases/download/"
CDN_PREFIX = "/github-production-release-asset/1339968948/"
SERVER_ERROR = "Failed to connect to update server, please try again later."
SLOT_BYTES = 0x3C0000
MAX_FLASHER_BYTES = 256 * 1024 * 1024


def current_platform() -> str | None:
    if platform.machine().lower() not in ('x86_64', 'amd64'):
        return None
    return {'linux': 'linux-x86_64', 'win32': 'windows-x86_64'}.get(sys.platform)


class OnlineUpdateError(ValueError):
    """A release or downloaded image could not be verified."""


class UpdateConnectionError(OnlineUpdateError):
    """The update service could not be reached or read."""


class UpdateCancelled(OnlineUpdateError):
    """The app is closing before the update completes."""


def version_tuple(value: str) -> tuple[int, int, int]:
    if not isinstance(value, str) or not re.fullmatch(r"v?[0-9]{1,6}\.[0-9]{1,6}\.[0-9]{1,6}", value):
        raise OnlineUpdateError("Invalid firmware version.")
    return tuple(int(part) for part in value.removeprefix("v").split("."))


def allowed_url(url: str, asset_redirect: bool = False) -> bool:
    try:
        parsed = urlsplit(url)
        if (parsed.scheme != "https" or parsed.port not in (None, 443) or
                parsed.username or parsed.password or parsed.fragment or
                any(ord(char) < 33 or ord(char) > 126 for char in url) or
                "\\" in url or "%" in parsed.path):
            return False
        if url in (LATEST_URL, RELEASES_URL):
            return True
        if url.startswith(DOWNLOAD_PREFIX) and not parsed.query:
            parts = url[len(DOWNLOAD_PREFIX):].split("/")
            return len(parts) == 2 and all(
                re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,127}", part)
                for part in parts
            )
        return bool(asset_redirect and parsed.hostname in (
            "release-assets.githubusercontent.com", "objects.githubusercontent.com"
        ) and parsed.path.startswith(CDN_PREFIX) and
            re.fullmatch(r"[A-Za-z0-9._-]+", parsed.path[len(CDN_PREFIX):]))
    except (ValueError, TypeError):
        return False


class RepoRedirectHandler(HTTPRedirectHandler):
    max_redirections = 5
    max_repeats = 2

    def __init__(self, cancel: threading.Event | None = None) -> None:
        self.cancel = cancel
        self.deadline = float('inf')

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if self.cancel is not None and self.cancel.is_set():
            raise UpdateCancelled("Online update cancelled.")
        remaining = self.deadline - time.monotonic()
        if remaining <= 0:
            raise UpdateConnectionError(SERVER_ERROR)
        if hasattr(req, 'timeout'):
            req.timeout = min(req.timeout, remaining)
        if not allowed_url(newurl, asset_redirect=req.full_url not in (LATEST_URL, RELEASES_URL)):
            raise OnlineUpdateError("Update server redirected outside the release repository.")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


@dataclass(frozen=True)
class OnlineFirmware:
    version: str
    filename: str
    url: str
    size: int
    sha256: str


@dataclass(frozen=True)
class OnlineFlasher:
    version: str
    filename: str
    url: str
    size: int
    sha256: str
    platform: str


@dataclass(frozen=True)
class UpdateCheck:
    firmware: OnlineFirmware | None = None
    flasher: OnlineFlasher | None = None
    firmware_error: str | None = None
    flasher_error: str | None = None


class OnlineUpdater:
    def __init__(self, app_version: str, cancel: threading.Event | None = None) -> None:
        self.cancel = cancel if cancel is not None else threading.Event()
        self.user_agent = f"MarauderEternalFlasher/{app_version}"
        context = ssl.create_default_context()
        context.set_alpn_protocols(["http/1.1"])
        self.redirects = RepoRedirectHandler(self.cancel)
        self.opener = build_opener(self.redirects, HTTPSHandler(context=context))

    def _check_cancel(self) -> None:
        if self.cancel.is_set():
            raise UpdateCancelled("Online update cancelled.")

    def _open(self, url: str, deadline: float):
        self._check_cancel()
        if not allowed_url(url):
            raise OnlineUpdateError("Invalid release asset URL.")
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise UpdateConnectionError(SERVER_ERROR)
        request = Request(url, headers={
            "User-Agent": self.user_agent,
            "Accept": "application/vnd.github+json" if url in (LATEST_URL, RELEASES_URL) else "application/octet-stream",
            "X-GitHub-Api-Version": "2022-11-28",
        })
        try:
            self.redirects.deadline = deadline
            response = self.opener.open(request, timeout=min(15.0, remaining))
        except (OSError, URLError, HTTPException) as error:
            raise UpdateConnectionError(SERVER_ERROR) from error
        if response.status != 200:
            response.close()
            raise UpdateConnectionError(SERVER_ERROR)
        return response

    def _chunks(self, response, deadline: float, maximum: int):
        total = 0
        while True:
            self._check_cancel()
            if time.monotonic() >= deadline:
                raise UpdateConnectionError(SERVER_ERROR)
            try:
                chunk = response.read1(min(65536, maximum - total + 1))
            except (OSError, URLError, HTTPException) as error:
                raise UpdateConnectionError(SERVER_ERROR) from error
            if not chunk:
                return
            total += len(chunk)
            if total > maximum:
                raise OnlineUpdateError("Update response is larger than expected.")
            yield chunk

    def _json(self, url: str, deadline: float, maximum: int, digest: str | None = None, *, array: bool = False):
        with self._open(url, deadline) as response:
            raw = b"".join(self._chunks(response, deadline, maximum))
        if digest and digest != "sha256:" + hashlib.sha256(raw).hexdigest():
            raise OnlineUpdateError("Release manifest checksum mismatch.")
        try:
            result = json.loads(raw)
        except (ValueError, UnicodeError) as error:
            raise OnlineUpdateError("Invalid release JSON.") from error
        if not isinstance(result, list if array else dict):
            raise OnlineUpdateError("Invalid release metadata.")
        return result

    @staticmethod
    def _asset(release: dict, name: str) -> dict:
        matches = [asset for asset in release['assets'] if isinstance(asset, dict) and asset.get('name') == name]
        if len(matches) != 1:
            raise OnlineUpdateError("Required release asset is missing or duplicated.")
        asset = matches[0]
        expected = DOWNLOAD_PREFIX + release['tag_name'] + '/' + name
        if asset.get('browser_download_url') != expected or asset.get('state') != 'uploaded':
            raise OnlineUpdateError("Invalid release asset.")
        return asset

    def latest(self) -> OnlineFirmware:
        deadline = time.monotonic() + 30.0
        release = self._json(LATEST_URL, deadline, 1024 * 1024)
        return self._firmware_from(release, deadline)

    @staticmethod
    def _stable(release) -> bool:
        return (isinstance(release, dict) and release.get('draft') is False and
                release.get('prerelease') is False and isinstance(release.get('tag_name'), str) and
                re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,127}", release['tag_name']) is not None and
                isinstance(release.get('assets'), list) and len(release['assets']) <= 128)

    def _firmware_from(self, release: dict, deadline: float) -> OnlineFirmware:
        tag = release.get('tag_name')
        if (release.get('draft') is not False or release.get('prerelease') is not False or
                not isinstance(tag, str) or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,127}", tag) or
                not isinstance(release.get('assets'), list) or len(release['assets']) > 128):
            raise OnlineUpdateError("Invalid stable release.")
        manifest_asset = self._asset(release, 'manifest.json')
        manifest = self._json(manifest_asset['browser_download_url'], deadline, 128 * 1024,
                              manifest_asset.get('digest'))
        version = manifest.get('version')
        version_tuple(version)
        if version.startswith('v'):
            raise OnlineUpdateError("Invalid manifest version.")
        required = {'product': 'ESP32 Marauder Eternal', 'hardware': 'Marauder Mini V3',
                    'chip': 'ESP32-C5', 'flash_size': '8MB',
                    'partition_layout': 'mini-v3-c5-8m-ota-v1',
                    'application_flash_offsets': ['0x10000', '0x400000']}
        if any(manifest.get(key) != value for key, value in required.items()) or manifest.get('psram') is not True:
            raise OnlineUpdateError("Release targets an incompatible device or partition layout.")
        filename = f'Marauder_Eternal_{version}_MiniV3_ESP32-C5.app.bin'
        asset = self._asset(release, filename)
        images = manifest.get('images')
        if not isinstance(images, list):
            raise OnlineUpdateError("Missing release images.")
        records = [record for record in images if isinstance(record, dict) and record.get('file') == filename]
        if len(records) != 1:
            raise OnlineUpdateError("Missing or duplicated application image.")
        image = records[0]
        size, digest = image.get('bytes'), image.get('sha256')
        if (type(size) is not int or not 1024 <= size <= SLOT_BYTES or
                type(asset.get('size')) is not int or size != asset['size'] or
                image.get('offset') != '0x10000' or not isinstance(digest, str) or
                not re.fullmatch(r'[0-9a-f]{64}', digest) or
                asset.get('digest') not in (None, '', 'sha256:' + digest)):
            raise OnlineUpdateError("Invalid application size, checksum or flash offset.")
        return OnlineFirmware(version, filename, asset['browser_download_url'], size, digest)

    def check(self, current_version: str) -> OnlineFirmware | None:
        candidate = self.latest()
        return candidate if version_tuple(candidate.version) > version_tuple(current_version) else None

    def _flasher_from(self, release: dict, deadline: float, target: str, expected_version: str) -> OnlineFlasher:
        metadata_asset = self._asset(release, f'flasher-{target}.json')
        metadata = self._json(metadata_asset['browser_download_url'], deadline, 16384, metadata_asset.get('digest'))
        version = metadata.get('version')
        version_tuple(version)
        extension = '.exe' if target.startswith('windows-') else ''
        filename = f'MarauderEternalFlasher-{version}-{target}{extension}'
        asset = self._asset(release, filename)
        size, digest = metadata.get('bytes'), metadata.get('sha256')
        if (metadata.get('product') != 'Marauder Eternal Flasher' or
                type(metadata.get('schema')) is not int or metadata['schema'] != 1 or
                type(metadata.get('update_protocol')) is not int or metadata['update_protocol'] != 1 or
                metadata.get('platform') != target or metadata.get('file') != filename or
                version != expected_version or version.startswith('v') or
                type(size) is not int or not 1024 <= size <= MAX_FLASHER_BYTES or
                type(asset.get('size')) is not int or asset['size'] != size or
                not isinstance(digest, str) or not re.fullmatch(r'[0-9a-f]{64}', digest) or
                asset.get('digest') not in (None, '', 'sha256:' + digest)):
            raise OnlineUpdateError('Invalid flasher release metadata.')
        return OnlineFlasher(version, filename, asset['browser_download_url'], size, digest, target)

    def check_all(self, firmware_version: str | None, flasher_version: str) -> UpdateCheck:
        """Check independent firmware/flasher releases with one release-list request."""
        deadline = time.monotonic() + 45.0
        releases = self._json(RELEASES_URL, deadline, 5 * 1024 * 1024, array=True)
        if len(releases) > 100:
            raise OnlineUpdateError('Invalid release list.')
        target = current_platform()
        firmware_candidates, flasher_candidates = [], []
        app_pattern = re.compile(r'Marauder_Eternal_([0-9]{1,6}\.[0-9]{1,6}\.[0-9]{1,6})_MiniV3_ESP32-C5\.app\.bin')
        extension = r'\.exe' if target and target.startswith('windows-') else ''
        desktop_pattern = re.compile(r'MarauderEternalFlasher-([0-9]{1,6}\.[0-9]{1,6}\.[0-9]{1,6})-' + re.escape(target or '') + extension)
        for release in releases:
            if not self._stable(release):
                continue
            for asset in release['assets']:
                name = asset.get('name', '') if isinstance(asset, dict) else ''
                if not isinstance(name, str):
                    continue
                firmware_match = app_pattern.fullmatch(name)
                flasher_match = desktop_pattern.fullmatch(name) if target else None
                if firmware_match:
                    firmware_candidates.append((version_tuple(firmware_match[1]), release))
                if flasher_match:
                    flasher_candidates.append((version_tuple(flasher_match[1]), flasher_match[1], release))
        firmware = flasher = None
        firmware_error = flasher_error = None
        if firmware_candidates:
            try:
                _, release = max(firmware_candidates, key=lambda candidate: candidate[0])
                latest = self._firmware_from(release, deadline)
                if firmware_version is None or version_tuple(latest.version) > version_tuple(firmware_version):
                    firmware = latest
            except UpdateCancelled:
                raise
            except Exception:
                firmware_error = SERVER_ERROR
        else:
            firmware_error = 'No compatible firmware is available in the update repository.'
        if flasher_candidates:
            try:
                _, latest_version, release = max(flasher_candidates, key=lambda candidate: candidate[0])
                if version_tuple(latest_version) > version_tuple(flasher_version):
                    flasher = self._flasher_from(release, deadline, target, latest_version)
            except UpdateCancelled:
                raise
            except UpdateConnectionError:
                flasher_error = SERVER_ERROR
            except Exception:
                flasher_error = 'Failed to check the flasher update package, please try again later.'
        return UpdateCheck(firmware, flasher, firmware_error, flasher_error)

    def download(self, candidate: OnlineFirmware | OnlineFlasher, destination: Path,
                 validate_image: Callable[[Path, str], None],
                 progress: Callable[[float], None] = lambda _percent: None) -> None:
        destination = destination.expanduser().resolve()
        if isinstance(candidate, OnlineFirmware) and destination.suffix.lower() != '.bin':
            raise OnlineUpdateError("Choose a destination with a .bin extension.")
        deadline = time.monotonic() + (600.0 if isinstance(candidate, OnlineFlasher) else 180.0)
        temporary = None
        try:
            suffix = '.exe' if isinstance(candidate, OnlineFlasher) and candidate.platform.startswith('windows-') else '.bin'
            with tempfile.NamedTemporaryFile(prefix='.marauder-update-', suffix=suffix,
                                             dir=destination.parent, delete=False) as output:
                temporary = Path(output.name)
                digest = hashlib.sha256()
                received = 0
                with self._open(candidate.url, deadline) as response:
                    for chunk in self._chunks(response, deadline, candidate.size):
                        output.write(chunk)
                        digest.update(chunk)
                        received += len(chunk)
                        progress(received * 100.0 / candidate.size)
                if received != candidate.size or digest.hexdigest() != candidate.sha256:
                    raise OnlineUpdateError("Firmware download failed its size or SHA-256 check.")
                output.flush()
                os.fsync(output.fileno())
            validate_image(temporary, candidate.version)
            self._check_cancel()
            os.replace(temporary, destination)
        finally:
            if temporary is not None:
                with contextlib.suppress(OSError):
                    temporary.unlink(missing_ok=True)
