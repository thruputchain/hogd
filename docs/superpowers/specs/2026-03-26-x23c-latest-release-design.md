# x23c Latest GitHub Release Design

Date: 2026-03-26
Status: Approved design

## Summary

Add a new manually triggered GitHub Actions workflow that builds the `x23c`
compiler for Linux x86_64 and publishes it to a single mutable GitHub Release
tagged `latest-x23c`.

This workflow is intentionally separate from the existing proper release flow in
`.github/workflows/release.yml`.
It exists only to provide a stable, easy-to-download `x23c` binary for use in
GitHub workflows.

## Goals

- Provide a stable GitHub Release download target for `x23c`.
- Support Linux x86_64 only.
- Require only manual execution via `workflow_dispatch`.
- Reuse the existing repo build environment and CI conventions where practical.
- Keep the main release workflow untouched.
- Avoid any local release mirror integration.

## Non-Goals

- No changes to the existing proper release workflow.
- No full product release packaging.
- No macOS, arm64, or Windows artifacts.
- No semantic versioning or per-commit immutable tags.
- No local-server sync or release mirroring.

## Current State

- `.github/workflows/release.yml`
  builds a full GCC release bundle and publishes releases only for pushed tags.
- `x23c` already exists as a standalone CMake target in
  `x23/x23c/CMakeLists.txt`.
- The repo build wrapper in `scripts/build.sh`
  accepts explicit make targets after option parsing, so it can build `x23c`
  without building the full release bundle.

## Approaches Considered

### 1. Dedicated manual `latest-x23c` release workflow

Recommended.

Create a new standalone workflow that builds only `x23c` and publishes one
mutable release/tag pair, `latest-x23c`.

Pros:

- Minimal scope and isolation.
- Matches the requested GitHub release snapshot flow.
- Gives downstream workflows a stable asset URL.

Cons:

- Duplicates a small amount of environment setup from the main release workflow.

### 2. Extend the existing release workflow

Rejected.

This would couple an ad hoc compiler snapshot path to the proper release path
and increases the chance of accidentally affecting the current release process.

### 3. Publish workflow artifacts only

Rejected.

This is less convenient for downstream workflow consumption and does not provide
the stable GitHub Release download contract requested.

## Proposed Design

### Workflow shape

Add a new workflow file:

- `.github/workflows/x23c_latest_release.yml`

Trigger:

- `workflow_dispatch` only

Permissions:

- `contents: write`
- `actions: read`

Runner:

- Use the same Linux Namespace runner class used by the proper release workflow,
  unless implementation discovers a narrower compatible runner already used
  elsewhere in the repo.

### Build steps

The new workflow should reuse the proven setup sequence from the existing
release workflow where relevant:

- SSH agent setup for private repo access
- checkout
- Namespace cache configuration
- submodule cache/git configuration
- submodule initialization
- environment configuration
- `./scripts/fetch.sh`

The build itself should stay narrow:

```bash
./scripts/build.sh -d build/release -t Release -c gcc x23c
```

Because `scripts/build.sh`
passes remaining arguments through to `make`, this configures the build and then
builds only the `x23c` target instead of the whole release target set.

Expected output binary:

- `build/release/x23/x23c/x23c`

### Packaging

Package one binary tarball:

- `x23c-linux-x86_64.tar.gz`

Also generate:

- `SHA256SUMS`

The tarball should contain the executable as `x23c` at the archive root.

No headers, libraries, or other release artifacts are included. This release is
for workflow-time code generation, not for full SDK/system installation.

### Release contract

Maintain exactly one mutable GitHub Release:

- tag: `latest-x23c`
- release name: human-readable `x23c latest` variant

The release body should record provenance for the current asset set, including:

- source commit SHA
- workflow run URL or run identifier
- build date

That provenance replaces the need for per-commit release tags.

### Asset update behavior

Each manual run should replace the existing asset set for `latest-x23c` with the
freshly built tarball and checksum file.

Desired behavior:

- first run creates the `latest-x23c` release
- later runs overwrite or recreate the release assets so the same tag always
  points to the newest `x23c` snapshot

Implementation may use either:

- GitHub CLI / REST API logic to create-or-update the release and replace assets
- a release action that reliably supports updating assets for a fixed tag

The implementation choice must preserve the single stable tag contract.

### Download contract

Downstream workflows should be able to fetch the binary from a stable release
URL shaped like:

```text
https://github.com/<owner>/<repo>/releases/download/latest-x23c/x23c-linux-x86_64.tar.gz
```

This stable URL is the primary value of the workflow.

### Release visibility guard

This snapshot release must not interfere with the repo's proper release flow.

Specifically, implementation should avoid making `latest-x23c` become the
repository-wide "Latest release" shown in GitHub UI if the platform/tooling
allows that to be controlled.

Acceptable ways to satisfy this include:

- explicitly marking the release as not latest
- using release metadata that keeps it out of the proper release lane

The exact mechanism can be finalized during implementation based on available
GitHub Actions or API support.

## Error Handling

- If the build fails, do not modify the existing `latest-x23c` release.
- If packaging fails, do not modify the existing `latest-x23c` release.
- If release publishing fails after artifacts are built, fail the workflow and
  keep logs/artifacts available for diagnosis.
- Emit checksum information and release-target details into the logs to simplify
  debugging.

## Testing And Verification

Implementation should include verification at three levels:

1. Static review of the new workflow YAML.
2. Local validation where feasible:
   - confirm `./scripts/build.sh -d build/release -t Release -c gcc x23c`
     targets only `x23c`
   - confirm the packaged tarball contains only the expected executable
3. Workflow-level validation:
   - manual dry run in GitHub Actions
   - confirm the release/tag `latest-x23c` exists
   - confirm the asset downloads from the stable URL
   - confirm a second manual run replaces the asset cleanly

## Rollout

1. Add the new workflow file.
2. Validate the workflow syntax and local packaging logic.
3. Manually trigger the workflow once to create the initial release.
4. Document the stable download URL in a follow-up if needed.

## Open Questions

None. The requested scope is intentionally narrow:

- Linux x86_64 only
- manual trigger only
- one mutable `latest-x23c` release
- no interaction with the proper product release flow
