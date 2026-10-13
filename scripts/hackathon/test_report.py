import json
from pathlib import Path
import tempfile
import unittest
from report import quantiles, summarize


class ReportTest(unittest.TestCase):
    def test_nearest_rank_percentiles_and_empty_samples(self):
        self.assertEqual(quantiles([]), {"p50": None, "p95": None, "p99": None})
        self.assertEqual(quantiles(list(range(1, 101))), {"p50": 50, "p95": 95, "p99": 99})

    def test_receipts_after_cutoff_do_not_inflate_window_throughput(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            (directory / "replay").mkdir()
            (directory / "corpus").mkdir()
            (directory / "corpus/manifest.json").write_text(json.dumps({"operations": 2, "stageSeconds": 1, "warmupSeconds": 1}))
            records = [{"phase": 1, "dueNs": 1_000_000_000, "startNs": 1_001_000_000, "endNs": 1_003_000_000, "receiptNs": 2_100_000_000, "outcome": "admitted", "execution": "ok"}, {"phase": 1, "dueNs": 1_100_000_000, "startNs": 0, "endNs": 0, "outcome": "outstanding_limit"}]
            (directory / "replay/results.jsonl").write_text("\n".join(json.dumps(r) for r in records))
            (directory / "mempool.stdout.log").write_text("INGESTION_INTERVAL phase=submit_gateway start_ns=100 end_ns=300\nINGESTION_INTERVAL phase=commit start_ns=200 end_ns=400\n")
            samples = [{"name": "mempool", "text": f"mempool_submitted_unvetted_tx_count {value}\n"} for value in (0, 1)]
            (directory / "replay/metrics.jsonl").write_text("\n".join(json.dumps(r) for r in samples))
            result = summarize(directory)
            phase = result["phases"]["1"]
            self.assertEqual(phase["submitted"], 1)
            self.assertEqual(phase["admittedWithinWindowPerSecond"], 1)
            self.assertEqual(phase["observedCommittedWithinWindowPerSecond"], 0)
            self.assertEqual(result["overlap"]["commit"]["sampledOverlappingSubmissions"], 1)
            self.assertFalse(result["overlap"]["validate"]["exercised"])


if __name__ == "__main__":
    unittest.main()
