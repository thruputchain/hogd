# HOGD Docker CI Environment

Container configuration for local CI testing and Namespace.so integration.

Supports both **podman** (preferred) and **docker**. The helper script auto-detects which is available.

## Local Usage

### Quick Start

```bash
# Build the Docker image
./docker/local-ci.sh build

# Run C++ CI checks (format, build, test - fastest option)
./docker/local-ci.sh cpp

# Run tidy checks (clang-tidy + clippy + flake8)
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
| `cpp` | Run C++ CI checks (format, GCC build+test, LLVM build+test) - skips tidy |
| `tidy` | Run tidy checks only (clang-tidy + clippy + flake8) |
| `full` | Run full Clang build + tidy checks |
| `clean` | Remove Docker image and cache volumes |

### Options

- `--no-cache`: Build Docker image without cache
- `--rebuild`: Force rebuild of Docker image before running command

### CPP Workflow Steps

The `cpp` command runs `scripts/check.sh -p -s` which executes these steps:

| Step | Name | Description | Time Est. |
|------|------|-------------|-----------|
| 1 | Format Check | clang-format on `*.cc/*.h`, cargo fmt on Rust, shfmt on shell | ~10s |
| 2 | GCC Debug Build | Full build with gcc-15, `-Werror`, Debug mode | ~5-8 min |
| 3 | GCC Debug Tests | Run all ~814 tests via ctest (parallel) | ~3-5 min |
| 4 | LLVM Debug Build | Full build with clang-21, `-Werror`, Debug mode | ~5-8 min |
| 5 | LLVM Debug Tests | Run all ~814 tests via ctest (parallel) | ~3-5 min |
| 6 | Tidy | clang-tidy + clippy + flake8 | SKIPPED (`-s`) |
| 7 | GCC Release Build | Full build with gcc-15, Release mode (optimized) | ~5-8 min |

**Total: ~20-35 minutes** (depending on cache hits)

Extra steps (only with `-e` flag, not run by default):
- Step 8: TSan - Thread Sanitizer build + tests
- Step 9: ASan - Address Sanitizer build + tests
- Step 10: UBSan - Undefined Behavior Sanitizer build + tests

Notes:
- The `-p` flag runs tests in parallel via `ctest --parallel`
- The `-b` flag (used on Namespace.so, not locally) runs builds 2/4/7 concurrently
- Local CI skips `-b` to prevent OOM on machines with <32GB RAM

### Tidy Workflow Steps

The `tidy` command runs these checks:

| Check | Tool | Target |
|-------|------|--------|
| C++ linting | clang-tidy-21 | `hogd/*.{h,cc}` |
| Rust linting | cargo clippy | `hogd/rust/{wasmi,crypto}` |
| Python linting | flake8 | `hogd/**/*.py` |

### Full Workflow

The `full` command combines: Clang Debug build + all tidy checks

### What Is Skipped Locally (without SSH agent)

- Private submodules (protocol, integration_test_contracts)
- `built_contracts/` - requires private Cargo dependencies

With SSH agent configured, the full CI test suite runs locally.

### Resource Limits

The script automatically limits container CPU and memory to prevent OOM issues:
- **CPUs**: `RAM_GB / 3`, capped at 8 (clang-tidy uses ~3GB per process)
- **Memory**: Host RAM minus 2GB (minimum 4GB for container)

Limits are displayed when starting the container.

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

### Workflows

Two Namespace.so-optimized workflows are available:

- **`.github/workflows/cpp-namespace-optimized.yml`** - C++ CI (format, build, test)
- **`.github/workflows/tidy-namespace.yml`** - Tidy checks (clang-tidy, clippy, flake8)

### Option 1: Dockerfile in Namespace.so Dashboard

1. Go to your runner profile settings in Namespace.so
2. Enable "Custom Container Image"
3. Paste the Dockerfile contents
4. Replace `ARG BASE_IMAGE=ubuntu:26.04` with:
   ```dockerfile
   ARG BASE_IMAGE=${NAMESPACE_BASE_IMAGE_REF}
   ```

### Option 2: Push to Registry

```bash
# Tag for GitHub Container Registry
podman tag localhost/hogd-ci:latest ghcr.io/groundhogchain/hogd-ci:latest
# Login (need a GitHub PAT with packages:write)
podman login ghcr.io
# Push
podman push ghcr.io/groundhogchain/hogd-ci:latest
```

Then reference `ghcr.io/groundhogchain/hogd-ci:latest` in Namespace.so profile.

### Workflow Simplification

Once the custom base image is active, the workflows skip these steps (already in image):

- "Add LLVM Repository"
- "Install Build Dependencies"
- update-alternatives setup

Only these remain:
- SSH agent setup
- Checkout
- Submodule initialization
- `conf.sh` creation
- Build/test steps

## Toolchain Versions

- **Clang/LLVM**: 21 (from apt.llvm.org) - includes clang-format-21, clang-tidy-21, lld-21
- **GCC**: 15 (default in 26.04's archive)
- **Rust**: 1.93.0 (default, required by CMakeLists.txt + hogd/rust), 1.96.0 (pre-staged for the stable unification, with wasm32 + riscv32im targets), nightly-2026-02-12 (for wasm32 + build-std contracts)
- **cbindgen**: 0.29.2
- **Additional tools**: shfmt, wabt, binaryen
- **Ubuntu**: 26.04 (resolute)
