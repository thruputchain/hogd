#!/usr/bin/env python3
"""Filter MSan log files to show only reports that are likely real bugs.

Suppresses known false positives from:
  - Uninstrumented OpenSSL (memcmp, CRYPTO_strdup, OBJ_obj2txt, etc.)
  - Uninstrumented compression libraries (snappy, LZ4, ZSTD, BZ2, zlib)
  - wasmtime ValRaw union (intentionally reads wider than stored type)
  - rocksdb XXH_read64 (xxhash reads past buffer for performance)
  - MSan interceptors inside uninstrumented code (fopen64, memcpy, strcmp)

Usage:
  python3 scripts/msan_filter.py msan/           # filter a directory of log files
  python3 scripts/msan_filter.py msan_log.12345  # filter a single file
"""

import re
import sys
from pathlib import Path

# Patterns matched against the SUMMARY line.
FALSE_POSITIVE_SUMMARY = [
    # OpenSSL — entirely uninstrumented shared library
    r"\bin memcmp$",
    r"\bin memcpy$",
    r"\bin bcmp$",
    r"\bin strcmp$",
    r"\bin strncmp$",
    r"\bin strlen$",
    r"\bin fopen64$",
    r"\bin BIO_new_file",
    r"\bin CRYPTO_strdup\b",
    r"\bin CRYPTO_zalloc\b",
    r"\bin OPENSSL_strlcpy\b",
    r"\bin OPENSSL_strlcat\b",
    r"\bin OBJ_obj2txt\b",
    r"\bin OPENSSL_",
    r"\bin RAND_",
    r"\bin EVP_",
    r"\bin ossl_",
    r"\bin evp_",
    # wasmtime ValRaw union — reads wider than stored wasm type
    r"ValRaw.*get_[iuf]",
    # rocksdb xxhash — reads past buffer for performance
    r"XXH_read",
    r"XXH3_",
]

# Stack frame patterns — if ANY frame in the report matches, suppress it.
FALSE_POSITIVE_STACK = [
    # OpenSSL shared library (frame from uninstrumented .so)
    r"libcrypto\.so",
    r"libssl\.so",
    # Compression libraries — uninstrumented
    r"libsnappy",
    r"liblz4",
    r"libzstd",
    r"libbz2",
    r"libz\.so",
]

_summary_pats = [re.compile(p) for p in FALSE_POSITIVE_SUMMARY]
_stack_pats = [re.compile(p) for p in FALSE_POSITIVE_STACK]

# Split on ==PID==WARNING lines.  The "Uninitialized bytes in ..." prefix
# belongs to the WARNING that follows it, so we only split on WARNING.
_WARNING = re.compile(r"^==\d+==WARNING: MemorySanitizer")
_UNINIT_PREFIX = re.compile(r"^Uninitialized bytes in ")


def split_reports(content: str) -> list[str]:
    """Split file content into individual MSan reports."""
    reports: list[str] = []
    current: list[str] = []
    for line in content.splitlines():
        if _WARNING.match(line) and current:
            # Check if the last line(s) of current are actually the prefix
            # for THIS report (e.g. "Uninitialized bytes in fopen64 ...")
            prefix: list[str] = []
            while current and _UNINIT_PREFIX.match(current[-1]):
                prefix.insert(0, current.pop())
            # Flush previous report
            if current:
                text = "\n".join(current)
                if "MemorySanitizer" in text:
                    reports.append(text)
            # Start new report with any prefix
            current = prefix
        current.append(line)
    if current:
        text = "\n".join(current)
        if "MemorySanitizer" in text:
            reports.append(text)
    return reports


def classify_report(report: str) -> tuple[bool, str]:
    """Returns (is_false_positive, reason)."""
    lines = report.splitlines()

    # Find SUMMARY line
    summary = ""
    for line in lines:
        if line.startswith("SUMMARY:"):
            summary = line
            break

    # Check summary
    for pat in _summary_pats:
        if pat.search(summary):
            return True, pat.pattern

    # Check stack frames
    for line in lines:
        if re.match(r"\s+#\d+", line):
            for pat in _stack_pats:
                if pat.search(line):
                    return True, pat.pattern

    return False, ""


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]

    if not args:
        print(__doc__)
        sys.exit(1)

    # Collect log files
    files: list[Path] = []
    for arg in args:
        p = Path(arg)
        if p.is_dir():
            files.extend(sorted(p.glob("msan_log.*")))
        elif p.is_file():
            files.append(p)
        else:
            print(f"warning: {arg} not found", file=sys.stderr)

    if not files:
        print("No msan_log.* files found.", file=sys.stderr)
        sys.exit(1)

    # Process all reports
    suppressed: dict[str, int] = {}
    real_bugs: list[tuple[str, str]] = []  # (source_file, report_text)
    total_reports = 0

    for f in files:
        content = f.read_text(errors="replace")
        reports = split_reports(content)
        for report in reports:
            total_reports += 1
            is_fp, reason = classify_report(report)
            if is_fp:
                suppressed[reason] = suppressed.get(reason, 0) + 1
            else:
                real_bugs.append((f.name, report))

    # Deduplicate real bugs by SUMMARY line
    seen_summaries: set[str] = set()
    unique_bugs: list[tuple[str, str]] = []
    for source, report in real_bugs:
        summary = ""
        for line in report.splitlines():
            if line.startswith("SUMMARY:"):
                summary = line
                break
        # Deduplicate by summary + first hogd frame
        hogd_frame = ""
        for line in report.splitlines():
            if re.match(r"\s+#\d+", line) and "/hogd/" in line:
                hogd_frame = line.strip()
                break
        key = (summary, hogd_frame)
        if key not in seen_summaries:
            seen_summaries.add(key)
            unique_bugs.append((source, report))

    # Output
    if unique_bugs:
        print(f"{'='*60}")
        print(f" {len(unique_bugs)} unique potentially real bug(s)")
        print(f"{'='*60}\n")
        for i, (source, report) in enumerate(unique_bugs, 1):
            print(f"[{i}] from {source}")
            print(report)
            print()
    else:
        print("No real bugs found — all reports are known false positives.")

    # Summary table
    n_suppressed = total_reports - len(real_bugs)
    print(f"\n{'='*60}")
    print(f" Total reports:      {total_reports:>8}")
    print(f" Suppressed:         {n_suppressed:>8}")
    print(f" Potentially real:   {len(real_bugs):>8}  ({len(unique_bugs)} unique)")
    print(f" Files scanned:      {len(files):>8}")
    print(f"{'='*60}")
    print(f"\n Suppressed breakdown:")
    for reason, count in sorted(suppressed.items(), key=lambda x: -x[1]):
        print(f"   {count:>8}  {reason}")


if __name__ == "__main__":
    main()
