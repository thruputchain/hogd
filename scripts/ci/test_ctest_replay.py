#!/usr/bin/env python3

import io
import os
import stat
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ctest_replay


def junit(body):
    """A ctest --output-junit file wrapping [body], as a temporary path."""
    handle, path = tempfile.mkstemp(suffix=".xml")
    with os.fdopen(handle, "w") as out:
        out.write(
            '<?xml version="1.0" encoding="UTF-8"?>\n'
            f'<testsuite name="(empty)">\n{body}\n</testsuite>\n'
        )
    return path


PASSED = textwrap.dedent(
    """\
    <testcase name="Fine.Passes" classname="Fine" status="run">
      <system-out>nothing to see</system-out>
    </testcase>
    """
)

FAILED = textwrap.dedent(
    """\
    <testcase name="HogDevnode.IndexerEndpointServesBlockExplorer"
              classname="HogDevnode" status="fail">
      <failure type="Failed" message="Failed"/>
      <system-out>WARNING: ThreadSanitizer: data race
      Write of size 8 at 0x7b0c by thread T3
      SUMMARY: ThreadSanitizer: data race indexer.cc:42</system-out>
    </testcase>
    """
)


class FailedTestcasesTest(unittest.TestCase):
    def test_selects_only_failures(self):
        path = junit(PASSED + FAILED)
        self.addCleanup(os.unlink, path)

        names = [c.get("name") for c in ctest_replay.failed_testcases(path)]

        self.assertEqual(
            names, ["HogDevnode.IndexerEndpointServesBlockExplorer"]
        )

    def test_counts_error_children_as_failures(self):
        path = junit(
            '<testcase name="Broken" status="fail">'
            '<error type="Exception" message="boom"/>'
            "</testcase>"
        )
        self.addCleanup(os.unlink, path)

        self.assertEqual(len(ctest_replay.failed_testcases(path)), 1)

    def test_no_failures_in_a_green_run(self):
        path = junit(PASSED)
        self.addCleanup(os.unlink, path)

        self.assertEqual(ctest_replay.failed_testcases(path), [])


class ElideTest(unittest.TestCase):
    def test_keeps_short_output_whole(self):
        self.assertEqual(ctest_replay.elide("a\nb\nc"), ["a", "b", "c"])

    def test_keeps_head_and_tail_of_long_output(self):
        total = ctest_replay.HEAD_LINES + ctest_replay.TAIL_LINES + 500
        lines = ctest_replay.elide(
            "\n".join(f"line{i}" for i in range(total))
        )

        self.assertEqual(lines[0], "line0")
        self.assertEqual(lines[-1], f"line{total - 1}")
        self.assertIn("500 lines elided", lines[ctest_replay.HEAD_LINES])
        self.assertEqual(
            len(lines), ctest_replay.HEAD_LINES + ctest_replay.TAIL_LINES + 1
        )


class ReplayTest(unittest.TestCase):
    def replay_output(self, path):
        out = io.StringIO()
        count = ctest_replay.replay(path, out=out)
        return count, out.getvalue()

    def test_prints_the_failing_tests_output(self):
        path = junit(PASSED + FAILED)
        self.addCleanup(os.unlink, path)

        count, text = self.replay_output(path)

        self.assertEqual(count, 1)
        self.assertIn("HogDevnode.IndexerEndpointServesBlockExplorer", text)
        self.assertIn("SUMMARY: ThreadSanitizer: data race", text)
        self.assertNotIn("nothing to see", text)

    def test_says_so_when_ctest_failed_without_a_failing_test(self):
        path = junit(PASSED)
        self.addCleanup(os.unlink, path)

        count, text = self.replay_output(path)

        self.assertEqual(count, 0)
        self.assertIn("no failing test recorded", text)

    def test_survives_a_missing_or_unparsable_file(self):
        count, text = self.replay_output("/nonexistent/ctest.xml")

        self.assertEqual(count, 0)
        self.assertIn("cannot read", text)

    def test_caps_the_number_of_tests_replayed(self):
        cases = "".join(
            f'<testcase name="Fails{i}" status="fail">'
            f'<failure type="Failed"/><system-out>body{i}</system-out>'
            "</testcase>"
            for i in range(ctest_replay.MAX_TESTS + 3)
        )
        path = junit(cases)
        self.addCleanup(os.unlink, path)

        count, text = self.replay_output(path)

        self.assertEqual(count, ctest_replay.MAX_TESTS + 3)
        self.assertIn(f"body{ctest_replay.MAX_TESTS - 1}", text)
        self.assertNotIn(f"body{ctest_replay.MAX_TESTS}", text)
        self.assertIn("3 further failing test(s) not replayed", text)


class MainTest(unittest.TestCase):
    """Drives main() against a stub ctest, so no build tree is needed."""

    def stub_ctest(self, script):
        directory = tempfile.mkdtemp()
        self.addCleanup(
            subprocess.call, ["rm", "-rf", "--", directory]
        )
        path = Path(directory) / "ctest"
        path.write_text("#!/usr/bin/env bash\n" + script)
        path.chmod(path.stat().st_mode | stat.S_IXUSR)
        return directory

    def run_main(self, script, argv):
        directory = self.stub_ctest(script)
        env_path = os.environ["PATH"]
        os.environ["PATH"] = f"{directory}:{env_path}"
        self.addCleanup(os.environ.__setitem__, "PATH", env_path)

        out = io.StringIO()
        stdout = sys.stdout
        sys.stdout = out
        try:
            status = ctest_replay.main(argv)
        finally:
            sys.stdout = stdout
        return status, out.getvalue()

    def test_passes_through_a_green_run_without_replaying(self):
        status, text = self.run_main("exit 0\n", ["--test-dir", "build"])

        self.assertEqual(status, 0)
        self.assertNotIn("replay", text)

    def test_replays_and_keeps_ctests_status_on_failure(self):
        # The stub writes the XML to whatever path it was handed, the way
        # ctest would, then fails.
        script = textwrap.dedent(
            """\
            junit=""
            while [ $# -gt 0 ]; do
              if [ "$1" = --output-junit ]; then junit="$2"; shift; fi
              shift
            done
            cat > "$junit" <<'XML'
            <testsuite name="(empty)">
            <testcase name="Boom" status="fail">
            <failure type="Failed"/><system-out>the real reason</system-out>
            </testcase>
            </testsuite>
            XML
            exit 8
            """
        )

        status, text = self.run_main(script, ["--test-dir", "build"])

        self.assertEqual(status, 8)
        self.assertIn("the real reason", text)

    def test_does_not_override_an_explicit_output_junit(self):
        for form in (["--output-junit", "mine.xml"], ["--output-junit=m.xml"]):
            with self.subTest(form=form):
                status, _ = self.run_main(
                    '[ "$1" = --output-junit ] || [ "${1%%=*}" = '
                    '--output-junit ] || exit 9\nexit 0\n',
                    form,
                )
                self.assertEqual(status, 0)


if __name__ == "__main__":
    unittest.main()
