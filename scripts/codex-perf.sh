#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Error: you must specify the directory to have the agent focus on." >&2
  echo "Usage: $0 <directory>" >&2
  exit 1
fi

FOCUS_DIR="$1"

WORKSPACE_PATH="/tmp/codex"
CHANGES_PATH="$WORKSPACE_PATH/CHANGES.md"
mkdir -p "$WORKSPACE_PATH"
# We are passing [-f] so that [rm] fails silently if the CHANGES.md file does
# not exist. We placed [set -euo pipefail] at the top of the script, so the
# entire script will exit if [rm] does not fail silently.
rm -f "$CHANGES_PATH"

for i in {1..100}; do
  # Clean up state left behind if codex timed out in the previous iteration.
  git switch main || git checkout main
  git reset --hard
  git clean -fd

  if ! sudo -n /usr/bin/systemd-run --scope --quiet \
    -p MemoryHigh=270G \
    -p MemoryMax=300G \
    -p MemorySwapMax=0 \
    sudo -u "$USER" \
    env \
    HOME="$HOME" \
    PATH="$PATH" \
    XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" \
    XDG_CONFIG_HOME="${XDG_CONFIG_HOME-}" \
    XDG_CACHE_HOME="${XDG_CACHE_HOME-}" \
    OPENAI_API_KEY="${OPENAI_API_KEY-}" \
    OPENAI_BASE_URL="${OPENAI_BASE_URL-}" \
    OPENAI_ORG_ID="${OPENAI_ORG_ID-}" \
    timeout --kill-after=30s 60m \
    codex exec --dangerously-bypass-approvals-and-sandbox "
Goal: improve runtime performance without changing behavior.

Make sure you start in the main branch if you are not there already.

Procedure:
1. Read \"${CHANGES_PATH}\" first (if it exists) so you do not repeat previous work.
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
   - write a short entry to \"${CHANGES_PATH}\" describing what you changed and
     why
   - run 'clang-format -i' on any changed files. for example, if you changed
     file1.cc and file2.cc, do 'clang-format -i file1.cc file2.cc'
   - create a new branch with a name that starts with \"codex/\" and then write
     a timestamp followed by a short human readable description of the change
   - commit all relevant changes with a detailed commit message in this new
     branch
   - do 'git push -u origin HEAD'
   - switch back to the main branch
   - open a GitHub PR for review with the gh command line tool and fill in the
     command accordingly: gh pr create --base main --head my-branch --title \"My
     PR title\" --body \"Description of the change\"
   - stop immediately
8. If the change is not correct or not faster, revert it and do not make a new branch or commit anything. Describe what you did in \"${CHANGES_PATH}\" and make it clear that this did not work. This information will be useful for you in the future so that you do not try the same thing again. After that, stop.

Prioritize making changes to \"${FOCUS_DIR}\".
Use \"$WORKSPACE_PATH\" as your workspace (make the directory if needed).
Do not make more than one optimization in this run.
"; then
    echo "Iteration $i failed or timed out" >&2
    exit 1
  fi
done
