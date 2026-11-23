#!/usr/bin/env python3
"""Regression tests for calibration completeness, bounds, and shared units."""

import copy
import io
import json
import math
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout, redirect_stderr

import rv32_gas_report as report
import rv32_syscall_report as syscall
from rv32_gas_units import gas_unit_ns


def synthetic_run():
    return {
        "_path": "synthetic", "config": {"repeats": 3},
        "provenance": {"host": "test", "engine_fingerprint": "test"},
        "baseline": {"ns_per_filler_instruction": 1.0},
        "opcodes": [{"op": "ADD", "context": "independent",
                     "working_set": "none", "magnitude": "default",
                     "branch_mode": "", "usable": True, "pair": False,
                     "ns_saturated": 1.0, "scheduled_gas": 1}],
        "syscalls": [],
        "workloads": [{"name": name, "ns_per_iteration": 100.0,
                       "scheduled_gas_per_iteration": 100,
                       "instructions_per_iteration": 100, "ns_per_gas": 1.0,
                       "op_counts": {"ADD": 100}, "samples": 3,
                       "ran_clean": True, "shape": "unfused ALU",
                       "relative_mad": 0.0, "fused_slots": 0}
                      for name in sorted(report.REQUIRED_WORKLOADS)],
    }


class WorkloadGateTest(unittest.TestCase):
    def setUp(self):
        self.data = synthetic_run()
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.ref = Path(self.tmp.name) / "reference.txt"
        self.ref.write_text("engine_fingerprint test\ntolerance 0.05\n"
                            "guard ADD independent 1.0 1\nworkload_spread 1.0\n")

    def check(self, data):
        with redirect_stdout(io.StringIO()):
            return report.check_against_reference(data, self.ref)

    def test_complete_valid_run_passes(self):
        self.assertEqual(self.check(self.data), 0)

    def test_uniform_slowdown_fails_despite_unchanged_spread(self):
        for w in self.data["workloads"]:
            w["ns_per_iteration"] *= 100
            w["ns_per_gas"] *= 100
        self.assertEqual(self.check(self.data), 1)

    def test_missing_empty_duplicate_and_failed_workloads_fail(self):
        cases = [[], self.data["workloads"][:-1],
                 self.data["workloads"] + [self.data["workloads"][0]]]
        for rows in cases:
            with self.subTest(rows=len(rows)):
                data = copy.deepcopy(self.data)
                data["workloads"] = rows
                self.assertEqual(self.check(data), 1)
        for change in ({"ran_clean": False}, {"samples": 2},
                       {"ns_per_iteration": 0, "ns_per_gas": 0},
                       {"ns_per_iteration": math.nan},
                       {"ns_per_iteration": math.inf}, {"op_counts": {}},
                       {"op_counts": {"ADD": 99}}, {"ns_per_gas": 0.5}):
            with self.subTest(change=change):
                data = copy.deepcopy(self.data)
                data["workloads"][0].update(change)
                self.assertEqual(self.check(data), 1)

    def test_unmeasured_opcode_fails(self):
        self.data["workloads"][0]["op_counts"] = {"LW": 100}
        self.assertEqual(self.check(self.data), 1)

    def test_old_unverified_measurement_fails(self):
        del self.data["workloads"][0]["ran_clean"]
        self.assertEqual(self.check(self.data), 1)

    def test_missing_rate_and_invalid_repeat_count_fail_cleanly(self):
        del self.data["workloads"][0]["ns_per_gas"]
        self.data["config"]["repeats"] = "three"
        self.assertEqual(self.check(self.data), 1)

    def test_predict_and_reference_cannot_accept_partial_suite(self):
        self.data["workloads"] = []
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            self.assertEqual(report.print_prediction(self.data), 1)
            self.assertEqual(report.emit_reference(self.data), 1)

    def test_inventory_matches_cpp(self):
        source = (Path(__file__).resolve().parents[1] /
                  "hogd/hog_exec/runtime/rv32/gas_calibration/workloads.cc").read_text()
        names = re.findall(r'\{"([a-z_]+)", "[^"]+", &\w+\}', source)
        self.assertEqual(set(names), report.REQUIRED_WORKLOADS)

    def test_cli_check_exits_nonzero_on_bound_violation(self):
        self.data["workloads"][0].update(ns_per_iteration=10000, ns_per_gas=100)
        path = Path(self.tmp.name) / "run.json"
        path.write_text(json.dumps(self.data))
        result = subprocess.run([sys.executable, str(Path(report.__file__)),
                                 str(path), "--check", str(self.ref)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("workload bound violation", result.stdout)


class GasUnitsTest(unittest.TestCase):
    def test_defaults_and_normalizers_agree(self):
        data = synthetic_run()
        self.assertEqual(report.DEFAULT_RESOLUTION, 1)
        self.assertEqual(report.DEFAULT_RESOLUTION, syscall.DEFAULT_RESOLUTION)
        self.assertEqual(report.baseline_ns(data), syscall.baseline_ns_from_calibration(data))

    def test_proposals_share_units_and_scale_ecall_credit(self):
        data = synthetic_run()
        bench = {"benchmarks": [
            {"name": "entry/GetBlockHeight", "real_time": 10, "gas": 1},
            {"name": "vm/GetBlockHeight", "real_time": 20,
             "items_per_second": 5e7},
        ]}
        for resolution in (1, 4):
            with self.subTest(resolution=resolution):
                _, unit = report.build_schedule([data], resolution)
                proposal = syscall.build_schedule(bench, data, resolution)
                self.assertEqual(unit, proposal["unit_ns"])
                self.assertEqual(proposal["ecall_gas"], resolution)
                self.assertEqual(proposal["base_gas"] + proposal["ecall_gas"],
                                 math.ceil(proposal["base_ns"] / unit))

    def test_invalid_units_rejected(self):
        for resolution in (0, -1, 0.5):
            with self.assertRaises(ValueError):
                gas_unit_ns(1.0, resolution)


if __name__ == "__main__":
    unittest.main()
