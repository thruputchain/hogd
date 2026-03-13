set -e

for i in {1..20}; do
  codex exec --full-auto "
Goal: improve runtime performance without changing behavior.

Procedure:
1. Identify a hotspot using benchmarks or profiling. Feel free to use existing
benchmarks or add your own since not all parts of the codebase are covered by
benchmarks. Also feel free to use any tools you wish, such as perf, gdb, and
others. Be sure not to repeat any work you previously did, which is detailed in CHANGES.md.
2. Modify the code to improve it.
3. Build the project.
4. Run tests to confirm functionality is unchanged.
5. Run benchmarks to verify improvement.
6. If the change is correct and faster, write a description of what you did to CHANGES.md and commit everything with a detailed message.
7. If not, revert the change.

You may write additional benchmarks if necessary.
"
done
