set -euo pipefail

for i in {1..100}; do
  timeout 60m codex exec --dangerously-bypass-approvals-and-sandbox "
Goal: improve runtime performance without changing behavior.

Procedure:
1. Read CHANGES.md first so you do not repeat previous work.
2. Identify one hotspot using benchmarks or profiling.
   You may use existing benchmarks or add focused new ones.
   You may add timestamps to the code (e.g., using ElapsedTimer) to measure the
   overhead of code regions.
   You may use tools like perf, gdb, and other local tools.
   The machine has over 300 GiB of DRAM, but be careful to not use up all the
   memory so that the machine does not resort to swapping and/or you and your
   work don't get killed by the OOM killer.
3. Make one focused optimization.
4. Build the project.
5. Run tests to confirm functionality is unchanged.
6. Run benchmarks to verify a meaningful improvement.
7. If the change is correct and faster:
   - write a short entry to CHANGES.md describing what you changed and why
   - create a new branch with a name that starts with \"codex/\" and then write
     a timestamp followed by a short human readable description of the change
   - commit all relevant changes with a detailed commit message in this new
     branch
   - push to HEAD
   - stop immediately without switching back to the previous branch
8. If the change is not correct or not faster, revert it and stop without making
a new branch or committing.

Prioritize making changes to hogd/mempool/.
Use /tmp/codex as your workspace (make the directory if needed).
Do not make more than one optimization in this run.
"
done
