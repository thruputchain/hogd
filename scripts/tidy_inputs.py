#!/usr/bin/env python3

import argparse
import json
import re
import shlex
import subprocess
import sys
from collections import defaultdict
from pathlib import Path


EE_EXEMPT_RE = re.compile(
    r"_tests?\.(cc|h)$"
    r"|/contract_tests/"
    r"|/direct_rpc_tests/"
    r"|/test_util/"
    r"|/bench/"
    r"|/loadgen/"
    r"|test_framework"
    r"|/hogdb/hogdb\.cc$"
)


def repo_path(path: Path, root: Path) -> str:
    return path.resolve().relative_to(root).as_posix()


def is_hogd_cpp(path: str) -> bool:
    return path.startswith("hogd/") and (path.endswith(".cc") or path.endswith(".h"))


def partition_matches(path: str, partition: str) -> bool:
    if partition == "all":
        return True
    exempt = EE_EXEMPT_RE.search(path) is not None
    if partition == "prod":
        return not exempt
    if partition == "exempt":
        return exempt
    raise ValueError(f"unsupported partition: {partition}")


def all_files(root: Path, suffix: str) -> set[str]:
    return {
        repo_path(path, root)
        for path in (root / "hogd").rglob(f"*{suffix}")
        if path.is_file()
    }


def compile_entries(compile_db: Path, root: Path) -> list[dict]:
    with compile_db.open(encoding="utf-8") as file:
        entries = json.load(file)
    result = []
    for entry in entries:
        file_path = Path(entry["file"])
        if not file_path.is_absolute():
            file_path = Path(entry.get("directory", root)) / file_path
        try:
            rel = repo_path(file_path, root)
        except ValueError:
            continue
        if rel.startswith("hogd/") and rel.endswith(".cc") and file_path.exists():
            result.append(entry)
    return result


def command_argv(entry: dict) -> list[str]:
    if "arguments" in entry:
        return list(entry["arguments"])
    return shlex.split(entry["command"])


def dependency_command(entry: dict) -> list[str]:
    argv = command_argv(entry)
    cleaned = []
    skip_next = False
    skip_with_arg = {
        "-o",
        "-MF",
        "-MT",
        "-MQ",
        "-MJ",
        "-Xclang",
        "-serialize-diagnostics",
    }
    skip_prefixes = ("-o", "-MF", "-MT", "-MQ", "-MJ")
    for arg in argv:
        if skip_next:
            skip_next = False
            continue
        if arg == "-c":
            continue
        if arg in skip_with_arg:
            skip_next = True
            continue
        if any(arg.startswith(prefix) and arg != prefix for prefix in skip_prefixes):
            continue
        cleaned.append(arg)
    return cleaned + ["-MM", "-MG", "-MT", "__tidy_deps__"]


def parse_make_deps(output: str, root: Path) -> set[str]:
    logical = output.replace("\\\n", " ")
    if ":" in logical:
        logical = logical.split(":", 1)[1]
    deps = set()
    for token in shlex.split(logical):
        path = Path(token)
        if not path.is_absolute():
            path = root / path
        try:
            rel = repo_path(path, root)
        except ValueError:
            continue
        if rel.startswith("hogd/") and rel.endswith(".h"):
            deps.add(rel)
    return deps


def dependency_graph(compile_db: Path, root: Path) -> tuple[set[str], dict[str, set[str]]]:
    sources = set()
    header_to_sources: dict[str, set[str]] = defaultdict(set)
    errors = []
    for entry in compile_entries(compile_db, root):
        file_path = Path(entry["file"])
        if not file_path.is_absolute():
            file_path = Path(entry.get("directory", root)) / file_path
        source = repo_path(file_path, root)
        sources.add(source)
        proc = subprocess.run(
            dependency_command(entry),
            cwd=entry.get("directory", root),
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if proc.returncode != 0:
            errors.append(f"{source}:\n{proc.stderr.strip()}")
            continue
        for header in parse_make_deps(proc.stdout, root):
            header_to_sources[header].add(source)
    if errors:
        print("failed to compute dependencies for clang-tidy inputs", file=sys.stderr)
        print("\n\n".join(errors), file=sys.stderr)
        sys.exit(1)
    return sources, header_to_sources


def changed_files(root: Path, base: str, head: str) -> set[str]:
    proc = subprocess.run(
        ["git", "diff", "--name-only", "--diff-filter=ACMR", base, head, "--", "hogd"],
        cwd=root,
        check=True,
        stdout=subprocess.PIPE,
        text=True,
    )
    return {
        line.strip()
        for line in proc.stdout.splitlines()
        if line.strip() and is_hogd_cpp(line.strip()) and (root / line.strip()).exists()
    }


def inputs_for_full(root: Path, compile_db: Path) -> set[str]:
    _, header_to_sources = dependency_graph(compile_db, root)
    sources = all_files(root, ".cc")
    headers = all_files(root, ".h")
    referenced_headers = set(header_to_sources)
    return sources | (headers - referenced_headers)


def inputs_for_changed(root: Path, compile_db: Path, base: str, head: str) -> set[str]:
    changed = changed_files(root, base, head)
    result = {path for path in changed if path.endswith(".cc")}
    changed_headers = sorted(path for path in changed if path.endswith(".h"))
    if not changed_headers:
        return result
    _, header_to_sources = dependency_graph(compile_db, root)
    for header in changed_headers:
        affected_sources = header_to_sources.get(header, set())
        if affected_sources:
            result.update(affected_sources)
        else:
            result.add(header)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description="List clang-tidy input files")
    parser.add_argument("--compile-db", required=True)
    parser.add_argument("--scope", choices=("full", "changed"), required=True)
    parser.add_argument("--partition", choices=("all", "prod", "exempt"), default="all")
    parser.add_argument("--base")
    parser.add_argument("--head")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    compile_db = Path(args.compile_db)
    if not compile_db.is_absolute():
        compile_db = root / compile_db
    if not compile_db.exists():
        print(f"compile database not found: {compile_db}", file=sys.stderr)
        return 1

    if args.scope == "changed":
        if not args.base or not args.head:
            print("--base and --head are required for changed scope", file=sys.stderr)
            return 1
        inputs = inputs_for_changed(root, compile_db, args.base, args.head)
    else:
        inputs = inputs_for_full(root, compile_db)

    for path in sorted(path for path in inputs if partition_matches(path, args.partition)):
        print(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
