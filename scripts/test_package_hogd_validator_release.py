import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).with_name("package_hogd_validator_release.sh")


class PackageValidatorTest(unittest.TestCase):
    def test_server_archive_does_not_publish_a_tuned_devnode_distribution(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build = root / "build"
            for directory, binary in (("hog_db", "hog-db"), ("hog_exec", "hog-exec"),
                                      ("hog_mempool", "hog-mempool"), ("hog_auth", "hog-auth"),
                                      ("hog_event", "hog-event"), ("hog_indexer", "hog-indexer"),
                                      ("devnode", "hog-devnode"), ("devnode", "hog-hosted-bootstrap")):
                path = build / "hogd" / directory / binary
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("fixture")
                path.chmod(0o755)
            contracts = build / "built_contracts"
            contracts.mkdir()
            for name in ("genesis_root.rv32a",):
                (contracts / name).write_text("contract")
            output = root / "assets"
            subprocess.run(["bash", str(SCRIPT), str(build), str(output), "release", "x86_64", "znver5"], check=True)
            self.assertEqual([p.name for p in output.iterdir()], ["hogd-validator-linux-x86_64-znver5.tar.gz"])
            with tarfile.open(next(output.iterdir())) as archive:
                self.assertIn("hog-db", archive.getnames())
                self.assertIn("contracts/genesis_root.rv32a", archive.getnames())
                self.assertNotIn("contracts/dev_wallet.rv32a", archive.getnames())
            subprocess.run(["bash", str(SCRIPT), str(build), str(output), "release", "x86_64"], check=True)
            self.assertTrue((output / "hog-devnode-linux-x86_64.tar.gz").is_file())
            self.assertTrue((output / "hogd-validator-linux-x86_64.tar.gz").is_file())
            with tarfile.open(output / "hog-devnode-linux-x86_64.tar.gz") as archive:
                self.assertIn("contracts/genesis_root.rv32a", archive.getnames())
                self.assertNotIn("contracts/dev_wallet.rv32a", archive.getnames())

    def test_unsupported_profiles_fail_before_packaging(self):
        for flavor, architecture, cpu in (("release", "arm64", "znver5"),
                                          ("debug", "x86_64", "znver5"),
                                          ("release", "x86_64", "native")):
            result = subprocess.run(["bash", str(SCRIPT), "/missing", "/missing", flavor, architecture, cpu], capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"unsupported CPU profile", result.stderr)


if __name__ == "__main__":
    unittest.main()
