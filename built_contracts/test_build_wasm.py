#!/usr/bin/env python3
"""Tests for build_wasm.sh.

Run from built_contracts/:
    python3 -m unittest test_build_wasm -v
"""

import os
import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parent
SCRIPT = ROOT / "build_wasm.sh"


class TestBuildWasm(unittest.TestCase):
    def test_outputs_and_intermediates_live_under_target_dir(self) -> None:
        with tempfile.TemporaryDirectory(prefix="build-wasm-") as tmp:
            target_dir = pathlib.Path(tmp)

            env = os.environ.copy()
            env.pop("RUSTC_WRAPPER", None)
            env.pop("RUSTFLAGS", None)
            env.pop("CARGO_ENCODED_RUSTFLAGS", None)

            result = subprocess.run(
                [str(SCRIPT), "fibonacci", str(target_dir), "-F", "entrypoints"],
                cwd=ROOT,
                env=env,
                capture_output=True,
                text=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((target_dir / "fibonacci.wasm").exists())
            self.assertTrue((target_dir / "fibonacci_spec.json").exists())

            cargo_target_dir = target_dir / "cargo-target"
            self.assertTrue((cargo_target_dir / "groundhog" / "fibonacci.wasm").exists())
            self.assertTrue((cargo_target_dir / "groundhog" / "fibonacci.spec.json").exists())
            self.assertTrue((cargo_target_dir / "release").exists())
            self.assertTrue((cargo_target_dir / "wasm32-unknown-unknown").exists())


if __name__ == "__main__":
    unittest.main()
