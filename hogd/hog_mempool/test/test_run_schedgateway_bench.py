"""Exercise the gateway runner's data-root selection with inert binaries."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

RUNNER = Path(__file__).with_name("run_schedgateway_bench.py")
STRESS_WRAPPER = RUNNER.resolve().parents[3] / "scripts/ci/schedgateway_stress_bench.sh"

NODE = """#!/usr/bin/env python3
import json, pathlib, sys, time
data = pathlib.Path(sys.argv[sys.argv.index('--data-dir') + 1])
manifest = data / 'runtime' / 'runtime-manifest.json'
manifest.parent.mkdir()
manifest.write_text('{}')
print(json.dumps({'manifestPath': str(manifest)}), flush=True)
while True:
    time.sleep(1)
"""

CLIENT = """#!/usr/bin/env python3
import json, pathlib, sys, os
if sys.argv[1] == '--emit-bootstrap-manifest':
    pathlib.Path(sys.argv[2]).write_text('{}')
else:
    assert pathlib.Path(sys.argv[1]).exists()
    if os.environ.get('TEST_FAIL'):
        sys.exit(9)
    pathlib.Path(sys.argv[2]).write_text(json.dumps({
        'measuredSuccess': 10, 'warmupSuccess': 2, 'drainSuccess': 1,
        'committedTransactions': 13, 'error': '', 'measuredMicros': 1000000,
        'measuredCalls': 1, 'latencyP50Us': 10, 'latencyP99Us': 12,
        'retryable': 0,
    }))
"""


class DataRootTest(unittest.TestCase):
    def run_case(self, override, failures=False):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build, output = root / "build", root / "results"
            default, dedicated = root / "default", root / "dedicated"
            default.mkdir()
            dedicated.mkdir()
            for path, contents in (
                (build / "hogd/devnode/hog-devnode", NODE),
                (build / "hogd/hog_mempool/test/schedgateway_client_bench", CLIENT),
            ):
                path.parent.mkdir(parents=True)
                path.write_text(contents)
                path.chmod(0o755)
            env = dict(os.environ, TMPDIR=str(default))
            env.pop("HOGD_CI_SCHEDGATEWAY_DATA_ROOT", None)
            env.pop("TEST_FAIL", None)
            if override:
                env["HOGD_CI_SCHEDGATEWAY_DATA_ROOT"] = str(dedicated)
            if failures:
                env["TEST_FAIL"] = "1"
            run = subprocess.run(
                [sys.executable, str(RUNNER), str(build), str(output)],
                env=env, capture_output=True, text=True, timeout=10,
            )
            self.assertEqual(run.returncode == 0, not failures, run.stderr)
            manifest = Path(json.loads((output / "readiness.json").read_text())["manifestPath"])
            self.assertEqual(manifest.parent.parent.parent, dedicated if override else default)
            self.assertTrue((output / "manifest.json").is_file())
            self.assertTrue((output / "node.log").is_file())
            self.assertTrue((output / "client.log").is_file())
            self.assertEqual((output / "result.json").is_file(), not failures)
            self.assertEqual(list(default.iterdir()), [])
            self.assertEqual(list(dedicated.iterdir()), [])

    def test_configured_storage_only_applies_to_stress_batches(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            stress = root / "schedgateway_stress_bench.sh"
            stress.write_text(STRESS_WRAPPER.read_text())
            fake = root / "schedgateway_bench.sh"
            fake.write_text(
                '#!/usr/bin/env bash\n'
                'mkdir -p "$1"\n'
                'printf "%s;%s;%s;%s\\n" "$HOGD_CI_SCHEDGATEWAY_DATA_ROOT" '
                '"$SCHEDGATEWAY_BENCH_OUTSTANDING" "$SCHEDGATEWAY_BENCH_BATCH" '
                '"$SCHEDGATEWAY_BENCH_TRANSACTIONS" >"$1/receipt"\n'
            )
            fake.chmod(0o755)
            env = dict(os.environ, HOGD_CI_SCHEDGATEWAY_STRESS_BATCHES_ROOT="/dedicated")
            env.pop("HOGD_CI_SCHEDGATEWAY_DATA_ROOT", None)
            output = root / "output"
            subprocess.run(["bash", str(stress), str(output)], env=env,
                           check=True, capture_output=True)
            self.assertEqual((output / "calls/receipt").read_text().strip(), ";64;1;5000000")
            self.assertEqual((output / "batches/receipt").read_text().strip(),
                             "/dedicated;8;100;8000000")

    def test_default_location_and_unchanged_output(self):
        self.run_case(override=False)

    def test_explicit_location_and_unchanged_output(self):
        self.run_case(override=True)

    def test_explicit_location_reclaimed_after_failure(self):
        self.run_case(override=True, failures=True)


if __name__ == "__main__":
    unittest.main()
