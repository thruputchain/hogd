#!/usr/bin/env python3
"""Exercise the offline CI branches without contacting dependency services."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class OfflineTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        for tool in (
            'cargo',
            'rustup',
            'cbindgen',
            'sccache',
            'gcc-15',
            'g++-15',
            'clang',
            'thruput',
            'git',
        ):
            script = self.bin / tool
            script.write_text(
                '#!/bin/sh\n'
                'echo "unexpected tool execution: $0" >&2\nexit 99\n'
            )
            script.chmod(0o755)
        self.env = dict(
            os.environ,
            HOGD_CI_OFFLINE='1',
            PATH=f'{self.bin}:/usr/bin',
            HOME=str(self.root),
        )

    def tearDown(self):
        self.temporary.cleanup()

    def run_script(self, relative, **kwargs):
        return subprocess.run(
            ['bash', str(ROOT / relative)],
            cwd=self.root,
            env=self.env,
            capture_output=True,
            text=True,
            **kwargs,
        )

    def test_fetch_uses_only_preinstalled_tools(self):
        self.assertEqual(self.run_script('scripts/fetch.sh').returncode, 0)

    def test_metrics_creation_and_publication_request_do_not_run_git(self):
        self.assertEqual(
            self.run_script('scripts/clone_metrics_history.sh').returncode, 0
        )
        metric = self.root / 'metrics_history/metrics/sample'
        metric.write_text('123')
        self.env['METRICS_COMMIT_REPO'] = 'hogd-rv32-gas-calibration'
        self.assertEqual(
            self.run_script('scripts/commit_metrics.sh').returncode, 0
        )
        self.assertEqual(metric.read_text(), '123')
        self.assertEqual(
            (self.root / 'metrics_history/.publish-requested')
            .read_text()
            .strip(),
            'hogd-rv32-gas-calibration',
        )

    def test_prepare_env_preserves_explicit_compiler_selection(self):
        (self.root / 'scripts').mkdir()
        self.env.update(HOGD_CI_GCC_CC='gcc', HOGD_CI_GCC_CXX='g++')
        result = subprocess.run(
            [
                'bash',
                '-c',
                f'source "{ROOT}/scripts/ci/prepare_env.sh"; '
                '[[ "$CARGO_NET_OFFLINE" == true && "$GOPROXY" == off && '
                '"$GOSUMDB" == off && "$GOTOOLCHAIN" == local ]]',
            ],
            cwd=self.root,
            env=self.env,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            (self.root / 'scripts/conf.sh').read_text(),
            'GCC_CC=gcc\nGCC_CXX=g++\n',
        )

    def test_hog_ip_resolves_run_local_address(self):
        (self.bin / 'getent').write_text(
            '#!/bin/sh\nprintf "172.28.1.12 STREAM hog4\\n"\n'
        )
        (self.bin / 'getent').chmod(0o755)
        self.env['HOGD_CI_CONTAINER'] = '1'
        result = subprocess.run(
            ['bash', '-c', f'source "{ROOT}/scripts/hog-ip.sh"; hog_ip hog4'],
            env=self.env,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), '172.28.1.12')
        self.env.pop('HOGD_CI_CONTAINER')
        result = subprocess.run(
            ['bash', '-c', f'source "{ROOT}/scripts/hog-ip.sh"; hog_ip hog4'],
            env=self.env,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.stdout.strip(), '10.100.0.104')


if __name__ == '__main__':
    unittest.main()
