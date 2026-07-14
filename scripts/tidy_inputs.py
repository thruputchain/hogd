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


def proxy_compile_argv(entry: dict, source: Path, output: Path) -> list[str]:
    argv = command_argv(entry)
    original_file = Path(entry["file"])
    if not original_file.is_absolute():
        original_file = Path(entry.get("directory", ".")) / original_file
    cleaned = []
    skip_next = False
    skip_with_arg = {
        "-o",
        "-MF",
        "-MT",
        "-MQ",
        "-MJ",
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
        if Path(arg) == original_file or arg == entry["file"]:
            continue
        cleaned.append(arg)
    return cleaned + ["-c", str(source), "-o", str(output)]


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
    sources, header_to_sources = dependency_graph(compile_db, root)
    headers = all_files(root, ".h")
    referenced_headers = set(header_to_sources)
    return sources | (headers - referenced_headers)


def representative_source(
    header: str, affected_sources: set[str], changed_sources: set[str]
) -> str:
    companion = Path(header).with_suffix(".cc").as_posix()
    if companion in affected_sources:
        return companion
    changed_dependents = affected_sources & changed_sources
    if changed_dependents:
        return min(changed_dependents)
    return min(affected_sources)


def inputs_for_changed(
    root: Path, compile_db: Path, base: str, head: str
) -> set[str]:
    changed = changed_files(root, base, head)
    changed_sources = {path for path in changed if path.endswith(".cc")}
    result = set(changed_sources)
    changed_headers = sorted(path for path in changed if path.endswith(".h"))
    if not changed_headers:
        return result
    _, header_to_sources = dependency_graph(compile_db, root)
    for header in changed_headers:
        affected_sources = header_to_sources.get(header, set())
        if affected_sources:
            representative = representative_source(
                header, affected_sources, changed_sources
            )
            result.add(representative)
        else:
            # Only standalone headers need a synthetic translation unit.
            result.add(header)
    return result


def write_proxy_compile_db(
    root: Path,
    compile_db: Path,
    inputs: set[str],
    proxy_dir: Path,
    proxy_compile_db: Path,
) -> set[str]:
    with compile_db.open(encoding="utf-8") as file:
        entries = json.load(file)
    base_entry = None
    for entry in entries:
        try:
            rel = repo_path(Path(entry["file"]), root)
        except ValueError:
            continue
        if rel.startswith("hogd/") and rel.endswith(".cc"):
            base_entry = entry
            break
    if base_entry is None:
        print("no hogd .cc entry found in compile database", file=sys.stderr)
        sys.exit(1)
    proxy_dir.mkdir(parents=True, exist_ok=True)
    proxy_compile_db.parent.mkdir(parents=True, exist_ok=True)
    rewritten_inputs = set()
    proxy_entries = []
    for path in sorted(inputs):
        if not path.endswith(".h"):
            rewritten_inputs.add(path)
            continue
        proxy = proxy_dir / f"{path.replace('/', '__')}.cc"
        proxy.write_text(f'#include "{path}"\n', encoding="utf-8")
        rewritten_inputs.add(str(proxy))
        proxy_entries.append(
            {
                "directory": str(root),
                "arguments": proxy_compile_argv(
                    base_entry, proxy, proxy.with_suffix(".o")
                ),
                "file": str(proxy),
            }
        )
    with proxy_compile_db.open("w", encoding="utf-8") as file:
        json.dump(entries + proxy_entries, file)
    return rewritten_inputs


def main() -> int:
    parser = argparse.ArgumentParser(description="List clang-tidy input files")
    parser.add_argument("--compile-db", required=True)
    parser.add_argument("--scope", choices=("full", "changed"), required=True)
    parser.add_argument("--partition", choices=("all", "prod", "exempt"), default="all")
    parser.add_argument("--base")
    parser.add_argument("--head")
    parser.add_argument("--proxy-dir")
    parser.add_argument("--proxy-compile-db")
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

    inputs = {path for path in inputs if partition_matches(path, args.partition)}

    if args.proxy_dir or args.proxy_compile_db:
        if not args.proxy_dir or not args.proxy_compile_db:
            print(
                "--proxy-dir and --proxy-compile-db must be used together",
                file=sys.stderr,
            )
            return 1
        inputs = write_proxy_compile_db(
            root,
            compile_db,
            inputs,
            Path(args.proxy_dir),
            Path(args.proxy_compile_db),
        )

    for path in sorted(inputs):
        print(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
