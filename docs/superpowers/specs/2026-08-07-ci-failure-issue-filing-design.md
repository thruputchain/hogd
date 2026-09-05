# CI Failure Issue Filing — Design

**Date:** 2026-08-07
**Status:** Approved

## Goal

When one of the long-running sanitizer/metrics workflows fails, automatically file a
GitHub issue containing enough context (run links, failed jobs, log-download commands)
for another agent or a human to start diagnosing. No root-cause analysis in the issue —
just pointers.

Initial rollout covers four workflows:

- `.github/workflows/sanitizer.yml`
- `.github/workflows/tsan.yml`
- `.github/workflows/combined_metrics.yml`
- `.github/workflows/asyncio_stress_test.yml`

The mechanism must be trivially reusable by other workflows later.

## Decisions

- **Dedup policy:** none — every failed run files a new issue. Triage closes them.
- **Label:** `ci-failure`, one shared label across all auto-filed issues, created
  automatically if missing. Label name is an input (default `ci-failure`) so it can be
  overridden per workflow later.

## Architecture

### Shared unit: composite action

`.github/actions/file-failure-issue/action.yml`, mirroring the existing
`print-log-instructions` composite action. Implementation is bash + `curl` against the
GitHub REST API using `github.token` — no dependency on `gh` being installed on the
self-hosted runners.

Rejected alternatives:

- **Reusable workflow (`workflow_call`):** forces a separate job, loses direct access to
  the failing job's context.
- **Bare script in `scripts/`:** still needs a wrapper step in every workflow; the
  composite action *is* that wrapper and matches repo convention.

### Per-workflow diff

Each of the four workflows gets two edits:

1. `issues: write` added to the job `permissions` block (currently
   `contents: read, actions: read`).
2. One step appended after the `hog_sched` step:

   ```yaml
   - name: File failure issue
     if: failure()
     uses: ./.github/actions/file-failure-issue
   ```

`actions/checkout` runs before the `hog_sched` step in all four workflows, so the local
action path resolves.

## Action behavior

Inputs:

- `label` — issue label, default `ci-failure`.

Steps (single bash step):

1. **Ensure label exists:** `POST /repos/{repo}/labels`; treat HTTP 422
   (`already_exists`) as success.
2. **Collect failed jobs:** `GET /repos/{repo}/actions/runs/{run_id}/jobs`, select jobs
   with `conclusion == "failure"` plus the currently-executing job (which is mid-failure
   and has no conclusion yet).
3. **File the issue:** `POST /repos/{repo}/issues` with:
   - **Title:** `[ci-failure] <workflow name>: run #<run_number> failed`
   - **Body:**
     - Link to the run: `{server_url}/{repo}/actions/runs/{run_id}/attempts/{run_attempt}`
     - Commit SHA (linked), branch/ref, trigger event, UTC timestamp
     - Failed job names, each with its job URL and the log-download command
       `gh run view --log --job <job_id> --repo <repo>`
     - A one-line note that the issue was auto-filed and is meant as a starting point
       for diagnosis
   - **Labels:** `[<label>]`

## Error handling

The step only runs when the job has already failed, so it must never mask the real
signal: any API error is logged and the step exits 0. The workflow's red status comes
from the `hog_sched` step regardless.

## Testing

- Verify the action script by inspection (it is a single short bash step; the API calls
  mirror the existing `print-log-instructions` pattern).
- Validate workflow YAML syntax locally.
- Live validation happens on the next genuine failure. If a live test is wanted sooner,
  a temporary `workflow_dispatch`-only workflow with a failing step can exercise the
  action end-to-end.
