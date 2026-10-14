"""Startup must report a crashed dependency without waiting for readiness."""
import os
from pathlib import Path
import pty
import select
import shlex
import signal
import sys
import tempfile
import time
import tomllib
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

from cluster import Cluster, ROOT


class StartupTest(unittest.TestCase):
    def cluster(self):
        cluster = Cluster.__new__(Cluster)
        cluster.args = SimpleNamespace(local=False, ready_timeout=600)
        cluster.logs = {"event": ("hog9", "/tmp/stdout", "/tmp/stderr")}
        cluster.remote = Mock()
        cluster.address = Mock(return_value="127.0.0.1:9547")
        cluster.check_services = Mock(side_effect=RuntimeError("coordinator exited"))
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


class ScheduleStdinTest(unittest.TestCase):
    def test_benchmark_cannot_consume_scheduler_exit_command(self):
        schedule = tomllib.loads((ROOT / "scripts/schedules/hackathon.toml").read_text())
        script = next(job["script"] for job in schedule["jobs"] if "file_out" in job)
        invocation = next(line for line in script.splitlines() if line.startswith("./scripts/ci/hackathon_bench.sh"))
        with tempfile.TemporaryDirectory() as directory:
            consumer = Path(directory) / "consume.py"
            # Model SSH forwarding stdin while hog_sched feeds bash -s through
            # a PTY. Without input isolation it steals the trailing exit line.
            consumer.write_text("import os, select\nwhile select.select([0], [], [], 0.2)[0]:\n if not os.read(0, 4096): break\n")
            command = invocation.replace("./scripts/ci/hackathon_bench.sh", shlex.join([sys.executable, str(consumer)]))
            pid, terminal = pty.fork()
            if pid == 0:
                os.execv("/bin/bash", ["bash", "--noprofile", "--norc", "-s"])
            completed, output = False, b""
            try:
                os.write(terminal, f"stty -echo\nPS1=\n{command}\nprintf 'EXIT_MARKER_REACHED\\n'\nexit 0\n".encode())
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    if select.select([terminal], [], [], 0.05)[0]:
                        try:
                            output += os.read(terminal, 65536)
                        except OSError:
                            pass
                    waited, status = os.waitpid(pid, os.WNOHANG)
                    if waited:
                        completed = True
                        self.assertEqual(os.waitstatus_to_exitcode(status), 0)
                        break
                self.assertTrue(completed, "benchmark consumed the scheduler's exit command")
                self.assertIn(b"\r\nEXIT_MARKER_REACHED\r\n", output)
            finally:
                if not completed:
                    os.kill(pid, signal.SIGKILL)
                    os.waitpid(pid, 0)
                os.close(terminal)


if __name__ == "__main__":
    unittest.main()
