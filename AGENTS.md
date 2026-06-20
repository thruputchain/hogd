# AGENTS.md

## Cursor Cloud specific instructions

This repository uses a custom Cursor Cloud Agent environment defined in `.cursor/environment.json`. The Dockerfile installs the same toolchain as CI (clang-21, gcc-15, Rust toolchains, wabt, binaryen, etc.).

### Required Cursor Secrets

Configure these in **Cursor Dashboard → Cloud Agents → Secrets** before running agents:

| Secret | Purpose |
|--------|---------|
| `CARGO_REGISTRIES_GROUNDHOG_TOKEN` | Authenticate with the Groundhog crate registry (`./scripts/fetch.sh`) |

The main repo clone token covers `groundhogchain/hogd` only. Private submodules also need read access:

- `protocol`
- `groundhog-go`
- `hogx`
- `built_contracts/integration_test_contracts`

If submodule init fails with "Repository not found", grant the cloud agent GitHub App (or a PAT secret) access to those private repos in the `groundhogchain` org.

### What the install script does

On each agent startup, `.cursor/cloud-agent-install.sh` runs:

1. `git submodule update --init --recursive`
2. Writes `scripts/conf.sh` with CI compiler paths
3. `./scripts/fetch.sh` (cargo fetch + groundhog-cli / groundhog-rv32-lower install)

### Building and testing

After install, agents can build and test like CI:

```bash
# Configure + build (Release, gcc)
./scripts/build.sh -d build -t Release -c gcc

# Run tests
cd build && ctest --output-on-failure --parallel $(nproc) --timeout 120
```

For Debug builds or clang, pass `-t Debug` or `-c clang`.

Format and lint checks:

```bash
./scripts/format.sh -c
./scripts/tidy.sh
```

### Submodule PRs

The built-in PR tool only targets the workspace root repo (`hogd`). To open PRs in submodule repos (`protocol`, `groundhog-go`, etc.), use the `gh` CLI explicitly.

### Environment snapshots

If a saved environment snapshot exists in the Cursor dashboard, it overrides the Dockerfile. Delete old snapshots when changing `.cursor/environment.json` so agents rebuild from the committed config.
