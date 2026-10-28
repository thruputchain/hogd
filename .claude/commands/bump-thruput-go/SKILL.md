---
name: bump-thruput-go
description: Use when a protocol change has landed on protocol `main` and the thruput-go submodule needs to be re-synced to pick it up (e.g. after pushing a protocol PR). Propagates protocol main through the nested thruput-go → protocol-go → protocol submodules, regenerates protocol-go Go bindings, and commits at each level. Does NOT push — the user pushes.
allowed-tools: Bash
---

# Bump thruput-go

Propagate the latest `protocol` repo `main` up through thruput-go's nested
submodules and push. `thruput-go` (a submodule of hogd) contains `protocol-go`,
which contains `protocol` — the SAME protocol repo hogd uses, nested two levels
deep.

> Two dependent submodules pointing at the same protocol repo is a known wart
> and is going away; that's a separate cleanup, not this skill's problem.

## Layout

```
hogd/thruput-go/                                submodule, branch main, commit.sh
hogd/thruput-go/protocol-go/                    submodule, branch main, build.sh + commit.sh
hogd/thruput-go/protocol-go/protocol/           submodule, branch main  (same repo as hogd/protocol)
```

## Preconditions

- The protocol change is already committed **and pushed to `protocol` main**.
  This skill only *pulls* protocol main; it never creates protocol commits.
- Run from the hogd repo root.

## Steps

### 1. Sync all three levels to the tip of their `main` (outermost first)

```bash
git -C thruput-go checkout main && git -C thruput-go pull
git -C thruput-go/protocol-go checkout main && git -C thruput-go/protocol-go pull
git -C thruput-go/protocol-go/protocol checkout main && git -C thruput-go/protocol-go/protocol pull
```

A level reporting "Already up to date" is fine — it was already current; continue.

### 2. Regenerate bindings and commit in protocol-go

`build.sh` and `commit.sh` use relative paths (`./protocol`), so they MUST run
from inside `protocol-go`:

```bash
(cd thruput-go/protocol-go && ./build.sh && git add -A && ./commit.sh)
```

- `build.sh` regenerates `pkg.go` / `generated.go` / `hogp.go` (the latter embeds
  the protocol commit hash), runs `go mod tidy && go generate && go build`. It has
  `set -e` — if `go build`/`go generate` fails, fix the cause before committing.
- `commit.sh` runs `git commit -m "bump to protocol@<short-hash>"` (commit only).

### 3. Commit the pointer bumps in thruput-go

```bash
(cd thruput-go && git add . && ./commit.sh)
```

- `commit.sh` records a message like `protocol=… chuckd=… protocol-go=… lib-go=…`
  (commit only).

### 4. Stop — do NOT push

This skill never pushes. A hook blocks automated pushes to these repos, so
pushing is left to the user. Report the new commits and hand off the push
commands (innermost first, so pointers resolve on the remote):

```bash
git -C thruput-go/protocol-go push
git -C thruput-go push
```

`protocol` itself is already on its remote main (step 1 only pulled it).

## Gotchas

- **Never push from this skill** — neither `commit.sh` pushes, and the skill
  itself must not run `git push`. Surface the push commands for the user to run.
- **Stops at thruput-go.** This does NOT bump hogd's own `thruput-go`
  submodule pointer — those are staged deliberately, separately.
- If a `commit.sh` reports "nothing to commit", that level had no pointer change
  (e.g. protocol main hadn't actually advanced) — re-check the preconditions.
