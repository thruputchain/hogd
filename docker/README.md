# HOGD Docker CI Environment

Container configuration for local CI testing and Namespace.so integration.

Supports both **podman** (preferred) and **docker**. The helper script auto-detects which is available.

## Local Usage

### Quick Start

```bash
# Build the Docker image
./docker/local-ci.sh build

# Run tidy checks (like CI)
./docker/local-ci.sh tidy

# Run full build + tidy
./docker/local-ci.sh full

# Interactive shell for debugging
./docker/local-ci.sh shell
```

### SSH Agent Setup (for private submodules)

The build requires access to private GitHub repositories. If you have SSH access:

```bash
# Start SSH agent (if not running)
eval $(ssh-agent)

# Add your key
ssh-add ~/.ssh/id_ed25519  # or your key path

# Verify it's loaded
ssh-add -l

# Now run tidy - SSH agent will be forwarded into container
./docker/local-ci.sh tidy
```

The script auto-detects SSH agent availability:
- **With SSH agent**: Initializes ALL submodules and fetches private Cargo dependencies
- **Without SSH agent**: Only initializes public submodules (limited testing)

### Available Commands

| Command | Description |
|---------|-------------|
| `build` | Build the Docker image |
| `shell` | Start interactive bash shell in container |
| `tidy` | Run tidy checks only (clang-tidy + clippy + flake8) |
| `full` | Run full Clang build + tidy checks |
| `clean` | Remove Docker image and cache volumes |

### Options

- `--no-cache`: Build Docker image without cache
- `--rebuild`: Force rebuild of Docker image before running command

### What Gets Tested Locally

- Clang Debug build with `-Werror`
- clang-tidy on all `hogd/*.{h,cc}` files
- Cargo clippy on `hogd/rust/{wasmi,crypto}`
- flake8 on Python files

### What Is Skipped Locally (without SSH agent)

- Private submodules (protocol, integration_test_contracts)
- `built_contracts/` - requires private Cargo dependencies
- GCC build (can be added if needed)

With SSH agent configured, the full CI test suite runs locally.

### Caching

The script creates Docker volumes for faster rebuilds:

- `hogd-ccache` - ccache for C++ compilation
- `hogd-cargo-registry` - Cargo registry cache
- `hogd-cargo-git` - Cargo git dependency cache

### Cleanup

```bash
./docker/local-ci.sh clean
```

This removes the Docker image, cache volumes, and build directory.

## Namespace.so Integration

This Dockerfile can also serve as Namespace.so's custom base image to speed up CI runs.

### Option 1: Dockerfile in Namespace.so Dashboard

1. Go to your runner profile settings in Namespace.so
2. Enable "Custom Container Image"
3. Paste the Dockerfile contents
4. Replace `ARG BASE_IMAGE=ubuntu:24.04` with:
   ```dockerfile
   ARG BASE_IMAGE=${NAMESPACE_BASE_IMAGE_REF}
   ```

### Option 2: Push to Registry

# Tag for GitHub Container Registry
podman tag localhost/hogd-ci:latest ghcr.io/groundhogchain/hogd-ci:latest
# Login (need a GitHub PAT with packages:write)
podman login ghcr.io
# Push
podman push ghcr.io/groundhogchain/hogd-ci:latest

Then reference ghcr.io/groundhogchain/hogd-ci:latest in Namespace.so profile.

### Workflow Simplification

Once the custom base image is active, you can remove these steps from `tidy-namespace.yml`:

- "Add LLVM Repository"
- "Install Build Dependencies"
- update-alternatives setup

Keep only:
- SSH agent setup
- Checkout
- Submodule initialization
- `conf.sh` creation
- Build and tidy steps

## Toolchain Versions

- **Clang/LLVM**: 21 (from apt.llvm.org)
- **GCC**: 14
- **Rust**: 1.90.0 (default, required by CMakeLists.txt), stable, nightly-2025-09-24 (for wasm32 builds)
- **cbindgen**: latest
- **Ubuntu**: 24.04 (noble)
