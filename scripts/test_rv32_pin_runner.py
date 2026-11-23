#!/usr/bin/env python3
"""Checks that the RV32 cost scripts refuse to measure unpinned.

Run: python3 -m unittest discover -s scripts -p 'test_rv32_pin_runner.py'
"""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SCRIPTS = Path(__file__).resolve().parent
HELPER = SCRIPTS / "rv32_pin_runner.sh"
REFUSED = "setarch: failed to set personality to x86_64: Operation not permitted"


class PinRunnerTest(unittest.TestCase):
    def run_helper(self, taskset="ok", setarch="ok", allow=None):
        """Source the helper with stub tools; return (status, RUNNER, stderr)."""
        with tempfile.TemporaryDirectory() as temporary:
            bin_dir = Path(temporary)
            # Only what the helper needs, so a real taskset or setarch on the
            # host cannot stand in for a missing stub.
            for tool in ("uname", "true"):
                (bin_dir / tool).symlink_to(shutil.which(tool))
            stubs = {"taskset": taskset, "setarch": setarch}
            for tool, behaviour in stubs.items():
                if behaviour == "missing":
                    continue
                body = "exit 0" if behaviour == "ok" else (
                    f"echo '{behaviour}' >&2; exit 1")
                stub = bin_dir / tool
                stub.write_text(f"#!/bin/sh\n{body}\n")
                stub.chmod(0o755)
            env = {"PATH": str(bin_dir)}
            if allow is not None:
                env["RV32_ALLOW_UNPINNED"] = allow
            result = subprocess.run(
                [shutil.which("bash"), "-euo", "pipefail", "-c",
                 f'source "{HELPER}"; rv32_pin_runner; '
                 'for arg in "${RUNNER[@]}"; do printf "%s\\n" "$arg"; done'],
                env=env, text=True, capture_output=True,
            )
            return result.returncode, result.stdout.split(), result.stderr

    def test_pins_core_and_layout(self):
        status, runner, stderr = self.run_helper()
        self.assertEqual(status, 0, stderr)
        arch = os.uname().machine
        self.assertEqual(runner, ["taskset", "-c", "0", "setarch", arch, "-R"])
        self.assertEqual(stderr, "")

    def test_refused_layout_pinning_is_an_error(self):
        # What Docker's default seccomp profile does to setarch -R.
        status, runner, stderr = self.run_helper(setarch=REFUSED)
        self.assertEqual(status, 1)
        self.assertEqual(runner, [])
        self.assertIn("a fixed code layout", stderr)
        self.assertIn("Operation not permitted", stderr)
        self.assertIn("RV32_ALLOW_UNPINNED=1", stderr)

    def test_missing_tools_are_errors(self):
        for stubs, message in (
            ({"taskset": "missing"}, "taskset not found"),
            ({"setarch": "missing"}, "setarch not found"),
            ({"taskset": "taskset: failed to set pid 0 affinity"},
             "taskset -c 0: taskset: failed"),
        ):
            with self.subTest(stubs=stubs):
                status, _, stderr = self.run_helper(**stubs)
                self.assertEqual(status, 1)
                self.assertIn(message, stderr)

    def test_explicit_opt_out_runs_with_what_is_available(self):
        status, runner, stderr = self.run_helper(setarch=REFUSED, allow="1")
        self.assertEqual(status, 0, stderr)
        self.assertEqual(runner, ["taskset", "-c", "0"])
        self.assertIn("measuring without a fixed code layout", stderr)
        self.assertIn("not comparable", stderr)

    def test_only_1_opts_out(self):
        status, _, _ = self.run_helper(setarch=REFUSED, allow="yes")
        self.assertEqual(status, 1)

    def test_cost_scripts_use_the_helper(self):
        for name in ("rv32_gas_calibrate.sh", "rv32_syscall_bench.sh",
                     "rv32_openssl_speed.sh"):
            with self.subTest(script=name):
                text = (SCRIPTS / name).read_text()
                self.assertIn("rv32_pin_runner.sh", text)
                self.assertIn("\nrv32_pin_runner\n", text)
                # The old fallback: probe, and skip quietly when refused.
                self.assertNotIn('-R true >/dev/null 2>&1; then', text)


if __name__ == "__main__":
    unittest.main()
