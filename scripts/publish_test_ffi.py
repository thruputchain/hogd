#!/usr/bin/env python3
"""Validate and mirror authenticated test-ffi release assets to public S3."""
from __future__ import annotations

import argparse
import re
import struct
import sys
import tarfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from public_artifacts import PublicationError, publish_file, sha256_file

PLATFORMS = ('linux-x86_64', 'linux-arm64', 'macos-arm64')
MAX_ARCHIVE_BYTES = 2 * 1024 * 1024 * 1024


def validate_library_header(header: bytes, platform: str) -> None:
    """Check the target-specific shared-library header without loading code."""
    if platform.startswith('linux-'):
        machine = 62 if platform == 'linux-x86_64' else 183
        if (len(header) < 64 or header[:7] != b'\x7fELF\x02\x01\x01'
                or struct.unpack_from('<HHI', header, 16) != (3, machine, 1)
                or struct.unpack_from('<H', header, 52)[0] != 64):
            raise PublicationError(f'expected a 64-bit little-endian ELF shared library for {platform}')
    else:
        # Thin ARM64 Mach-O, MH_DYLIB. Fat binaries are not this release layout.
        if (len(header) < 32 or header[:4] != b'\xcf\xfa\xed\xfe'
                or struct.unpack_from('<I', header, 4)[0] != 0x0100000C
                or struct.unpack_from('<I', header, 12)[0] != 6):
            raise PublicationError(f'expected a 64-bit ARM64 Mach-O dylib for {platform}')


def prepare_files(assets_dir: Path, tag: str) -> list[dict]:
    """Validate every archive before allowing any public write."""
    if not re.fullmatch(r'test-ffi-v\d+\.\d+\.\d+', tag):
        raise PublicationError(f'invalid release tag: {tag}')
    files = []
    for platform in PLATFORMS:
        name = f'libtest_ffi-{platform}.tar.gz'
        archive = assets_dir / name
        checksum = assets_dir / f'{name}.sha256'
        for path in (archive, checksum):
            if path.is_symlink() or not path.is_file() or not 0 < path.stat().st_size <= MAX_ARCHIVE_BYTES:
                raise PublicationError(f'invalid publication file: {path}')
        if checksum.stat().st_size > 256:
            raise PublicationError(f'oversized checksum: {checksum}')
        digest = sha256_file(archive)
        if checksum.read_text() != f'{digest}  {name}\n':
            raise PublicationError(f'checksum mismatch or invalid sidecar: {name}')
        library = 'libtest_ffi.dylib' if platform.startswith('macos') else 'libtest_ffi.so'
        expected = {library, 'test_bindings.h', 'bindings.rs'}
        with tarfile.open(archive, 'r:gz') as tar:
            members = tar.getmembers()
            if (len(members) != len(expected) or {m.name for m in members} != expected
                    or any(not m.isfile() or not 0 < m.size <= MAX_ARCHIVE_BYTES for m in members)):
                raise PublicationError(f'unexpected archive layout: {name}')
            with tar.extractfile(library) as handle:
                validate_library_header(handle.read(64), platform)
        for path, content_type in ((archive, 'application/gzip'), (checksum, 'text/plain; charset=utf-8')):
            files.append({'path': path, 'key': f'test-ffi/{tag}/{path.name}',
                          'size': path.stat().st_size, 'sha256': sha256_file(path),
                          'content_type': content_type})
    return files


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tag', required=True)
    parser.add_argument('--assets-dir', type=Path, required=True)
    parser.add_argument('--bucket')
    parser.add_argument('--base-url')
    parser.add_argument('--publish', action='store_true')
    args = parser.parse_args()
    try:
        files = prepare_files(args.assets_dir, args.tag)
        if args.publish and (not args.bucket or not args.base_url):
            parser.error('--publish requires --bucket and --base-url')
        for item in files:
            outcome = publish_file(item, bucket=args.bucket, base_url=args.base_url) if args.publish else 'validated'
            print(f'{outcome}: {item["key"]}')
    except (PublicationError, OSError, ValueError, tarfile.TarError) as error:
        print(f'error: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
