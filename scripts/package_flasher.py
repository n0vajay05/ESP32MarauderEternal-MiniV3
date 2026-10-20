#!/usr/bin/env python3
"""Create the native executable and metadata consumed by flasher self-updates."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import sys

PROJECT_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT_ROOT))
from flasher.online_update import current_platform, version_tuple
from flasher.self_update import file_hash, verify_flasher


def package_flasher(executable: Path, version: str, platform: str) -> list[Path]:
    executable = executable.resolve()
    version_tuple(version)
    if version.startswith('v'):
        raise ValueError('Flasher package versions must use numeric major.minor.patch.')
    if platform not in ('linux-x86_64', 'windows-x86_64'):
        raise ValueError('Unsupported flasher release platform.')
    verify_flasher(executable, version, platform)
    extension = '.exe' if platform.startswith('windows-') else ''
    package = executable.with_name(f'MarauderEternalFlasher-{version}-{platform}{extension}')
    shutil.copy2(executable, package)
    metadata = package.with_name(f'flasher-{platform}.json')
    metadata.write_text(json.dumps({
        'product': 'Marauder Eternal Flasher', 'schema': 1, 'update_protocol': 1,
        'version': version, 'platform': platform, 'file': package.name,
        'bytes': package.stat().st_size, 'sha256': file_hash(package),
    }, indent=2) + '\n', encoding='utf-8')
    checksums = package.with_name(f'flasher-{platform}.sha256')
    checksums.write_text(''.join(f'{file_hash(path)}  {path.name}\n' for path in (package, metadata)), encoding='ascii')
    executable.with_name('SHA256SUMS').write_text(
        ''.join(f'{file_hash(path)}  {path.name}\n' for path in (executable, package, metadata)), encoding='ascii')
    return [package, metadata, checksums]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('--platform', default=current_platform())
    args = parser.parse_args()
    manifest = json.loads((PROJECT_ROOT / 'release/manifest.json').read_text(encoding='utf-8'))
    for path in package_flasher(args.executable, manifest['flasher_version'], args.platform):
        print(path)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
