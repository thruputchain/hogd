#!/usr/bin/env python3
"""Check private/public cache selection without contacting a server."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).with_name("sccache-env.sh").resolve()


class SccacheEnvironmentTest(unittest.TestCase):
    def environment(self, address="10.100.0.103", **overrides):
        temporary = tempfile.TemporaryDirectory(prefix="hogd-sccache-env-")
        self.addCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        ip = directory / "ip"
        ip.write_text(
            f"#!/bin/sh\necho '2: eth0 inet {address}/16 scope global eth0'\n"
        )
        ip.chmod(0o755)
        env = {
            k: v for k, v in os.environ.items()
            if not k.startswith(("SCCACHE_", "HOGD_SCCACHE_"))
        }
        env.update(PATH=f"{directory}:{env['PATH']}", **overrides)
        return env

    def settings(self, **kwargs):
        result = subprocess.check_output(
            ["bash", str(SCRIPT), "--print-env"],
            env=self.environment(**kwargs), text=True,
        )
        return dict(line.split("=", 1) for line in result.splitlines())

    def test_private_host_uses_shared_cache(self):
        settings = self.settings()
        self.assertEqual(
            settings["SCCACHE_WEBDAV_ENDPOINT"], "http://hog12:9281/"
        )
        self.assertEqual(settings["SCCACHE_WEBDAV_KEY_PREFIX"], "hogd/rust/")

    def test_public_host_keeps_normal_backend(self):
        self.assertEqual(self.settings(address="192.168.1.2"), {})

    def test_remote_discovery_can_be_disabled(self):
        self.assertEqual(self.settings(HOGD_SCCACHE_REMOTE="0"), {})

    def test_explicit_endpoint_and_prefix_win(self):
        settings = self.settings(
            SCCACHE_WEBDAV_ENDPOINT="http://example.invalid/cache/",
            SCCACHE_WEBDAV_KEY_PREFIX="other/",
        )
        self.assertEqual(
            settings["SCCACHE_WEBDAV_ENDPOINT"],
            "http://example.invalid/cache/",
        )
        self.assertEqual(settings["SCCACHE_WEBDAV_KEY_PREFIX"], "other/")

    def test_explicit_disk_or_other_backend_is_not_replaced(self):
        for override in [
            {"SCCACHE_DIR": "/tmp/custom-cache"},
            {"SCCACHE_CONF": "/tmp/custom-config"},
            {"SCCACHE_BUCKET": "custom-bucket"},
            {"SCCACHE_REDIS": "redis://example.invalid"},
        ]:
            with self.subTest(override=override):
                self.assertNotIn(
                    "SCCACHE_WEBDAV_ENDPOINT", self.settings(**override)
                )

    def test_authentication_is_not_embedded_in_build_commands(self):
        settings = self.settings(
            SCCACHE_WEBDAV_USERNAME="private-user",
            SCCACHE_WEBDAV_PASSWORD="private-password",
            SCCACHE_WEBDAV_TOKEN="private-token",
        )
        self.assertFalse(any(
            value.startswith("private-") for value in settings.values()
        ))

    def test_sourcing_disables_incremental_and_preserves_arguments(self):
        result = subprocess.check_output(
            ["bash", "-c", 'source "$1"; '
             'printf "%s %s %s" "$CARGO_INCREMENTAL" "$#" "$2"',
             "test", str(SCRIPT), "keep-argument"],
            env=self.environment(CARGO_INCREMENTAL="1"), text=True,
        )
        self.assertEqual(result, "0 2 keep-argument")

    @unittest.skipUnless(shutil.which("flock"), "warmup requires flock")
    def test_failed_warmup_reports_both_caches_and_preserves_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            scripts = root / "scripts"
            ci = scripts / "ci"
            ci.mkdir(parents=True)
            shutil.copy(SCRIPT, scripts / SCRIPT.name)
            warmup = SCRIPT.parent / "ci/warm_ccache.sh"
            shutil.copy(warmup, ci / warmup.name)
            commands = root / "bin"
            commands.mkdir()
            stubs = {
                "git": "echo test-commit\n",
                "ccache": "echo ccache-stub-stats\n",
                "sccache": "echo sccache-stub-stats\n",
            }
            for name, body in stubs.items():
                executable = commands / name
                executable.write_text("#!/bin/sh\n" + body)
                executable.chmod(0o755)
            build = scripts / "build.sh"
            build.write_text(
                '#!/bin/bash\n'
                '[[ "$CARGO_INCREMENTAL" == 0 ]] || exit 8\n'
                '[[ "$SCCACHE_MULTILEVEL_CHAIN" == webdav ]] || exit 9\n'
                '[[ "$CCACHE_RESHARE" == 1 ]] || exit 10\n'
                'exit 7\n'
            )
            build.chmod(0o755)
            env = self.environment(
                SCCACHE_WEBDAV_ENDPOINT="http://example.invalid/",
                SCCACHE_MULTILEVEL_CHAIN="disk,webdav",
                HOGD_CCACHE_WARMUP_JOBS="1",
                HOGD_CCACHE_WARMUP_COMPILER="gcc",
                HOGD_CCACHE_WARMUP_BUILD_TYPE="Release",
                HOGD_CCACHE_WARMUP_VARIANT="default",
                HOGD_CCACHE_WARMUP_LOCK=str(root / "warmup.lock"),
            )
            env["PATH"] = f"{commands}:{env['PATH']}"
            result = subprocess.run(
                ["bash", str(ci / warmup.name), "test-commit"],
                env=env, text=True, capture_output=True,
            )
            self.assertEqual(result.returncode, 7, result.stderr)
            output = result.stdout.splitlines()
            self.assertEqual(output.count("ccache-stub-stats"), 2)
            self.assertEqual(output.count("sccache-stub-stats"), 2)


if __name__ == "__main__":
    unittest.main()
