"""Corpus reuse must preserve both signed inputs and their workload identity."""

import hashlib
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import Mock, patch

import reuse_corpus


class ReuseCorpusTest(unittest.TestCase):
    def archive(self, directory, mismatch=False, corrupt=False, unsafe=False):
        archive = Path(directory) / "retained.tar.gz"
        entries = {
            "hackathon-results/gateway-build.json": json.dumps(
                {
                    "protocolRevision": "b" * 40,
                }
            ).encode()
        }
        for shared in (0, 1):
            transactions = f"signed corpus {shared}\n".encode()
            identity = {
                "hogdRevision": "a" * 40,
                "corpusSha256": hashlib.sha256(transactions).hexdigest(),
            }
            entries[f"hackathon-results/code-{shared}/identity.json"] = json.dumps(
                identity
            ).encode()
            entries[f"hackathon-results/code-{shared}/hogd.patch"] = b""
            manifest = {
                "schemaVersion": 1,
                "seed": 1,
                "users": 1000,
                "apps": 500,
                "predeployed": 400,
                "walletVariants": 5,
                "sharedCode": bool(shared),
                "warmupSeconds": 30,
                "stageSeconds": 60,
                "durationSeconds": 270,
                "rateDivisor": 1,
                "rateMultiplier": 11 if mismatch and shared else 10,
            }
            entries[f"hackathon-corpus-{shared}/manifest.json"] = json.dumps(
                manifest
            ).encode()
            entries[f"hackathon-corpus-{shared}/genesis.xdr"] = b"genesis"
            entries[f"hackathon-corpus-{shared}/artifacts.json"] = b"{}"
            entries[f"hackathon-corpus-{shared}/transactions.jsonl"] = (
                b"damaged" if corrupt and shared else transactions
            )
        if unsafe:
            entries["hackathon-corpus-0/../escape"] = b"outside corpus"
        with tarfile.open(archive, "w:gz") as output:
            for name, data in entries.items():
                info = tarfile.TarInfo(name)
                info.size = len(data)
                output.addfile(info, io.BytesIO(data))
        return archive

    def test_restores_both_variants_with_provenance(self):
        with tempfile.TemporaryDirectory() as directory, patch(
            "reuse_corpus.validate_source"
        ) as validate:
            archive = self.archive(directory)
            output = Path(directory) / "new-run"
            reuse_corpus.restore(archive, output, 1000, 10)
            for shared in (0, 1):
                self.assertEqual(
                    (
                        output / f"hackathon-corpus-{shared}/transactions.jsonl"
                    ).read_text(),
                    f"signed corpus {shared}\n",
                )
            metadata = json.loads(
                (output / "hackathon-results/corpus-source.json").read_text()
            )
            self.assertEqual(
                metadata["archiveSha256"],
                hashlib.sha256(archive.read_bytes()).hexdigest(),
            )
            self.assertEqual(set(metadata["identities"]), {"0", "1"})
            validate.assert_called_once_with("a" * 40, "b" * 40, reuse_corpus.ROOT, b"")

    def test_bad_control_variant_never_installs_either_corpus(self):
        for problem, message in (
            ("mismatch", "parameters differ"),
            ("corrupt", "checksum"),
            ("unsafe", "unexpected retained"),
        ):
            with self.subTest(
                problem=problem
            ), tempfile.TemporaryDirectory() as directory, patch(
                "reuse_corpus.validate_source"
            ):
                archive = self.archive(directory, **{problem: True})
                digest = hashlib.sha256(archive.read_bytes()).hexdigest()
                output = Path(directory) / "new-run"
                with self.assertRaisesRegex(ValueError, message):
                    reuse_corpus.restore(archive, output, 1000, 10)
                for shared in (0, 1):
                    self.assertFalse((output / f"hackathon-corpus-{shared}").exists())
                self.assertEqual(
                    hashlib.sha256(archive.read_bytes()).hexdigest(), digest
                )
                self.assertFalse((output / "escape").exists())

    def test_validator_source_change_rejects_reuse(self):
        with patch(
            "reuse_corpus.subprocess.run", return_value=Mock(returncode=0)
        ), patch(
            "reuse_corpus.subprocess.check_output",
            side_effect=["hogd/hog_mempool/scheduler.cc\n", ""],
        ):
            with self.assertRaisesRegex(ValueError, "unchanged validator"):
                reuse_corpus.validate_source("a" * 40, "b" * 40, Path("/checkout"))

    def test_harness_only_change_preserves_protocol_check(self):
        for protocol in ("b" * 40, "c" * 40):
            with self.subTest(protocol=protocol), patch(
                "reuse_corpus.subprocess.run", return_value=Mock(returncode=0)
            ), patch(
                "reuse_corpus.subprocess.check_output",
                side_effect=[
                    "hogd/loadgen/hackathon/cluster.py\n",
                    "",
                    b"",
                    protocol + "\n",
                ],
            ):
                if protocol == "b" * 40:
                    reuse_corpus.validate_source("a" * 40, "b" * 40, Path("/checkout"))
                else:
                    with self.assertRaisesRegex(ValueError, "protocol does not match"):
                        reuse_corpus.validate_source(
                            "a" * 40, "b" * 40, Path("/checkout")
                        )

    def test_ci_registry_patch_must_match_exactly(self):
        registry_patch = b"diff --git a/.cargo/config.toml b/.cargo/config.toml\ndeleted file mode 100644\n"
        for current in (registry_patch, b""):
            with self.subTest(current=current), patch(
                "reuse_corpus.subprocess.run", return_value=Mock(returncode=0)
            ), patch(
                "reuse_corpus.subprocess.check_output",
                side_effect=["", ".cargo/config.toml\n", current, "b" * 40 + "\n"],
            ):
                if current == registry_patch:
                    reuse_corpus.validate_source(
                        "a" * 40, "b" * 40, Path("/checkout"), registry_patch
                    )
                else:
                    with self.assertRaisesRegex(
                        ValueError, "build configuration differs"
                    ):
                        reuse_corpus.validate_source(
                            "a" * 40, "b" * 40, Path("/checkout"), registry_patch
                        )


if __name__ == "__main__":
    unittest.main()
