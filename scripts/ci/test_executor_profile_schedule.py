#!/usr/bin/env python3
"""Exercise the real schedule's profiler supervision without remote hosts/perf."""

import os
from pathlib import Path
import subprocess
import tomllib
import unittest


SCHEDULE = Path(__file__).resolve().parents[1] / "schedules/e2e_payment.toml"
SERVER_COMMAND = "./release/hogd/hog_exec/bench/control_server"


class ExecutorProfileScheduleTest(unittest.TestCase):
    def run_schedule(
        self, *, enabled=True, host="hog5", profiler_status=0,
        server_status=0, profiler_delay="0", server_delay="0.05", errexit=True,
    ):
        schedule = tomllib.loads(SCHEDULE.read_text())
        script = schedule["jobs"][0]["script"].split(
            "echo 'run bench server'", 1
        )[1]
        script = script.replace("{{ .executor_profile }}", str(enabled).lower())
        self.assertEqual(script.count(SERVER_COMMAND), 1)
        script = script.replace(SERVER_COMMAND, "mock_control_server")
        mocks = """
hostname() { echo "$TEST_HOST"; }
bash() {
    echo PROFILE_STARTED
    sleep "$TEST_PROFILER_DELAY"
    echo PROFILE_FINISHED
    return "$TEST_PROFILER_STATUS"
}
mock_control_server() {
    sleep "$TEST_SERVER_DELAY"
    return "$TEST_SERVER_STATUS"
}
"""
        env = {
            **os.environ,
            "TEST_HOST": host,
            "TEST_PROFILER_STATUS": str(profiler_status),
            "TEST_SERVER_STATUS": str(server_status),
            "TEST_PROFILER_DELAY": profiler_delay,
            "TEST_SERVER_DELAY": server_delay,
            "CI_CONTROL_PORT": "11111",
        }
        # Avoid an inherited shell variable accidentally enabling a wait.
        env.pop("executor_profiler", None)
        result = subprocess.run(
            ["bash", "-c", ("set -eu\n" if errexit else "set -u\n") + mocks + script],
            env=env, capture_output=True, text=True, timeout=5,
        )
        return result

    def test_profiler_failure_before_server_exit_is_propagated(self):
        for errexit in (True, False):
            with self.subTest(errexit=errexit):
                result = self.run_schedule(profiler_status=17, errexit=errexit)
                self.assertEqual(result.returncode, 17, result.stderr)
                self.assertIn("EXECUTOR_PROFILE_FAILED", result.stderr)

    def test_waits_for_profiler_after_server_exit(self):
        for status in (0, 19):
            with self.subTest(status=status):
                result = self.run_schedule(
                    profiler_delay="0.1", server_delay="0", profiler_status=status,
                )
                self.assertEqual(result.returncode, status, result.stderr)
                self.assertIn("PROFILE_FINISHED", result.stdout)

    def test_successful_profile(self):
        for host in ("hog5", "hog6"):
            with self.subTest(host=host):
                result = self.run_schedule(host=host)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("PROFILE_FINISHED", result.stdout)

    def test_control_server_failure_is_preserved(self):
        for enabled in (True, False):
            with self.subTest(enabled=enabled):
                result = self.run_schedule(
                    enabled=enabled, server_status=23, profiler_status=17,
                )
                self.assertEqual(result.returncode, 23, result.stderr)

    def test_disabled_or_other_host_does_not_start_profiler(self):
        for options in ({"enabled": False}, {"host": "hog4"}):
            with self.subTest(options=options):
                result = self.run_schedule(profiler_status=17, **options)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertNotIn("PROFILE_STARTED", result.stdout)


if __name__ == "__main__":
    unittest.main()
