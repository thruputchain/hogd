"""Startup must report a crashed dependency without waiting for readiness."""
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


if __name__ == "__main__":
    unittest.main()
