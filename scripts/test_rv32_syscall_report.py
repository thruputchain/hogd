#!/usr/bin/env python3
"""Unit tests for the storage half of rv32_syscall_report.py.

The report turns benchmark rows into fitted per-key costs and a proposal, and
a mistake in that arithmetic looks exactly like a measurement. These tests
feed it synthetic rows with known slopes and check that what comes out is
what went in -- and that the rows the schedule must not price stay out of it.
"""

import io
import tempfile
import os
import json
import unittest
from contextlib import redirect_stdout

import rv32_syscall_report as report


def row(name, real_time, aggregate="median", **extra):
    entry = {"run_name": name, "name": name, "real_time": real_time,
             "aggregate_name": aggregate, "time_unit": "ns"}
    entry.update(extra)
    return entry


def synthetic_run(push_ns=300.0, first_touch_ns=120.0, walk_ns=15.0,
                  noload_ns=60.0, set_add_ns=90.0, merge_item_ns=40.0,
                  cold_ns=25000.0):
    """A run whose storage rows lie exactly on known lines.

    frame/Push/D           = push_ns * D
    FirstTouch/D/K         = push_ns * D + K * (first_touch_ns + walk_ns * (D - 1))
    NoLoad/1/K             = push_ns + K * noload_ns
    SetAddFirstTouch/1/K   = push_ns + K * set_add_ns
    Discard/K              = 2 * push_ns + K * set_add_ns
    Merge/K                = Discard/K + K * merge_item_ns
    """
    depths = (1, 16, 254)
    keys = (1, 16, 256, 4096)
    benchmarks = [
        row("entry/GetBlockHeight", 70.0, gas=51),
        row("vm/GetBlockHeight", 100.0, items_per_second=5.12e9),
        row("entry/NNIntGet", 110.0, gas=51),
        row("entry/NNIntGetNoLoad", 40.0, gas=51),
        row("entry/NNIntSetAdd", 70.0, gas=51),
        row("cold/NNIntGet/iterations:20000", cold_ns, gas=51),
        row("cold/NNIntGet/iterations:20000", 12.5, aggregate="stddev"),
    ]
    for depth in depths:
        benchmarks.append(row(f"frame/Push/{depth}", push_ns * depth))
        for k in keys:
            per_key = first_touch_ns + walk_ns * (depth - 1)
            benchmarks.append(row(f"frame/NNIntGetFirstTouch/{depth}/{k}",
                                  push_ns * depth + k * per_key))
    for k in keys:
        benchmarks.append(row(f"frame/NNIntGetFirstTouchNoLoad/1/{k}",
                              push_ns + k * noload_ns))
        benchmarks.append(row(f"frame/NNIntSetAddFirstTouch/1/{k}",
                              push_ns + k * set_add_ns))
        discard = 2 * push_ns + k * set_add_ns
        benchmarks.append(row(f"frame/NNIntSetAddDiscard/{k}", discard))
        benchmarks.append(row(f"frame/NNIntSetAddMerge/{k}",
                              discard + k * merge_item_ns))
    return {"context": {"hogd_host": "test", "engine_fingerprint": "fp",
                        "hogd_production_build": "true"},
            "benchmarks": benchmarks}


def synthetic_calibration(baseline_ns=1.6):
    return {"provenance": {"host": "test", "engine_fingerprint": "fp",
                           "production_build": True},
            "opcodes": [{"usable": True, "ns_saturated": baseline_ns}]}


class StorageParsingTest(unittest.TestCase):
    def test_cold_rows_are_not_syscalls(self):
        measured = report.rows(synthetic_run())
        names = report.syscall_names(measured)
        self.assertIn("NNIntGet", names)
        self.assertNotIn("NNIntGet/cold", names)
        self.assertFalse(any(n.startswith("cold") for n in names))
        self.assertFalse(any(n.startswith("frame") for n in names))

    def test_sweeps_are_grouped_and_sorted(self):
        sweeps = report.storage_sweeps(report.rows(synthetic_run()))
        self.assertEqual(sorted(sweeps), ["NNIntGetFirstTouch",
                                          "NNIntGetFirstTouchNoLoad",
                                          "NNIntSetAddFirstTouch"])
        self.assertEqual(sorted(sweeps["NNIntGetFirstTouch"]), [1, 16, 254])
        keys = [k for k, _ in sweeps["NNIntGetFirstTouch"][16]]
        self.assertEqual(keys, [1, 16, 256, 4096])

    def test_commit_pairs_match_merge_to_discard(self):
        pairs = report.storage_commit_pairs(report.rows(synthetic_run()))
        self.assertEqual(list(pairs), ["NNIntSetAdd"])
        self.assertEqual([k for k, _, _ in pairs["NNIntSetAdd"]],
                         [1, 16, 256, 4096])


class StorageFitTest(unittest.TestCase):
    def setUp(self):
        run = synthetic_run()
        self.summary = report.storage_summary(report.rows(run),
                                              report.stddevs(run))

    def test_per_key_cost_recovers_the_input_at_every_k(self):
        first = self.summary["sweeps"]["NNIntGetFirstTouch"]
        for keys, cost in first[1]["per_key"]:
            self.assertAlmostEqual(cost, 120.0, places=6, msg=f"K={keys}")
        for keys, cost in first[16]["per_key"]:
            self.assertAlmostEqual(cost, 120.0 + 15.0 * 15, places=6,
                                   msg=f"K={keys}")
        noload = self.summary["sweeps"]["NNIntGetFirstTouchNoLoad"][1]
        self.assertAlmostEqual(noload["worst_ns"], 60.0, places=6)

    def test_baseline_is_the_push_row(self):
        for depth, entry in self.summary["sweeps"]["NNIntGetFirstTouch"].items():
            self.assertEqual(entry["baseline_ns"], 300.0 * depth)

    def test_exact_lines_are_flat(self):
        for entry in self.summary["sweeps"]["NNIntSetAddFirstTouch"].values():
            self.assertAlmostEqual(entry["spread"], 1.0, places=9)
            self.assertTrue(entry["flat"])

    def test_rising_per_key_cost_is_not_flat(self):
        run = synthetic_run()
        for entry in run["benchmarks"]:
            # Make each first touch at depth 1 cost 120 + K/20 ns: 1.7x from
            # K=1 to K=4096, roughly what the first real run showed.
            match = report.FRAME_SWEEP_ROW.match(entry["name"])
            if match and match.group(1) == "NNIntGetFirstTouch" \
                    and match.group(2) == "1":
                k = int(match.group(3))
                entry["real_time"] = 300.0 + k * (120.0 + k / 20.0)
        summary = report.storage_summary(report.rows(run), report.stddevs(run))
        entry = summary["sweeps"]["NNIntGetFirstTouch"][1]
        self.assertFalse(entry["flat"])
        self.assertEqual(entry["worst_at"], 4096)
        self.assertAlmostEqual(entry["worst_ns"], 120.0 + 4096 / 20.0,
                               places=6)

    def test_parent_walk_from_depth_sweep(self):
        per_frame, shallow, deep, keys = report.parent_walk_ns(self.summary)
        self.assertEqual((shallow, deep, keys), (1, 254, 4096))
        self.assertAlmostEqual(per_frame, 15.0, places=6)

    def test_sweep_without_baseline_is_reported_not_fitted(self):
        run = synthetic_run()
        run["benchmarks"] = [e for e in run["benchmarks"]
                             if e["name"] != "frame/Push/16"]
        summary = report.storage_summary(report.rows(run), report.stddevs(run))
        entry = summary["sweeps"]["NNIntGetFirstTouch"][16]
        self.assertIsNone(entry["baseline_ns"])
        self.assertNotIn("per_key", entry)
        out = io.StringIO()
        with redirect_stdout(out):
            report.print_table(run)
        self.assertIn("no frame/Push/16 baseline", out.getvalue())

    def test_commit_cost_per_item(self):
        for _, _, _, per_item in self.summary["commits"]["NNIntSetAdd"]:
            self.assertAlmostEqual(per_item, 40.0, places=6)

    def test_surcharge_subtracts_the_matching_warm_row(self):
        # per-key at depth 1 is 120 and the warm entry/NNIntGet is 110, so the
        # first touch itself is the 10 ns difference -- charging 120 on top of
        # a table that already prices the warm call bills it twice.
        first = self.summary["sweeps"]["NNIntGetFirstTouch"][1]
        self.assertAlmostEqual(first["worst_ns"], 120.0, places=6)
        self.assertAlmostEqual(first["warm_ns"], 110.0, places=6)
        self.assertAlmostEqual(first["surcharge_ns"], 10.0, places=6)

    def test_warm_row_mapping_covers_every_family(self):
        self.assertEqual(report.warm_syscall_for("NNIntGetFirstTouch"),
                         "NNIntGet")
        self.assertEqual(report.warm_syscall_for("NNIntGetFirstTouchNoLoad"),
                         "NNIntGetNoLoad")
        self.assertEqual(report.warm_syscall_for("NNIntSetAddFirstTouch"),
                         "NNIntSetAdd")
        noload = self.summary["sweeps"]["NNIntGetFirstTouchNoLoad"][1]
        self.assertAlmostEqual(noload["surcharge_ns"], 60.0 - 40.0, places=6)
        write = self.summary["sweeps"]["NNIntSetAddFirstTouch"][1]
        self.assertAlmostEqual(write["surcharge_ns"], 90.0 - 70.0, places=6)

    def test_surcharge_floors_at_zero(self):
        # A warm row dearer than the swept one is noise, not a negative cost.
        run = synthetic_run()
        for entry in run["benchmarks"]:
            if entry["name"] == "entry/NNIntGet":
                entry["real_time"] = 500.0
        summary = report.storage_summary(report.rows(run), report.stddevs(run))
        self.assertEqual(
            summary["sweeps"]["NNIntGetFirstTouch"][1]["surcharge_ns"], 0.0)

    def test_missing_warm_row_leaves_no_surcharge(self):
        run = synthetic_run()
        run["benchmarks"] = [e for e in run["benchmarks"]
                             if e["name"] != "entry/NNIntGet"]
        summary = report.storage_summary(report.rows(run), report.stddevs(run))
        entry = summary["sweeps"]["NNIntGetFirstTouch"][1]
        self.assertIn("worst_ns", entry)
        self.assertNotIn("surcharge_ns", entry)
        proposal = report.build_schedule(run, synthetic_calibration(1.6),
                                         resolution=1)
        self.assertNotIn("storage_first_touch_gas", proposal)

    def test_ragged_sweeps_align_under_their_own_columns(self):
        # Depth 1 sweeps further than the others; the table's columns are the
        # union and a missing point is blank rather than shifting the row.
        run = synthetic_run()
        for k in (16384, 65536):
            run["benchmarks"].append(
                row(f"frame/NNIntGetFirstTouch/1/{k}", 300.0 + k * 120.0))
        summary = report.storage_summary(report.rows(run), report.stddevs(run))
        self.assertEqual([k for k, _ in
                          summary["sweeps"]["NNIntGetFirstTouch"][1]["per_key"]],
                         [1, 16, 256, 4096, 16384, 65536])
        self.assertEqual([k for k, _ in
                          summary["sweeps"]["NNIntGetFirstTouch"][16]["per_key"]],
                         [1, 16, 256, 4096])
        out = io.StringIO()
        with redirect_stdout(out):
            report.print_table(run)
        text = out.getvalue()
        header = [ln for ln in text.split("\n")
                  if ln.startswith("NNIntGetFirstTouch ")][0]
        self.assertIn("65536", header)
        # The depth-16 row still ends at its own worst, not under a K it
        # never measured.
        depth16 = [ln for ln in text.split("\n")
                   if ln.strip().startswith("16 ") and "345.0" in ln]
        self.assertTrue(depth16, "depth 16 row missing from the table")

    def test_cold_rows_drop_the_iteration_suffix_and_keep_stddev(self):
        self.assertEqual(list(self.summary["cold"]), ["NNIntGet"])
        self.assertEqual(self.summary["cold"]["NNIntGet"], (25000.0, 12.5))

    def test_no_storage_rows_means_none(self):
        run = {"benchmarks": [row("entry/GetBlockHeight", 70.0, gas=51)]}
        self.assertIsNone(report.storage_summary(report.rows(run),
                                                 report.stddevs(run)))


class StorageScheduleTest(unittest.TestCase):
    def test_proposal_carries_storage_gas(self):
        proposal = report.build_schedule(synthetic_run(),
                                         synthetic_calibration(1.6),
                                         resolution=1)
        # The first touch is charged as its surcharge over the warm call:
        # ceil((120 - 110) / 1.6) = 7, not ceil(120 / 1.6) = 75.
        self.assertEqual(proposal["storage_first_touch_gas"], 7)
        self.assertEqual(proposal["storage_first_touch_from"], "NNIntGet")
        # ceil(40 / 1.6) = 25.
        self.assertEqual(proposal["storage_commit_item_gas"], 25)

    def test_parent_walk_is_measured_but_not_proposed(self):
        proposal = report.build_schedule(synthetic_run(),
                                         synthetic_calibration(1.6),
                                         resolution=1)
        # Reported as nanoseconds, with what it leaves unpriced at the deepest
        # depth, and never as a constant: a gas-schedule entry is hashed into
        # the digest, so one is not added for a term this small.
        self.assertNotIn("storage_parent_frame_gas", proposal)
        self.assertAlmostEqual(proposal["storage_parent_frame_ns"], 15.0,
                               places=6)
        # 15 ns/frame over 254 frames at a 1.6 ns unit.
        self.assertAlmostEqual(proposal["storage_parent_frame_unpriced_gas"],
                               15.0 * 254 / 1.6, places=4)
        out = io.StringIO()
        with redirect_stdout(out):
            report.print_schedule(synthetic_run(), synthetic_calibration(1.6),
                                  resolution=1)
        text = out.getvalue()
        self.assertNotIn("kStorageParentFrameCost", text)
        self.assertIn("parent walk is not proposed", text)
        # Still measured and shown in the tables.
        self.assertIn("parent walk: a first touch costs", text)

    def test_cold_row_never_reaches_the_syscall_table(self):
        proposal = report.build_schedule(synthetic_run(),
                                         synthetic_calibration(1.6),
                                         resolution=1)
        self.assertNotIn("NNIntGet/cold", proposal["syscalls"])
        self.assertNotIn("cold/NNIntGet", proposal["syscalls"])
        self.assertTrue(all("cold" not in n for n in proposal["heavy"]))

    def test_printers_run(self):
        out = io.StringIO()
        with redirect_stdout(out):
            status = report.print_table(synthetic_run())
        self.assertEqual(status, 0)
        text = out.getvalue()
        self.assertIn("parent walk", text)
        self.assertIn("cold (mock db)", text)
        self.assertNotIn("<- NOT FLAT", text)
        out = io.StringIO()
        with redirect_stdout(out):
            status = report.print_schedule(synthetic_run(),
                                           synthetic_calibration(1.6),
                                           resolution=1)
        self.assertEqual(status, 0)
        self.assertIn("kStorageFirstTouchCost = 7;", out.getvalue())
        self.assertIn("surcharge over a warm NNIntGet", out.getvalue())

    def test_not_flat_is_flagged(self):
        run = synthetic_run()
        for entry in run["benchmarks"]:
            if entry["name"] == "frame/NNIntSetAddFirstTouch/1/4096":
                entry["real_time"] *= 2.0
        out = io.StringIO()
        with redirect_stdout(out):
            report.print_table(run)
        self.assertIn("<- NOT FLAT", out.getvalue())


def guardable_run(**kwargs):
    """A run the gate can actually guard: several cheap syscalls, and a
    stddev for every row.

    The gate needs both. Ratios are relative to the median of the syscalls
    that only cross the boundary, which needs at least three of them, and a
    row with no stddev cannot be shown to reproduce well enough to guard.
    """
    run = synthetic_run(**kwargs)
    # Three more crossing-only syscalls, close to GetBlockHeight's 70 ns, so a
    # median over them means something.
    for name, cost in (("GetCalldataLen", 72.0), ("GetCurrentAddress", 68.0),
                       ("GetMsgSender", 71.0)):
        run["benchmarks"].append(row(f"entry/{name}", cost, gas=51))
    for entry in list(run["benchmarks"]):
        if entry.get("aggregate_name") != "median":
            continue
        run["benchmarks"].append(
            row(entry["name"], entry["real_time"] * 0.002,
                aggregate="stddev"))
    run["context"] = {"engine_fingerprint": "abc123", "hogd_host": "test"}
    return run


def reference_text(run):
    out = io.StringIO()
    with redirect_stdout(out):
        report.emit_reference(run)
    return out.getvalue()


def write_reference(case, text):
    path = os.path.join(case.tmp, "syscall_reference.txt")
    with open(path, "w") as handle:
        handle.write(text)
    return path


class GateTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.tmp = self.dir.name
        self.addCleanup(self.dir.cleanup)

    def check(self, run, path):
        out = io.StringIO()
        with redirect_stdout(out):
            status = report.check_against_reference(run, path)
        return status, out.getvalue()

    def test_a_run_passes_its_own_reference(self):
        run = guardable_run()
        path = write_reference(self, reference_text(run))
        status, text = self.check(run, path)
        self.assertEqual(status, 0)
        self.assertIn("guard(s) hold", text)

    def test_reference_guards_both_syscalls_and_storage(self):
        text = reference_text(guardable_run())
        self.assertIn("guard syscall/NNIntGet ", text)
        self.assertIn("guard storage/NNIntGetFirstTouch/1/4096 ", text)
        # The cold row is bounded by the benchmark's own mock, so gating it
        # would gate the harness rather than hogd.
        self.assertNotIn("cold", text)

    def test_a_doubled_syscall_fails(self):
        path = write_reference(self, reference_text(guardable_run()))
        run = guardable_run()
        for entry in run["benchmarks"]:
            if entry["name"] == "entry/NNIntGet":
                entry["real_time"] *= 2.5
        status, text = self.check(run, path)
        self.assertEqual(status, 1)
        self.assertIn("syscall/NNIntGet", text)

    def test_a_small_rise_does_not_fire(self):
        """The tolerance is an alarm, not a budget: 20% must not fail."""
        path = write_reference(self, reference_text(guardable_run()))
        run = guardable_run()
        for entry in run["benchmarks"]:
            if entry["name"] == "entry/NNIntGet":
                entry["real_time"] *= 1.2
        status, _ = self.check(run, path)
        self.assertEqual(status, 0)

    def test_a_missing_row_fails_rather_than_passing(self):
        """A guard that was not measured was not checked."""
        path = write_reference(self, reference_text(guardable_run()))
        run = guardable_run()
        run["benchmarks"] = [e for e in run["benchmarks"]
                             if e["name"] != "entry/NNIntGet"]
        status, text = self.check(run, path)
        self.assertEqual(status, 1)
        self.assertIn("not measured in this run", text)

    def test_a_noisy_row_is_not_guarded(self):
        run = guardable_run()
        for entry in run["benchmarks"]:
            if (entry["name"] == "entry/NNIntGet"
                    and entry.get("aggregate_name") == "stddev"):
                entry["real_time"] *= 100
        self.assertNotIn("guard syscall/NNIntGet ", reference_text(run))

    def test_an_ill_conditioned_cell_is_not_guarded(self):
        """A cell whose frames dominate its own measurement.

        At depth 254 with one key the per-key cost is the gap between two
        numbers that are almost equal, so its error is whatever theirs is,
        magnified. Guarding it gates on that subtraction rather than on the
        touch.
        """
        text = reference_text(guardable_run())
        self.assertNotIn("guard storage/NNIntGetFirstTouch/254/1 ", text)
        self.assertIn("guard storage/NNIntGetFirstTouch/254/4096 ", text)

    def test_a_moved_engine_fingerprint_fails(self):
        path = write_reference(self, reference_text(guardable_run()))
        run = guardable_run()
        run["context"]["engine_fingerprint"] = "moved99"
        status, text = self.check(run, path)
        self.assertEqual(status, 1)
        self.assertIn("engine fingerprint", text)

    def test_too_few_syscalls_gives_no_baseline(self):
        """One row carrying every ratio is one row whose noise is the gate."""
        run = guardable_run()
        keep = ("entry/GetBlockHeight", "entry/NNIntGet")
        run["benchmarks"] = [e for e in run["benchmarks"]
                             if not e["name"].startswith("entry/")
                             or e["name"] in keep]
        baseline, guards = report.guardable(report.rows(run),
                                            report.stddevs(run))
        self.assertIsNone(baseline)
        self.assertEqual(guards, {})

    def test_an_anomalously_cheap_row_does_not_move_the_baseline(self):
        """The anchor is a median of the cheapest few, not the single
        cheapest, so one fast outlier cannot exclude the crossing cluster."""
        run = guardable_run()
        with_outlier = guardable_run()
        for entry in with_outlier["benchmarks"]:
            if entry["name"] == "entry/NNIntGetNoLoad":
                entry["real_time"] = 1.0
        plain, _ = report.guardable(report.rows(run), report.stddevs(run))
        skewed, guards = report.guardable(report.rows(with_outlier),
                                          report.stddevs(with_outlier))
        self.assertEqual(plain, skewed)
        self.assertIn("syscall/NNIntGet", guards)


if __name__ == "__main__":
    unittest.main()
