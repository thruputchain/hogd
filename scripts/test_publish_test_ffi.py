from __future__ import annotations

import gzip
import hashlib
import importlib.util
import io
import subprocess
import struct
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('publish_test_ffi', ROOT / 'scripts/publish_test_ffi.py')
script = importlib.util.module_from_spec(spec)
spec.loader.exec_module(script)


def library_header(platform):
    if platform.startswith('linux-'):
        # ELF64 ET_DYN with the native platform's machine value.
        header = bytearray(64)
        header[:7] = b'\x7fELF\x02\x01\x01'
        struct.pack_into('<HHI', header, 16, 3, 62 if platform == 'linux-x86_64' else 183, 1)
        struct.pack_into('<H', header, 52, 64)
        return bytes(header)
    return struct.pack('<8I', 0xFEEDFACF, 0x0100000C, 0, 6, 0, 0, 0, 0)


def make_assets(root, replacement=None):
    for platform in script.PLATFORMS:
        name = f'libtest_ffi-{platform}.tar.gz'
        library = 'libtest_ffi.dylib' if platform.startswith('macos') else 'libtest_ffi.so'
        with tarfile.open(root / name, 'w:gz') as tar:
            for member in (library, 'test_bindings.h', 'bindings.rs'):
                info = tarfile.TarInfo(member)
                payload = library_header(platform) if member == library else b'test'
                if member == library and replacement is not None and replacement[0] == platform:
                    payload = replacement[1]
                info.size = len(payload)
                tar.addfile(info, io.BytesIO(payload))
        digest = hashlib.sha256((root / name).read_bytes()).hexdigest()
        (root / f'{name}.sha256').write_text(f'{digest}  {name}\n')


class PublicationTests(unittest.TestCase):
    def test_exact_six_objects_and_upstream_bytes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            make_assets(root)
            files = script.prepare_files(root, 'test-ffi-v1.2.3')
            self.assertEqual(len(files), 6)
            for item in files:
                self.assertEqual(item['key'], f'test-ffi/test-ffi-v1.2.3/{item["path"].name}')
                self.assertEqual(item['sha256'], hashlib.sha256(item['path'].read_bytes()).hexdigest())

    def replace_archive(self, root, raw):
        name = 'libtest_ffi-linux-x86_64.tar.gz'
        path = root / name
        path.write_bytes(gzip.compress(raw))
        (root / f'{name}.sha256').write_text(f'{hashlib.sha256(path.read_bytes()).hexdigest()}  {name}\n')

    def test_oversized_members_rejected_from_header_without_reading_payload(self):
        for member, limit in [('libtest_ffi.so', script.MAX_LIBRARY_BYTES),
                              ('bindings.rs', script.MAX_BINDING_BYTES)]:
            with self.subTest(member=member), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                make_assets(root)
                info = tarfile.TarInfo(member)
                info.size = limit + 1
                # No payload: a parser advancing beyond this header fails with
                # a truncated-archive error instead of the required early limit.
                self.replace_archive(root, info.tobuf())
                with self.assertRaisesRegex(script.PublicationError, 'member exceeds size limit'):
                    script.prepare_files(root, 'test-ffi-v1.2.3')

    def test_metadata_expansion_is_bounded_before_tar_yields_a_member(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            make_assets(root)
            info = tarfile.TarInfo('././@LongLink')
            info.type = tarfile.GNUTYPE_LONGNAME
            info.size = 2 * 1024 * 1024
            self.replace_archive(root, info.tobuf() + b'x' * info.size)
            consumed = []
            original_read = gzip.GzipFile.read
            def counted_read(handle, size=-1):
                data = original_read(handle, size)
                consumed.append(len(data))
                return data
            with patch.object(script, 'MAX_UNCOMPRESSED_BYTES', 64 * 1024), \
                    patch.object(gzip.GzipFile, 'read', counted_read):
                with self.assertRaisesRegex(script.PublicationError, 'total uncompressed-size'):
                    script.prepare_files(root, 'test-ffi-v1.2.3')
            self.assertLessEqual(sum(consumed), 64 * 1024 + 1)

    def test_total_limit_applies_across_individually_valid_members(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            make_assets(root)
            raw = io.BytesIO()
            with tarfile.open(fileobj=raw, mode='w') as archive:
                for name in ('libtest_ffi.so', 'test_bindings.h', 'bindings.rs'):
                    info = tarfile.TarInfo(name)
                    payload = library_header('linux-x86_64') if name.endswith('.so') else b''
                    payload = payload.ljust(32 * 1024, b'\0')
                    info.size = len(payload)
                    archive.addfile(info, io.BytesIO(payload))
            self.replace_archive(root, raw.getvalue())
            with patch.object(script, 'MAX_UNCOMPRESSED_BYTES', 64 * 1024):
                with self.assertRaisesRegex(script.PublicationError, 'total uncompressed-size'):
                    script.prepare_files(root, 'test-ffi-v1.2.3')

    def test_trailing_data_after_tar_end_is_bounded(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            make_assets(root)
            path = root / 'libtest_ffi-linux-x86_64.tar.gz'
            raw = gzip.decompress(path.read_bytes()) + b'\0' * (128 * 1024)
            self.replace_archive(root, raw)
            with patch.object(script, 'MAX_UNCOMPRESSED_BYTES', 64 * 1024):
                with self.assertRaisesRegex(script.PublicationError, 'total uncompressed-size'):
                    script.prepare_files(root, 'test-ffi-v1.2.3')

    def test_wrong_platform_and_malformed_libraries_rejected(self):
        cases = [
            ('linux-x86_64', library_header('linux-arm64')),
            ('linux-arm64', library_header('linux-x86_64')),
            ('macos-arm64', library_header('linux-arm64')),
            ('linux-arm64', library_header('macos-arm64')),
        ]
        for platform in script.PLATFORMS:
            header = library_header(platform)
            cases.extend([(platform, b'test'), (platform, header[:19])])
            executable = bytearray(header)
            struct.pack_into('<H' if platform.startswith('linux') else '<I',
                             executable, 16 if platform.startswith('linux') else 12, 2)
            cases.append((platform, bytes(executable)))
        macho_x86 = bytearray(library_header('macos-arm64'))
        struct.pack_into('<I', macho_x86, 4, 0x01000007)
        cases.append(('macos-arm64', bytes(macho_x86)))
        for platform, payload in cases:
            with self.subTest(platform=platform, payload=payload), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                make_assets(root, (platform, payload))
                # Matching checksums must not make a wrong-platform library acceptable.
                with self.assertRaises(script.PublicationError):
                    script.prepare_files(root, 'test-ffi-v1.2.3')

    def test_corrupt_last_archive_fails_before_publication(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            make_assets(root)
            (root / 'libtest_ffi-macos-arm64.tar.gz').write_bytes(b'corrupt')
            with self.assertRaisesRegex(script.PublicationError, 'checksum'):
                script.prepare_files(root, 'test-ffi-v1.2.3')

    def test_missing_checksum_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            make_assets(root)
            next(root.glob('*.sha256')).unlink()
            with self.assertRaises(script.PublicationError):
                script.prepare_files(root, 'test-ffi-v1.2.3')

    def test_symlink_archive_member_rejected_even_with_valid_checksum(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            make_assets(root)
            name = 'libtest_ffi-linux-x86_64.tar.gz'
            with tarfile.open(root / name, 'w:gz') as tar:
                info = tarfile.TarInfo('libtest_ffi.so')
                info.type = tarfile.SYMTYPE
                info.linkname = '/tmp/evil'
                tar.addfile(info)
            digest = hashlib.sha256((root / name).read_bytes()).hexdigest()
            (root / f'{name}.sha256').write_text(f'{digest}  {name}\n')
            with self.assertRaisesRegex(script.PublicationError, 'layout'):
                script.prepare_files(root, 'test-ffi-v1.2.3')

    def test_invalid_tag_rejected(self):
        with self.assertRaises(script.PublicationError):
            script.prepare_files(Path('/nonexistent'), '../other')


class DeliveryTests(unittest.TestCase):
    def test_dry_run_never_calls_publisher(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            make_assets(root)
            with patch.object(script, 'publish_file') as publish, patch('sys.argv', [
                'script', '--tag', 'test-ffi-v1.2.3', '--assets-dir', str(root),
            ]):
                self.assertEqual(script.main(), 0)
                publish.assert_not_called()

    def test_last_invalid_platform_prevents_all_writes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            make_assets(root, ('macos-arm64', b'not a library'))
            with patch.object(script, 'publish_file') as publish, patch('sys.argv', [
                'script', '--tag', 'test-ffi-v1.2.3', '--assets-dir', str(root),
                '--publish', '--bucket', 'test', '--base-url', 'https://example.invalid',
            ]):
                self.assertEqual(script.main(), 1)
                publish.assert_not_called()


class TransportTests(unittest.TestCase):
    def setUp(self):
        import public_artifacts
        self.transport = public_artifacts
        self.payload = b'library bytes'
        self.item = {'path': Path('library.tar.gz'), 'key': 'test-ffi/test-ffi-v1.2.3/library.tar.gz',
                     'content_type': 'application/gzip', 'size': len(self.payload),
                     'sha256': hashlib.sha256(self.payload).hexdigest()}

    def response(self, payload=None):
        result = io.BytesIO(self.payload if payload is None else payload)
        result.headers = {}
        return result

    def publish(self, runner, opener=None, sleeper=lambda _: None):
        return self.transport.publish_file(self.item, bucket='test', base_url='https://example.invalid',
                                          runner=runner, opener=opener or (lambda *_a, **_k: self.response()),
                                          sleeper=sleeper)

    def test_create_uses_conditional_write_and_checks_anonymous_readback(self):
        def runner(command, **kwargs):
            self.assertEqual(command[command.index('--if-none-match') + 1], '*')
            self.assertEqual(command[command.index('--key') + 1], self.item['key'])
            return subprocess.CompletedProcess(command, 0, '', '')
        self.assertEqual(self.publish(runner), 'created')
        with self.assertRaises(self.transport.PublicationError):
            self.publish(runner, opener=lambda *_a, **_k: self.response(b'wrong bytes'))

    def test_existing_object_must_match(self):
        runner = lambda cmd, **kw: subprocess.CompletedProcess(cmd, 1, '', 'PreconditionFailed')
        self.assertEqual(self.publish(runner), 'existing')
        with self.assertRaises(self.transport.PublicationError):
            self.publish(runner, opener=lambda *_a, **_k: self.response(b'wrong bytes'))

    def test_conflicts_have_bounded_retries(self):
        calls = []
        def runner(cmd, **kw):
            calls.append(cmd)
            return subprocess.CompletedProcess(cmd, 1, '', 'ConditionalRequestConflict')
        with self.assertRaises(self.transport.PublicationError):
            self.publish(runner)
        self.assertEqual(len(calls), 3)

    def test_other_upload_errors_are_not_accepted_as_existing(self):
        runner = lambda cmd, **kw: subprocess.CompletedProcess(cmd, 1, '', 'AccessDenied')
        with self.assertRaises(self.transport.PublicationError):
            self.publish(runner)
