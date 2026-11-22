#!/usr/bin/env python3
"""Compile distinct RV32 artifacts outside the source tree."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

BENCHMARK_DIR = Path(__file__).resolve().parent
ROOT = BENCHMARK_DIR.parents[2]


def build(output, apps, wallets):
    output = output.resolve()
    if apps < 5 or wallets < 1 or wallets > 5:
        raise ValueError(
            "need at least five apps and one to five wallet variants"
        )
    output.mkdir(parents=True, exist_ok=True)
    project = output / "project"
    project.mkdir(exist_ok=True)
    manifest = (ROOT / "built_contracts/Cargo.toml").read_text()
    manifest = (
        manifest[: manifest.index("members = [")]
        + 'members = ["app", "wallet"]\n\n'
        + manifest[manifest.index("[workspace.package]"):]
    )
    (project / "Cargo.toml").write_text(manifest)
    for name in ("Cargo.lock", "rust-toolchain.toml", "thruput.toml"):
        shutil.copyfile(ROOT / "built_contracts" / name, project / name)
    shutil.copytree(
        ROOT / "built_contracts/.cargo", project / ".cargo", dirs_exist_ok=True
    )
    for kind in ("app", "wallet"):
        crate = project / kind
        (crate / "src").mkdir(parents=True, exist_ok=True)
        (crate / "Cargo.toml").write_text(f"""[package]
name = "hackathon_{kind}"
version = "0.1.0"
edition = "2024"
[dependencies]
thruput-sdk = {{ workspace = true }}
thruput-erc20-if = {{ workspace = true }}
[lib]
crate-type = ["cdylib", "rlib"]
[features]
entrypoints = []
dev = ["thruput-sdk/dev"]
[[bin]]
name = "hackathon_{kind}"
path = "src/main.rs"
required-features = ["entrypoints"]
""")
        (crate / "src/main.rs").write_text(
            f"#![no_std]\n#![no_main]\nextern crate hackathon_{kind};\n"
        )
        shutil.copyfile(
            BENCHMARK_DIR / f"contracts/{kind}/src/lib.rs",
            crate / "src/lib.rs",
        )
    subprocess.run(
        ["cargo", "metadata", "--offline", "--format-version=1"],
        cwd=project,
        stdout=subprocess.DEVNULL,
        check=True,
    )
    version = subprocess.check_output(
        ["thruput", "--version"], text=True
    ).strip()
    expected = "thruput " + (
        ROOT / "scripts/thruput-tools-version"
    ).read_text().strip()
    if version.split(" (", 1)[0] != expected:
        raise ValueError(f"expected {expected}, got {version}")
    records = []
    for kind, count in (("app", apps), ("wallet", wallets)):
        template = (BENCHMARK_DIR / f"contracts/{kind}/src/lib.rs").read_text()
        for index in range(count):
            source = template.replace(
                "const VARIANT: u64 = 1;", f"const VARIANT: u64 = {index + 1};"
            )
            source = source.replace(
                "const FAMILY: u64 = 0;", f"const FAMILY: u64 = {index % 5};"
            )
            (project / kind / "src/lib.rs").write_text(source)
            result = subprocess.run(
                [
                    "thruput",
                    "build",
                    "-p",
                    f"hackathon_{kind}",
                    "--json",
                    "--target-dir",
                    str(output / "target"),
                    "--",
                    "--frozen",
                    "--offline",
                ],
                cwd=project,
                text=True,
                stdout=subprocess.PIPE,
                check=True,
            )
            data = json.loads(result.stdout)["data"]
            artifact = output / f"{kind}_{index}.rv32a"
            shutil.copyfile(data["rv32"], artifact)
            shutil.copyfile(
                data["specJson"], output / f"{kind}_{index}_spec.json"
            )
            if index == 0:
                subprocess.run(
                    [
                        "python3",
                        str(ROOT / "built_contracts/generate_header.py"),
                        data["specJson"],
                        "-o",
                        str(output / f"hackathon_{kind}.h"),
                        "-n",
                        f"hackathon_{kind}",
                        "-r",
                        artifact.name,
                    ],
                    check=True,
                )
            records.append(
                {
                    "kind": kind,
                    "index": index,
                    "family": index % 5 if kind == "app" else None,
                    "sourceSha256": hashlib.sha256(
                        source.encode()
                    ).hexdigest(),
                    "artifact": artifact.name,
                    "sha256": hashlib.sha256(
                        artifact.read_bytes()
                    ).hexdigest(),
                }
            )
    if len({x["sha256"] for x in records}) != len(records):
        raise ValueError("compiler produced duplicate artifacts")
    (output / "artifacts.json").write_text(
        json.dumps(
            {"schemaVersion": 1, "compiler": version, "artifacts": records},
            indent=2,
        )
        + "\n"
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--apps", type=int, default=500)
    parser.add_argument("--wallets", type=int, default=5)
    args = parser.parse_args()
    build(args.output, args.apps, args.wallets)
