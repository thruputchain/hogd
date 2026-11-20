import hashlib
import json
import os
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).with_name("package_hogd_validator_release.sh")
BINARIES = (("hog_db", "hog-db"), ("hog_exec", "hog-exec"),
            ("hog_mempool", "hog-mempool"), ("hog_auth", "hog-auth"),
            ("hog_event", "hog-event"), ("hog_indexer", "hog-indexer"),
            ("devnode", "hog-devnode"), ("devnode", "hog-hosted-bootstrap"))
CONTRACTS = ["contracts/genesis_root.rv32a"]

# Text fixtures model ELF sections; these tools reject incorrect objcopy flags.
MOCK_TOOL = '''#!/usr/bin/env python3
import json
import sys
from pathlib import Path

args = sys.argv[1:]
if Path(sys.argv[0]).name == "mock-readelf":
    assert args[0] == "-n"
    binary = json.loads(Path(args[1]).read_text())
    if binary["build_id"]:
        print("    Build ID: " + binary["build_id"])
elif args[0] == "--only-keep-debug":
    assert len(args) == 3
    source = json.loads(Path(args[1]).read_text())
    assert ".debug_info" in source["sections"]
    Path(args[2]).write_text(json.dumps(source))
else:
    assert len(args) == 3 and args[0] == "--strip-debug"
    assert args[1].startswith("--add-gnu-debuglink=")
    debug = Path(args[1].split("=", 1)[1])
    assert debug.is_file()
    path = Path(args[2])
    binary = json.loads(path.read_text())
    binary["sections"].remove(".debug_info")
    binary["sections"].append(".gnu_debuglink")
    binary["debuglink"] = debug.name
    path.write_text(json.dumps(binary))
'''


class PackageValidatorTest(unittest.TestCase):
    def fixtures(self, root, missing_build_id=False):
        build = root / "build"
        for directory, binary in BINARIES:
            path = build / "hogd" / directory / binary
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(json.dumps({
                "build_id": "" if missing_build_id else hashlib.sha1(binary.encode()).hexdigest(),
                "sections": [".symtab", ".debug_info"],
            }))
            path.chmod(0o755)
        contracts = build / "built_contracts"
        contracts.mkdir()
        for name in CONTRACTS:
            (build / "built_contracts" / Path(name).name).write_text("contract")
        env = os.environ.copy()
        # Keep macOS tar's AppleDouble metadata out of these Linux fixtures.
        env["COPYFILE_DISABLE"] = "1"
        for variable, name in (("OBJCOPY", "mock-objcopy"), ("READELF", "mock-readelf")):
            tool = root / name
            tool.write_text(MOCK_TOOL)
            tool.chmod(0o755)
            env[variable] = str(tool)
        return build, env

    def check_archives(self, output, package, binaries):
        with tarfile.open(output / f"{package}.tar.gz") as archive:
            self.assertEqual(sorted(archive.getnames()), sorted(CONTRACTS + binaries))
            for name in binaries:
                binary = json.load(archive.extractfile(name))
                self.assertIn(".symtab", binary["sections"])
                self.assertNotIn(".debug_info", binary["sections"])
                self.assertIn(".gnu_debuglink", binary["sections"])
                self.assertEqual(binary["debuglink"], f"{name}.debug")
        with tarfile.open(output / f"{package}-debuginfo.tar.gz") as archive:
            files = [member.name for member in archive.getmembers() if member.isfile()]
            expected = []
            for name in binaries:
                build_id = hashlib.sha1(name.encode()).hexdigest()
                member = f"usr/lib/debug/.build-id/{build_id[:2]}/{build_id[2:]}.debug"
                expected.append(member)
                debug = json.load(archive.extractfile(member))
                self.assertIn(".debug_info", debug["sections"])
                self.assertEqual(debug["build_id"], build_id)
            self.assertEqual(sorted(files), sorted(expected))

    def test_server_archive_does_not_publish_a_tuned_devnode_distribution(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build, env = self.fixtures(root)
            output = root / "assets"
            subprocess.run(["bash", str(SCRIPT), str(build), str(output), "release", "x86_64", "znver5"], check=True, env=env)
            package = "hogd-validator-linux-x86_64-znver5"
            self.assertEqual(sorted(p.name for p in output.iterdir()),
                             sorted([f"{package}.tar.gz", f"{package}-debuginfo.tar.gz"]))
            self.check_archives(output, package, [binary for _, binary in BINARIES])

    def test_portable_archives(self):
        for flavor in ("release", "debug"):
            for architecture in ("x86_64", "arm64"):
                with self.subTest(flavor=flavor, architecture=architecture), tempfile.TemporaryDirectory() as tmp:
                    root = Path(tmp)
                    build, env = self.fixtures(root)
                    output = root / "assets"
                    subprocess.run(["bash", str(SCRIPT), str(build), str(output), flavor, architecture], check=True, env=env)
                    suffix = f"{'debug-' if flavor == 'debug' else ''}linux-{architecture}"
                    self.assertEqual(len(list(output.iterdir())), 4)
                    self.check_archives(output, f"hogd-validator-{suffix}", [binary for _, binary in BINARIES])
                    self.check_archives(output, f"hog-devnode-{suffix}", ["hog-devnode"])

    def test_missing_build_id_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build, env = self.fixtures(root, missing_build_id=True)
            output = root / "assets"
            result = subprocess.run(["bash", str(SCRIPT), str(build), str(output)], capture_output=True, env=env)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"missing or invalid GNU build-id", result.stderr)
            self.assertEqual(list(output.iterdir()), [])

    def test_unsupported_profiles_fail_before_packaging(self):
        for flavor, architecture, cpu in (("release", "arm64", "znver5"),
                                          ("debug", "x86_64", "znver5"),
                                          ("release", "x86_64", "native")):
            result = subprocess.run(["bash", str(SCRIPT), "/missing", "/missing", flavor, architecture, cpu], capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"unsupported CPU profile", result.stderr)


if __name__ == "__main__":
    unittest.main()
