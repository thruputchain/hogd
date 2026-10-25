#!/usr/bin/env python3
"""Run ctest, and replay each failing test's output at the end of the log.

`--output-on-failure` prints a failing test's output inline, where the test
finished -- in a nightly, tens of thousands of lines from the end. Tools that
read a job's log over the API hand back only its tail (the GitHub MCP server
caps that at 5000 lines), and the whole log is only in the run's archive,
which downloads from Azure blob storage and is not reachable from every
network.

TSAN run #197 (#4605) was lost that way: the one failing test of 3986,
HogDevnode.IndexerEndpointServesBlockExplorer, finished three minutes into a
ten-minute suite, a few hundred lines before the start of that tail. The run
told us which test had failed and nothing whatever about why.

So run ctest with --output-junit as well, and when it fails, print the failing
tests' captured output again at the end, where the window reaches. ctest's own
inline output is untouched; this only adds a replay after the summary.

Usage: scripts/ci/ctest_replay.py <ctest args...>
"""

import os
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

# Enough to carry a sanitizer report or a gtest failure, bounded so that a test
# that died printing megabytes cannot push the summary back out of the window
# this exists to land inside.
HEAD_LINES = 60
TAIL_LINES = 240
MAX_TESTS = 20


def failed_testcases(junit_path):
    """The <testcase> elements ctest recorded as failures, in file order."""
    root = ET.parse(junit_path).getroot()
    # ctest writes a single <testsuite>, but iterate so a <testsuites> wrapper
    # from a future CMake parses the same way.
    cases = root.iter("testcase")
    return [
        case
        for case in cases
        if case.find("failure") is not None or case.find("error") is not None
    ]


def elide(text):
    """Head and tail of [text], with a count of what was dropped between."""
    lines = text.splitlines()
    if len(lines) <= HEAD_LINES + TAIL_LINES:
        return lines
    dropped = len(lines) - HEAD_LINES - TAIL_LINES
    return (
        lines[:HEAD_LINES]
        + [f"... {dropped} lines elided by ctest_replay.py ..."]
        + lines[-TAIL_LINES:]
    )


def replay(junit_path, out=None):
    """Print the failing tests' output. Returns the number replayed."""
    # Resolved here rather than as a default argument, which would capture
    # whatever sys.stdout was at import and ignore a later redirect.
    out = sys.stdout if out is None else out
    try:
        cases = failed_testcases(junit_path)
    except (OSError, ET.ParseError) as err:
        print(f"ctest_replay.py: cannot read {junit_path}: {err}", file=out)
        return 0

    if not cases:
        # ctest can fail without a failing test: no tests matched the filter,
        # the build directory is unusable, ctest itself crashed. Say so rather
        # than print an empty replay that reads like nothing went wrong.
        print(
            "ctest_replay.py: ctest failed with no failing test recorded; "
            "see its output above",
            file=out,
        )
        return 0

    print(f"\n===== replay of {len(cases)} failing test(s) =====", file=out)
    print(
        "(the same output --output-on-failure printed inline above, repeated "
        "here so it survives a tail-truncated log)",
        file=out,
    )
    for case in cases[:MAX_TESTS]:
        name = case.get("name", "<unnamed>")
        print(f"\n----- {name} -----", file=out)
        for child in ("failure", "error"):
            node = case.find(child)
            if node is not None and node.get("message"):
                print(f"{child}: {node.get('message')}", file=out)
        system_out = case.find("system-out")
        if system_out is None or not (system_out.text or "").strip():
            print("(ctest captured no output for this test)", file=out)
            continue
        for line in elide(system_out.text):
            print(line, file=out)
    if len(cases) > MAX_TESTS:
        print(
            f"\n... {len(cases) - MAX_TESTS} further failing test(s) not "
            "replayed; see the inline output above ...",
            file=out,
        )
    return len(cases)


def main(argv):
    # --output-junit needs CTest 3.21; x23/CMakeLists.txt already requires
    # 3.28, so every host that can build the tree can write the XML.
    if any(arg.split("=", 1)[0] == "--output-junit" for arg in argv):
        # Honour an explicit path rather than silently overriding it, so a
        # caller that wants the XML kept still gets it where it asked.
        return subprocess.call(["ctest"] + argv)

    # An absolute path: ctest resolves a relative --output-junit against the
    # build directory, which --test-dir may have moved.
    handle, junit_path = tempfile.mkstemp(prefix="ctest-", suffix=".xml")
    os.close(handle)
    try:
        status = subprocess.call(
            ["ctest", "--output-junit", junit_path] + argv
        )
        if status != 0:
            replay(junit_path)
        return status
    finally:
        os.unlink(junit_path)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
