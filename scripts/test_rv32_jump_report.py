"""Jump bounds must affect prices without redefining the gas unit."""

import unittest

import rv32_gas_report as report
from rv32_gas_units import baseline_ns


class JumpReportTest(unittest.TestCase):
    def test_absolute_jump_bound_is_resolved_and_preserved(self):
        jump = {"op": "JALR", "context": "target_chase/shuffled/n4096",
                "working_set": "none", "magnitude": "default",
                "branch_mode": "", "pair": False, "usable": True,
                "jump_kernel": True, "ns_saturated": 0.0,
                "ns_upper_bound": 12.0, "ns_per_instruction": 12.0,
                "slope_stderr_ns": 0.0, "scheduled_gas": 3}
        alu = dict(jump, op="ADD", jump_kernel=False, ns_saturated=1.5,
                   ns_upper_bound=0.0, scheduled_gas=1)
        data = {"opcodes": [alu] + [dict(jump, context=str(i)) for i in range(100)],
                "provenance": {"host": "test"}}
        self.assertTrue(report.resolved(jump))
        self.assertEqual(report.cost_ns(jump), 12.0)
        self.assertEqual(baseline_ns(data), 1.5)
        merged = report.merge_runs([data])
        rows = merged.values() if isinstance(merged, dict) else merged
        for row in rows:
            if row["op"] == "JALR":
                self.assertTrue(row["resolved"])
                self.assertEqual(row["ns_per_instruction"], 12.0)


if __name__ == "__main__":
    unittest.main()
