set -e

for i in {1..20}; do
  codex --approval-mode auto-edit "
Goal: improve runtime performance without changing behavior.

Procedure:
1. Identify a hotspot using benchmarks or profiling. Feel free to use existing
benchmarks or add your own since not all parts of the codebase are covered by
benchmarks. Also feel free to use any tools you wish, such as perf, gdb, and
others.
2. Modify the code to improve it.
3. Build the project.
4. Run tests to confirm functionality is unchanged.
5. Run benchmarks to verify improvement.
6. If the change is correct and faster, commit with a detailed message.
7. If not, revert the change.

You may write additional benchmarks if necessary.
"
done
