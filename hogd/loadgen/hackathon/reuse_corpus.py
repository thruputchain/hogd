"""Restore retained signed corpora for a compatible capacity experiment."""

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[3]
CORPUS_FILES = {"genesis.xdr", "artifacts.json", "transactions.jsonl", "manifest.json"}
HARNESS_CHANGES = {
    ".github/workflows/hackathon.yml",
    "README-ci.md",
    "scripts/ci/hackathon_bench.sh",
    "scripts/schedules/hackathon.toml",
    "hogd/loadgen/hackathon/cluster.py",
    "hogd/loadgen/hackathon/reuse_corpus.py",
    "hogd/loadgen/hackathon/test_reuse_corpus.py",
}


def validate_source(revision, protocol, root, retained_patch=b""):
    if not re.fullmatch(r"[0-9a-f]{40}", revision):
        raise ValueError("invalid retained source revision")
    available = subprocess.run(
        ["git", "cat-file", "-e", revision + "^{commit}"],
        cwd=root,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if available.returncode:
        subprocess.run(
            ["git", "fetch", "--quiet", "--depth", "1", "origin", revision],
            cwd=root,
            check=True,
        )
    changed = set(
        subprocess.check_output(
            ["git", "diff", "--name-only", revision, "HEAD"],
            cwd=root,
            text=True,
        ).splitlines()
    )
    if changed - HARNESS_CHANGES:
        raise ValueError(
            "retained corpus requires unchanged validator and fixture sources: "
            + ", ".join(sorted(changed - HARNESS_CHANGES))
        )
    # prepare_env removes this registry configuration in normal CI. Accept
    # precisely the same generated configuration change, never dirty runtime
    # code or a dirty submodule whose contents a root diff cannot describe.
    build_config = ".cargo/config.toml"
    dirty = set(
        subprocess.check_output(
            ["git", "diff", "--name-only", "HEAD"],
            cwd=root,
            text=True,
        ).splitlines()
    )
    if dirty - {build_config}:
        raise ValueError("working validator and fixture sources must be clean")
    pairs = re.findall(rb"^diff --git a/(\S+) b/(\S+)$", retained_patch, re.MULTILINE)
    if retained_patch and (
        not pairs or any(a != b or a.decode() != build_config for a, b in pairs)
    ):
        raise ValueError("retained working changes are not the CI build configuration")
    actual_patch = subprocess.check_output(
        ["git", "diff", "HEAD", "--", build_config],
        cwd=root,
    )
    if retained_patch != actual_patch:
        raise ValueError("retained CI build configuration differs from this run")
    actual = subprocess.check_output(
        ["git", "rev-parse", "HEAD"],
        cwd=root / "protocol",
        text=True,
    ).strip()
    if protocol != actual:
        raise ValueError("retained corpus protocol does not match this checkout")


def restore(archive, output, users, multiplier, root=ROOT):
    archive, output = Path(archive), Path(output)
    output.mkdir(parents=True, exist_ok=True)
    for shared in (0, 1):
        if (output / f"hackathon-corpus-{shared}").exists():
            raise ValueError("corpus output must be fresh")
    identities, patches, gateway = {}, {}, None
    with tempfile.TemporaryDirectory(prefix="restore-corpus-", dir=output) as temporary:
        staging = Path(temporary)
        with tarfile.open(archive, "r|gz") as source:
            for member in source:
                path = PurePosixPath(member.name)
                if path.as_posix() == "hackathon-results/gateway-build.json":
                    if not member.isfile() or member.size > 1024 * 1024:
                        raise ValueError("invalid retained gateway metadata")
                    gateway = json.load(source.extractfile(member))
                elif path.as_posix() in {
                    f"hackathon-results/code-{shared}/identity.json"
                    for shared in (0, 1)
                }:
                    if not member.isfile() or member.size > 1024 * 1024:
                        raise ValueError("invalid retained identity")
                    identities[int(path.parts[1][-1])] = json.load(
                        source.extractfile(member)
                    )
                elif path.as_posix() in {
                    f"hackathon-results/code-{shared}/hogd.patch" for shared in (0, 1)
                }:
                    if not member.isfile() or member.size > 1024 * 1024:
                        raise ValueError("invalid retained source patch")
                    patches[int(path.parts[1][-1])] = source.extractfile(member).read()
                elif path.parts and path.parts[0] in {
                    "hackathon-corpus-0",
                    "hackathon-corpus-1",
                }:
                    if len(path.parts) == 1 and member.isdir():
                        continue
                    if (
                        len(path.parts) != 2
                        or path.name not in CORPUS_FILES
                        or not member.isfile()
                    ):
                        raise ValueError(
                            "unexpected retained corpus member: " + member.name
                        )
                    target = staging / path
                    if target.exists():
                        raise ValueError("duplicate retained corpus member")
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with source.extractfile(member) as data, target.open(
                        "wb"
                    ) as destination:
                        shutil.copyfileobj(data, destination)
        if set(identities) != {0, 1} or set(patches) != {0, 1} or gateway is None:
            raise ValueError(
                "retained archive needs both workload identities and gateway metadata"
            )
        revisions = {identity["hogdRevision"] for identity in identities.values()}
        if len(revisions) != 1:
            raise ValueError("retained workload source revisions differ")
        if patches[0] != patches[1]:
            raise ValueError("retained workload source patches differ")
        validate_source(revisions.pop(), gateway["protocolRevision"], root, patches[0])
        manifests = {}
        for shared in (0, 1):
            corpus = staging / f"hackathon-corpus-{shared}"
            if (
                not corpus.is_dir()
                or {path.name for path in corpus.iterdir()} != CORPUS_FILES
            ):
                raise ValueError("retained corpus is incomplete")
            manifest = json.loads((corpus / "manifest.json").read_text())
            expected = {
                "schemaVersion": 1,
                "seed": 1,
                "users": users,
                "apps": 500,
                "predeployed": 400,
                "walletVariants": 5,
                "sharedCode": bool(shared),
                "warmupSeconds": 30,
                "stageSeconds": 60,
                "durationSeconds": 270,
                "rateDivisor": 1,
                "rateMultiplier": multiplier,
            }
            if any(manifest.get(key) != value for key, value in expected.items()):
                raise ValueError(
                    "retained corpus parameters differ from this experiment"
                )
            with (corpus / "transactions.jsonl").open("rb") as transactions:
                digest = hashlib.file_digest(transactions, "sha256").hexdigest()
            if digest != identities[shared]["corpusSha256"]:
                raise ValueError(
                    "retained signed transactions failed checksum validation"
                )
            manifests[str(shared)] = {key: manifest[key] for key in expected}
        with archive.open("rb") as data:
            digest = hashlib.file_digest(data, "sha256").hexdigest()
        metadata = {
            "archiveSha256": digest,
            "identities": identities,
            "parameters": manifests,
            "buildConfigPatchSha256": hashlib.sha256(patches[0]).hexdigest(),
        }
        for shared in (0, 1):
            name = f"hackathon-corpus-{shared}"
            (staging / name).rename(output / name)
        results = output / "hackathon-results"
        results.mkdir(exist_ok=True)
        (results / "corpus-source.json").write_text(
            json.dumps(metadata, indent=2) + "\n"
        )
        print("Restored both validated signed corpora from", archive)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--users", type=int, required=True)
    parser.add_argument("--rate-multiplier", type=int, required=True)
    args = parser.parse_args()
    restore(args.archive, args.output, args.users, args.rate_multiplier)
