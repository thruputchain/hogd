"""Executable corpus checks, enabled when the opt-in builder/artifacts exist."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


@unittest.skipUnless(os.getenv("HACKATHON_CORPUS_BINARY") and os.getenv("HACKATHON_ARTIFACTS"), "set HACKATHON_CORPUS_BINARY and HACKATHON_ARTIFACTS")
class CorpusTest(unittest.TestCase):
    def test_reproducible_signed_corpus_and_unique_transaction_ids(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            for output in (root / "first", root / "second"):
                subprocess.run([os.environ["HACKATHON_CORPUS_BINARY"], "--artifacts", os.environ["HACKATHON_ARTIFACTS"], "--out", str(output), "--apps", "5", "--users", "10", "--wallets", "2", "--stage-seconds", "2", "--warmup-seconds", "1", "--rate-divisor", "100"], check=True, capture_output=True)
            for filename in ("genesis.xdr", "transactions.jsonl", "manifest.json", "artifacts.json"):
                self.assertEqual((root / "first" / filename).read_bytes(), (root / "second" / filename).read_bytes(), filename)
            rows = [json.loads(line) for line in (root / "first/transactions.jsonl").read_text().splitlines()]
            self.assertEqual(len(rows), len({r["hash"] for r in rows}))
            self.assertEqual([r["dueNs"] for r in rows], sorted(r["dueNs"] for r in rows))
            manifest = json.loads((root / "first/manifest.json").read_text())
            self.assertEqual(len(rows), manifest["operations"])
            self.assertEqual(len({r["hash"] for r in manifest["contracts"]}), 5)
            self.assertEqual({r["app"] for r in rows if r["kind"] == "deploy"}, {4})


if __name__ == "__main__":
    unittest.main()
