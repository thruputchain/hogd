"""Startup must report a crashed dependency without waiting for readiness."""

from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

from cluster import Cluster


class StartupTest(unittest.TestCase):
    def cluster(self):
        cluster = Cluster.__new__(Cluster)
        cluster.args = SimpleNamespace(local=False, ready_timeout=600)
        cluster.logs = {"event": ("hog9", "/tmp/stdout", "/tmp/stderr")}
        cluster.remote = Mock()
        cluster.address = Mock(return_value="127.0.0.1:9547")
        cluster.check_services = Mock(
            side_effect=RuntimeError("coordinator exited")
        )
        return cluster

    def test_crashed_dependency_interrupts_remote_log_readiness(self):
        cluster = self.cluster()
        with patch("cluster.time.monotonic", side_effect=[0, 0, 0, 601]):
            with self.assertRaisesRegex(RuntimeError, "coordinator exited"):
                cluster.wait_log("event", "event initialization complete")
        cluster.remote.assert_not_called()

    def test_crashed_dependency_interrupts_remote_gateway_readiness(self):
        cluster = self.cluster()
        with patch("cluster.time.monotonic", side_effect=[0, 0, 0, 601]):
            with self.assertRaisesRegex(RuntimeError, "coordinator exited"):
                cluster.wait_gateway()


class ReplayShutdownTest(unittest.TestCase):
    def test_interrupted_remote_replay_flushes_before_copy(self):
        for failure in ("service", "deadline", "hung"):
            with self.subTest(
                failure=failure
            ), tempfile.TemporaryDirectory() as name:
                directory = Path(name)
                corpus = directory / "corpus"
                corpus.mkdir()
                (corpus / "manifest.json").write_text('{"durationSeconds": 1}')
                cluster = Cluster.__new__(Cluster)
                cluster.args = SimpleNamespace(
                    local=False,
                    build=directory,
                    corpus=corpus,
                    drain=1,
                    outstanding=1,
                    receipt_workers=1,
                    ready_timeout=1,
                )
                cluster.directory, cluster.control, cluster.port = (
                    directory,
                    directory / "control",
                    "1234",
                )
                cluster.hosts = {"replay": "hog7"}
                cluster.address = Mock(return_value="127.0.0.1:9546")
                cluster.start = Mock()
                cluster.check_services = Mock(
                    side_effect=(
                        RuntimeError("service exited")
                        if failure != "deadline"
                        else None
                    )
                )
                events = []
                cluster.signal_remote_groups = Mock(
                    side_effect=lambda host, sig, **kwargs: events.append(sig)
                )
                waiter = Mock()
                waiter.poll.return_value = None

                def wait(timeout):
                    events.append(f"wait:{timeout}")
                    if (failure == "deadline" and timeout == 5) or (
                        failure == "hung" and timeout == 30
                    ):
                        raise subprocess.TimeoutExpired("wait", timeout)
                    waiter.poll.return_value = 1
                    return 1

                waiter.wait.side_effect = wait
                with patch(
                    "cluster.subprocess.Popen", return_value=waiter
                ), patch(
                    "cluster.subprocess.run",
                    side_effect=lambda *args, **kwargs: events.append("copy"),
                ), patch(
                    "cluster.time.monotonic", side_effect=[0, 10000]
                ):
                    with self.assertRaisesRegex(
                        RuntimeError,
                        (
                            "deadline exceeded"
                            if failure == "deadline"
                            else "service exited"
                        ),
                    ):
                        cluster.replay()
                expected = (["wait:5"] if failure == "deadline" else []) + [
                    "TERM",
                    "wait:30",
                ]
                if failure == "hung":
                    expected += ["KILL", "wait:10"]
                self.assertEqual(events, expected + ["copy"])
                waiter.terminate.assert_not_called()
                cluster.signal_remote_groups.assert_any_call(
                    "hog7", "TERM", names=("replay",)
                )


if __name__ == "__main__":
    unittest.main()
