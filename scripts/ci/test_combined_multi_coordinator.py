import importlib.util
import io
import json
from pathlib import Path
import unittest

path = Path(__file__).with_name("combined_multi_coordinator.py")
spec = importlib.util.spec_from_file_location("combined_multi_coordinator", path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def line(number, body, file="vproposal"):
    return (f"\033[90m2026-11-23T13:44:{number:02}.000000012Z\033[0m INF "
            f"/workspace/thruput-go/chuckd/sub/baton/baton/{file}.go:52 {body}\n").encode()


def validation(start, height, value, trace, outcome):
    typed = f"height {height}; tx_count 1000000; value {value}; proposer vid:aa39ad trace={trace}"
    key = f"height={height}; value={value} trace={trace}"
    events = [line(start, "VALIDATING: " + typed), line(start + 1, "scheduler validating: " + key)]
    if outcome == "repair":
        events += [line(start + 2, "scheduler is missing transactions: " + key),
                   line(start + 4, "scheduler validating: " + key)]
    elif outcome == "error":
        events += [line(start + 2, "cannot validate, scheduler error (untrusted-payload) trace=" + trace)]
    return b"".join(events + [line(start + 5, "VALIDATED: " + typed)])


class CoordinatorSummaryTest(unittest.TestCase):
    def reduce(self, raw):
        return module.reduce_log(io.BytesIO(raw), len(raw))

    def test_multiple_cycles_distinguish_repair_failure_and_winning_value(self):
        one, two, three = "a" * 12, "b" * 12, "c" * 12
        raw = validation(1, 34, one, "abcd", "repair")
        raw += line(7, f"COMMITTED: height 34; tx_count 1000000; value {one}; proposer vid:aa39ad", "commit")
        raw += validation(10, 35, two, "efgh", "normal")
        raw += line(16, f"COMMITTED: height 35; tx_count 1000000; value {three}; proposer vid:aa39ad", "commit")
        raw += validation(20, 36, three, "ijkl", "error")
        result = self.reduce(raw)
        self.assertEqual(result["full_committed_values"], 2)
        joins = result["validation_joins"]
        self.assertEqual([r["value_committed"] for r in joins], [True, False, False])
        self.assertEqual([r["rpc_attempts"] for r in joins], [2, 1, 1])
        self.assertEqual([r["result"] for r in joins],
                         ["ended_without_logged_failure", "ended_without_logged_failure", "logged_failure"])
        elapsed = [r["offset_ns"] for r in result["events"] if r.get("height") == 34 and r["kind"] == "validation_rpc"]
        self.assertEqual(elapsed, [1_000_000_000, 4_000_000_000])
        self.assertNotIn("untrusted-payload", json.dumps(result))

    def test_nested_ambiguous_truncated_and_malformed_records(self):
        raw = b"[truncated by ci_truncate_log_to_tail: kept last 1048576 bytes]\n"
        raw += b"[scpd:out] " + line(1, "scheduler validating: height=1; value=aaaaaaaaaaaa trace=aa")
        raw += validation(2, 34, "a" * 12, "abc", "repair")
        raw += validation(10, 35, "b" * 12, "abc", "repair")
        raw += line(22, "cannot validate, scheduler error (transport is closed) trace=abc")
        raw += line(23, "scheduler validating: height=forged; value=payload trace=bad")
        raw += b"x" * (module.MAX_LINE + 2) + b"\npartial"
        result = self.reduce(raw)
        self.assertEqual(len(result["events"]), 11)
        self.assertTrue(result["prior_tail_truncation"])
        self.assertEqual(result["omitted"], dict(unrecognized_selected_lines=1, oversize_lines=1, partial_final_line=1))
        self.assertEqual(result["unjoined_rpc_errors"], 1)
        self.assertEqual([r["result"] for r in result["validation_joins"]], ["ambiguous", "ambiguous"])

    def test_ambiguous_same_key_does_not_become_receipt_success(self):
        raw = validation(1, 34, "a" * 12, "abc", "normal")
        result = self.reduce(raw + validation(10, 34, "a" * 12, "abc", "normal"))
        self.assertEqual(result["validation_joins"][0]["result"], "ambiguous")

    def test_file_boundaries_and_rejected_path(self):
        with self.assertRaisesRegex(ValueError, "shrank"):
            module.reduce_log(io.BytesIO(b""), 1)
        with self.assertRaisesRegex(ValueError, "unexpected control"):
            module.capture("/tmp/hogd_stderr_1_1_abcdef; printf raw")
        with self.assertRaisesRegex(ValueError, "too many"):
            raw = line(1, "scheduler validating: height=1; value=aaaaaaaaaaaa trace=aa") * (module.MAX_EVENTS + 1)
            self.reduce(raw)


if __name__ == "__main__":
    unittest.main()
