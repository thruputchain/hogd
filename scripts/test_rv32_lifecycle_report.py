import unittest

from rv32_lifecycle_report import gas_estimate, lifecycle_rows


class LifecycleReportTest(unittest.TestCase):
    def test_median_and_units(self):
        plain = {"name": "lifecycle/SetupTeardown/4096", "real_time": 10, "time_unit": "us"}
        median = dict(plain, run_name=plain["name"], aggregate_name="median", real_time=2)
        self.assertEqual(lifecycle_rows({"benchmarks": [plain, median]}),
                         {plain["name"]: 2000})

    def test_failed_repetition_invalidates_median(self):
        good = {"run_name": "lifecycle/SetupTeardown/4096", "aggregate_name": "median",
                "real_time": 20, "time_unit": "ns"}
        failed = dict(good, error_occurred=True, error_message="wrong result")
        with self.assertRaises(ValueError):
            lifecycle_rows({"benchmarks": [good, failed]})

    def test_manual_timing_and_floor(self):
        def row(name, ns):
            return {"run_name": name + "/iterations:100000/manual_time", "aggregate_name": "median",
                    "real_time": ns, "time_unit": "ns"}
        data = {"context": {"lifecycle_timing_version": "2"}, "benchmarks": [
            row("lifecycle/TimerFloor", 3), row("lifecycle/Setup/4096", 100),
            row("lifecycle/Teardown/4096", 8)]}
        measured = lifecycle_rows(data)
        self.assertEqual(gas_estimate("lifecycle/Setup/4096", 100, measured, 2), "50")
        self.assertEqual(gas_estimate("lifecycle/Teardown/4096", 8, measured, 2),
                         "unresolved")
        self.assertEqual(gas_estimate("lifecycle/TimerFloor", 3, measured, 2),
                         "timer floor")
        data["benchmarks"].pop(0)
        with self.assertRaisesRegex(ValueError, "missing lifecycle/TimerFloor"):
            lifecycle_rows(data)

    def test_refuses_old_timer_method(self):
        data = {"benchmarks": [{"name": "lifecycle/Setup/4096", "real_time": 801,
                                "time_unit": "ns"}]}
        with self.assertRaisesRegex(ValueError, "obsolete"):
            lifecycle_rows(data)

    def test_missing_or_invalid_timings(self):
        with self.assertRaises(ValueError):
            lifecycle_rows({"benchmarks": []})
        for value in (0, -1, float("nan"), float("inf")):
            with self.subTest(value=value), self.assertRaises(ValueError):
                lifecycle_rows({"benchmarks": [{"name": "lifecycle/SetupTeardown/4096",
                                                "real_time": value, "time_unit": "ns"}]})


if __name__ == "__main__":
    unittest.main()
