#!/usr/bin/env python3

import argparse
import json
import re
import sys
from json import JSONDecodeError


DEFAULT_COUNTER_PREFIXES = ("gas", "rv32.")
TIME_UNIT_TO_NS = {
    "ns": 1.0,
    "us": 1_000.0,
    "ms": 1_000_000.0,
    "s": 1_000_000_000.0,
}


def load_benchmarks(path):
    with open(path, encoding="utf-8") as file:
        data = json.load(file)

    iterations = {}
    means = {}
    aggregates = {}
    for benchmark in data.get("benchmarks", []):
        name = benchmark.get("run_name") or benchmark.get("name")
        if not name:
            continue
        if benchmark.get("run_type") == "aggregate":
            if benchmark.get("aggregate_name") == "mean":
                means[name] = benchmark
            elif "aggregate_name" not in benchmark:
                aggregates[name] = benchmark
            continue
        if benchmark.get("run_type", "iteration") == "iteration":
            iterations[name] = benchmark
    return iterations | aggregates | means


def markdown_escape(value):
    return (
        str(value)
        .replace("\\", "\\\\")
        .replace("|", "\\|")
        .replace("`", "\\`")
        .replace("\n", "<br>")
    )


def percent_change(before, after):
    if before is None or after is None or before == 0:
        return None
    return ((after - before) / before) * 100.0


def format_number(value):
    if value is None:
        return "-"
    if abs(value) >= 1000:
        return f"{value:,.0f}"
    if value == int(value):
        return f"{value:.0f}"
    return f"{value:.3f}".rstrip("0").rstrip(".")


def time_ns(benchmark, key):
    value = benchmark.get(key)
    unit = benchmark.get("time_unit", "ns")
    factor = TIME_UNIT_TO_NS.get(unit)
    if value is None or factor is None:
        return None
    return value * factor


def format_delta(value, faster_is_positive=False):
    if value is None:
        return "-"
    adjusted = -value if faster_is_positive else value
    return f"{adjusted:+.2f}%"


def counter_keys(before, after):
    keys = set()
    for benchmark in (before, after):
        for key, value in benchmark.items():
            if not isinstance(value, (int, float)):
                continue
            if key in {"iterations", "real_time", "cpu_time", "time_unit"}:
                continue
            if key == "bytes_per_second" or key.startswith(DEFAULT_COUNTER_PREFIXES):
                keys.add(key)
    return sorted(keys)


def format_counter_changes(before, after):
    changes = []
    for key in counter_keys(before, after):
        before_value = before.get(key)
        after_value = after.get(key)
        if before_value == after_value:
            continue
        delta = percent_change(before_value, after_value)
        changes.append(
            f"{markdown_escape(key)}: {format_number(before_value)} -> "
            f"{format_number(after_value)} ({format_delta(delta)})"
        )
    return "<br>".join(changes) if changes else "-"


def selected_names(before, after, regex):
    names = sorted(set(before) | set(after))
    if regex is None:
        return names
    return [name for name in names if regex.search(name)]


def write_markdown(before, after, pattern, output):
    names = selected_names(before, after, pattern)
    print(
        "| Benchmark | CPU before | CPU after | CPU delta | Real before | "
        "Real after | Real delta | Items/s before | Items/s after | "
        "Items/s delta | Counter changes |",
        file=output,
    )
    print(
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|",
        file=output,
    )
    for name in names:
        before_benchmark = before.get(name)
        after_benchmark = after.get(name)
        if before_benchmark is None or after_benchmark is None:
            before_state = "missing" if before_benchmark is None else "-"
            after_state = "missing" if after_benchmark is None else "-"
            print(
                f"| {markdown_escape(name)} | {before_state} | {after_state} | - | - | - | "
                "- | - | - | - | - |",
                file=output,
            )
            continue

        cpu_before = time_ns(before_benchmark, "cpu_time")
        cpu_after = time_ns(after_benchmark, "cpu_time")
        real_before = time_ns(before_benchmark, "real_time")
        real_after = time_ns(after_benchmark, "real_time")
        items_before = before_benchmark.get("items_per_second")
        items_after = after_benchmark.get("items_per_second")
        cpu_delta = percent_change(cpu_before, cpu_after)
        real_delta = percent_change(real_before, real_after)
        items_delta = percent_change(items_before, items_after)
        print(
            f"| {markdown_escape(name)} | {format_number(cpu_before)} ns | "
            f"{format_number(cpu_after)} ns | "
            f"{format_delta(cpu_delta, faster_is_positive=True)} | "
            f"{format_number(real_before)} ns | {format_number(real_after)} ns | "
            f"{format_delta(real_delta, faster_is_positive=True)} | "
            f"{format_number(items_before)} | {format_number(items_after)} | "
            f"{format_delta(items_delta)} | "
            f"{format_counter_changes(before_benchmark, after_benchmark)} |",
            file=output,
        )

    if not names:
        print("_No benchmarks matched._", file=output)


def main():
    parser = argparse.ArgumentParser(
        description="Compare two Google Benchmark JSON outputs."
    )
    parser.add_argument("before", help="baseline benchmark JSON")
    parser.add_argument("after", help="comparison benchmark JSON")
    parser.add_argument(
        "--filter",
        help="regular expression used to select benchmark names",
    )
    args = parser.parse_args()

    try:
        regex = re.compile(args.filter) if args.filter else None
    except re.error as error:
        parser.error(f"invalid --filter regex: {error}")

    try:
        before = load_benchmarks(args.before)
        after = load_benchmarks(args.after)
    except OSError as error:
        parser.error(str(error))
    except JSONDecodeError as error:
        parser.error(f"invalid JSON: {error}")

    write_markdown(
        before,
        after,
        regex,
        sys.stdout,
    )


if __name__ == "__main__":
    main()
