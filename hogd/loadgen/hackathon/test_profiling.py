"""Profiling must target only owned processes and retain failures explicitly."""

import json
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import Mock, patch
from profiling import CPUProfiles, PROFILE_SCRIPT


class CPUProfilesTest(unittest.TestCase):
    def test_profile_follows_traffic_clock_after_slow_startup(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            cluster = SimpleNamespace(
                directory=directory, args=SimpleNamespace(local=True)
            )
            profiles = CPUProfiles(cluster)
            profiles.start = Mock()
            # No run.json: loading is still in progress even if process age is large.
            profiles.maybe_start()
            profiles.start.assert_not_called()
            (directory / "replay").mkdir()
            (directory / "replay/run.json").write_text(
                '{"startedUnixNs":1000000000000}'
            )
            with patch("profiling.time.time_ns", return_value=1219000000000):
                profiles.maybe_start()
            profiles.start.assert_not_called()
            with patch("profiling.time.time_ns", return_value=1221000000000):
                profiles.maybe_start()
            profiles.start.assert_called_once()
            self.assertEqual(profiles.trigger_elapsed, 221)

    def test_missed_window_is_explicit_and_never_profiles_drain(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            (directory / "replay").mkdir()
            (directory / "replay/run.json").write_text(
                '{"startedUnixNs":1000000000000}'
            )
            cluster = SimpleNamespace(
                directory=directory, args=SimpleNamespace(local=True)
            )
            profiles = CPUProfiles(cluster)
            profiles.start = Mock()
            with patch("profiling.time.time_ns", return_value=1300000000000):
                profiles.maybe_start()
            profiles.start.assert_not_called()
            self.assertTrue(profiles.started)
            self.assertEqual(
                json.loads((directory / "profiles/status.json").read_text())[
                    "error"
                ],
                "missed peak profiling window",
            )

    def test_invalid_remote_pid_never_starts_profiler(self):
        with tempfile.TemporaryDirectory() as tmp:
            cluster = SimpleNamespace(
                directory=Path(tmp),
                args=SimpleNamespace(local=False),
                hosts={"gateway": "hog4", "mempool": "hog10"},
                remote=Mock(return_value=SimpleNamespace(stdout="0; kill -1")),
            )
            profiles = CPUProfiles(cluster)
            with patch("profiling.subprocess.Popen") as start:
                profiles.start()
                profiles.finish()
            start.assert_not_called()
            for name in cluster.hosts:
                self.assertIn(
                    "Invalid service PID",
                    json.loads(
                        (
                            Path(tmp) / "profiles" / name / "status.json"
                        ).read_text()
                    )["error"],
                )

    def test_local_perf_failure_is_retained_without_killing_services(self):
        with tempfile.TemporaryDirectory() as tmp:
            cluster = SimpleNamespace(
                directory=Path(tmp),
                args=SimpleNamespace(local=True),
                hosts={"gateway": "localhost", "mempool": "localhost"},
                processes={
                    "gateway": SimpleNamespace(pid=123),
                    "mempool": SimpleNamespace(pid=456),
                },
            )
            child = Mock()
            child.wait.return_value = 1
            with patch(
                "profiling.subprocess.Popen", return_value=child
            ) as start:
                profiles = CPUProfiles(cluster)
                profiles.start()
                profiles.finish()
            commands = [call.args[0] for call in start.call_args_list]
            self.assertEqual([cmd[4] for cmd in commands], ["123", "456"])
            self.assertTrue(all(cmd[2] == PROFILE_SCRIPT for cmd in commands))
            child.terminate.assert_not_called()
            for name in cluster.hosts:
                status = json.loads(
                    (Path(tmp) / "profiles" / name / "status.json").read_text()
                )
                self.assertEqual(status["exitStatus"], 1)
                self.assertEqual(status["copyStatus"], 0)


if __name__ == "__main__":
    unittest.main()
