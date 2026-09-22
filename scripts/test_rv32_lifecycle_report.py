import unittest

from rv32_lifecycle_report import lifecycle_rows


class LifecycleReportTest(unittest.TestCase):
    def test_median_and_units(self):
        plain = {"name": "lifecycle/Setup/4096", "real_time": 10, "time_unit": "us"}
        median = dict(plain, run_name=plain["name"], aggregate_name="median", real_time=2)
        self.assertEqual(lifecycle_rows({"benchmarks": [plain, median]}),
                         {plain["name"]: 2000})

    def test_failed_repetition_invalidates_median(self):
        good = {"run_name": "lifecycle/Setup/4096", "aggregate_name": "median",
                "real_time": 20, "time_unit": "ns"}
        failed = dict(good, error_occurred=True, error_message="wrong result")
        with self.assertRaises(ValueError):
            lifecycle_rows({"benchmarks": [good, failed]})

    def test_missing_or_invalid_timings(self):
        with self.assertRaises(ValueError):
            lifecycle_rows({"benchmarks": []})
        for value in (0, -1, float("nan"), float("inf")):
            with self.subTest(value=value), self.assertRaises(ValueError):
                lifecycle_rows({"benchmarks": [{"name": "lifecycle/Setup/4096",
                                                "real_time": value, "time_unit": "ns"}]})


if __name__ == "__main__":
    unittest.main()
