"""Changing-target sequences validate gas without setting opcode prices."""

import contextlib
import io
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import rv32_gas_report as report
from rv32_gas_units import baseline_ns


def cell(op, ns, gas, context="independent", jump=False):
    return dict(op=op, context=context, working_set="none", magnitude="default",
                branch_mode="", pair=False, usable=True, jump_kernel=jump,
                ns_saturated=0.0 if jump else ns, ns_upper_bound=ns,
                ns_per_instruction=ns, slope_stderr_ns=0.0, scheduled_gas=gas)


def fixture():
    # Values from run 35812028325, rounded in its printed report.
    rows = [cell("ADD", 1.6213, 1), cell("ADDI", 1.6213, 1),
            cell("LW", 4.0435, 3),
            cell("JAL", 4.8832, 1, "no_link/shuffled/n512", True),
            cell("JALR", 4.4167, 3, "fixed_base_independent/shuffled/n64", True),
            cell("JALR", 15.2398, 3, "target_chase/shuffled/n4096", True)]
    return dict(opcodes=rows, provenance={"host": "test"}, _path="test")


class JumpReportTest(unittest.TestCase):
    def test_prices_exclude_sequence_and_preserve_isolated_bounds(self):
        data = fixture()
        schedule, unit = report.build_schedule([data])
        self.assertEqual(unit, 1.6213)
        self.assertEqual(schedule["JALR"]["proposed_gas"], 3)
        self.assertEqual(schedule["JAL"]["proposed_gas"], 4)
        self.assertEqual(schedule["JALR"]["ns"], 4.4167)
        self.assertFalse(any("target_chase" in row["context"]
                             for row in report.merge_runs([data]).values()))
        sequence = data["opcodes"][-1]
        data["opcodes"].extend(dict(sequence) for _ in range(100))
        self.assertEqual(baseline_ns(data), unit)

    def test_sequence_budget_uses_all_four_instructions(self):
        data = fixture()
        row, = report.jump_sequence_rows(data)
        self.assertEqual(row["gas"], 10)
        self.assertAlmostEqual(row["budget"], 16.213)
        self.assertIsNone(row["error"])
        schedule, unit = report.build_schedule([data])
        row, = report.jump_sequence_rows(
            data, {op: c["proposed_gas"] for op, c in schedule.items()}, unit)
        self.assertEqual(row["gas"], 13)
        self.assertIsNone(row["error"])

    def test_expensive_sequence_fails_without_inflating_jalr(self):
        data = fixture()
        data["opcodes"][-1]["ns_upper_bound"] = 30.0
        self.assertIn("exceeds gas budget", report.jump_sequence_rows(data)[0]["error"])
        self.assertTrue(any("exceeds gas budget" in f for f in report.workload_failures(data)))
        self.assertEqual(report.build_schedule([data])[0]["JALR"]["proposed_gas"], 3)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(report.print_schedule([data]), 1)
            self.assertEqual(report.print_prediction(data), 1)
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(report.emit_reference(data), 1)

    def test_partial_or_invalid_measurement_is_not_a_pass(self):
        data = fixture()
        data["opcodes"] = [c for c in data["opcodes"] if c["op"] != "LW"]
        self.assertIn("unmeasured", report.jump_sequence_rows(data)[0]["error"])
        for value in (0.0, float("nan"), float("inf")):
            data = fixture()
            data["opcodes"][-1]["ns_upper_bound"] = value
            self.assertIn("invalid", report.jump_sequence_rows(data)[0]["error"])

    def test_sequence_reference_guards_presence(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "reference.txt"
            path.write_text("guard_sequence target_chase/shuffled/n4096\n")
            ref = report.load_reference(path)
            self.assertEqual(ref["sequences"], {"target_chase/shuffled/n4096"})
            data = fixture()
            data.update(_path="test", workloads=[])
            data["opcodes"].pop()
            output = io.StringIO()
            with patch.object(report, "workload_failures", return_value=[]), contextlib.redirect_stdout(output):
                self.assertEqual(report.check_against_reference(data, str(path)), 1)
            self.assertIn("sequence not measured", output.getvalue())


if __name__ == "__main__":
    unittest.main()
