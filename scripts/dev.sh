#!/bin/bash
# dev.sh - Container-based development and CI for HOGD
#
# Usage:
#   ./scripts/dev.sh shell             # Interactive shell in container
#   ./scripts/dev.sh build ...         # Fetch deps + configure build
#   ./scripts/dev.sh <cmd>             # Run arbitrary command in container
#   ./scripts/dev.sh ci cpp            # Run C++ CI checks
#   ./scripts/dev.sh ci tidy           # Run tidy checks
#   ./scripts/dev.sh ci full           # Full CI run
#
# Recommended runtime:
#   - macOS: OrbStack (https://orbstack.dev) - fast, lightweight, Docker-compatible
#   - Linux: Docker or Podman (both work well natively)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
IMAGE_NAME="hogd-dev"
IMAGE_TAG="latest"
CONTAINER_NAME="hogd-dev"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log_info() { echo -e "${GREEN}[dev]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[dev]${NC} $1"; }
log_error() { echo -e "${RED}[dev]${NC} $1"; }

# Detect container runtime (prefer docker for OrbStack compatibility, fall back to podman)
detect_runtime() {
  if command -v docker &>/dev/null; then
    echo "docker"
  elif command -v podman &>/dev/null; then
    echo "podman"
  else
    log_error "No container runtime found." >&2
    echo "" >&2
    echo "Recommended setup:" >&2
    if [[ "$(uname -s)" == "Darwin" ]]; then
      echo "  macOS: Install OrbStack (https://orbstack.dev)" >&2
      echo "         brew install orbstack" >&2
    else
      echo "  Linux: Install Docker or Podman" >&2
      echo "         sudo apt install docker.io  # Debian/Ubuntu" >&2
      echo "         sudo dnf install podman     # Fedora" >&2
    fi
    exit 1
  fi
}

# Detect platform and get memory in GB
get_memory_gb() {
  if [[ "$(uname -s)" == "Darwin" ]]; then
    # macOS: sysctl returns bytes
    local mem_bytes
    mem_bytes=$(sysctl -n hw.memsize 2>/dev/null || echo 17179869184) # default 16GB
    echo $((mem_bytes / 1024 / 1024 / 1024))
  else
    # Linux: /proc/meminfo returns KB
    awk '/MemTotal/ {print int($2/1024/1024)}' /proc/meminfo 2>/dev/null || echo 16
  fi
}

# Get mount options for the platform
get_mount_options() {
  if [[ "$(uname -s)" == "Darwin" ]]; then
    # macOS: Use :cached for better I/O performance
    echo ":cached"
  else
    echo ""
  fi
}

# Ensure the container image exists
ensure_image() {
  local runtime="$1"
  local no_cache="${2:-}"

  if [[ -n "$no_cache" ]] || ! $runtime image inspect "${IMAGE_NAME}:${IMAGE_TAG}" &>/dev/null; then
    if [[ -z "$no_cache" ]]; then
      log_warn "Container image not found: ${IMAGE_NAME}:${IMAGE_TAG}"
    fi
    log_info "Building image..."

    local build_args=(
      build
      --build-arg "UID=$(id -u)"
      --build-arg "GID=$(id -g)"
      -t "${IMAGE_NAME}:${IMAGE_TAG}"
      -f "${REPO_ROOT}/.devcontainer/Dockerfile"
      "${REPO_ROOT}/.devcontainer"
    )

    if [[ -n "$no_cache" ]]; then
      build_args+=(--no-cache)
    fi

    $runtime "${build_args[@]}"
    log_info "Image built successfully"
  fi
}

# Initialize submodules based on SSH agent availability
init_submodules() {
  cd "${REPO_ROOT}"

  # Check if SSH agent is available for private repos
  if [[ -n "${SSH_AUTH_SOCK:-}" ]] && ssh-add -l &>/dev/null; then
    log_info "SSH agent detected - initializing ALL submodules..."
    git submodule update --init --recursive
  else
    log_info "No SSH agent - initializing public submodules only..."
    local public_submodules=(
      "external/rocksdb"
      "external/model-checker"
      "external/googletest"
      "external/benchmark"
      "external/crc32c"
      "external/blake3"
      "external/corrosion"
    )

    for submodule in "${public_submodules[@]}"; do
      if [[ ! -d "${REPO_ROOT}/${submodule}/.git" ]] && [[ ! -f "${REPO_ROOT}/${submodule}/.git" ]]; then
        log_info "Initializing ${submodule}..."
        git submodule update --init --recursive "${submodule}" 2>/dev/null ||
          log_warn "Failed to init ${submodule} (may require SSH or be empty)"
      fi
    done
  fi
}

# Main container run function
run_container() {
  local runtime="$1"
  shift
  local -a cmd_args=("$@")

  # Default to interactive shell if no command given
  if [[ ${#cmd_args[@]} -eq 0 ]]; then
    cmd_args=("/bin/bash")
  fi

  # Ensure cache volumes exist
  $runtime volume create hogd-ccache 2>/dev/null || true
  $runtime volume create hogd-cargo-registry 2>/dev/null || true
  $runtime volume create hogd-cargo-git 2>/dev/null || true

  # SSH agent forwarding
  local ssh_agent_args=""
  if [[ -n "${SSH_AUTH_SOCK:-}" ]]; then
    ssh_agent_args="-v ${SSH_AUTH_SOCK}:/ssh-agent:ro -e SSH_AUTH_SOCK=/ssh-agent"
  fi

  # Calculate resource limits
  local mem_gb
  mem_gb=$(get_memory_gb)

  # CPUs: RAM/5GB for clang memory requirements during parallel builds, capped at 6
  local max_cpus=$((mem_gb / 5))
  max_cpus=$((max_cpus > 6 ? 6 : max_cpus))
  max_cpus=$((max_cpus < 2 ? 2 : max_cpus))

  # Memory: leave 2GB for host, min 4GB for container
  local mem_limit_gb=$((mem_gb - 2))
  mem_limit_gb=$((mem_limit_gb < 4 ? 4 : mem_limit_gb))

  # Get platform-specific mount options
  local mount_opts
  mount_opts=$(get_mount_options)

  log_info "Container: ${max_cpus} CPUs, ${mem_limit_gb}GB RAM (host: ${mem_gb}GB)"

  # Build run arguments - use -it only if we have a TTY
  local tty_args="-it"
  if [[ ! -t 0 ]]; then
    tty_args=""
  fi

  local run_args=(
    run ${tty_args} --rm
    --name "${CONTAINER_NAME}"
    --user builder
    --cpus="${max_cpus}"
    --memory="${mem_limit_gb}g"
    --memory-swap="${mem_limit_gb}g"
    -v "${REPO_ROOT}:/workspace${mount_opts}"
    -v "hogd-ccache:/home/builder/.cache/ccache:rw"
    -v "hogd-cargo-registry:/usr/local/cargo/registry:rw"
    -v "hogd-cargo-git:/usr/local/cargo/git:rw"
    -e "CCACHE_DIR=/home/builder/.cache/ccache"
    -e "CCACHE_MAXSIZE=5G"
    -e "CARGO_NET_GIT_FETCH_WITH_CLI=true"
    -e "MAKEFLAGS=-j${max_cpus}"
    -e "CI_CPUS=${max_cpus}"
    -e "NPROC=${max_cpus}"
    -w /workspace
  )

  # Add SSH agent args if available
  if [[ -n "$ssh_agent_args" ]]; then
    # shellcheck disable=SC2206
    run_args+=($ssh_agent_args)
  fi

  # Add --replace for podman (removes existing container with same name)
  if [[ "$runtime" == "podman" ]]; then
    run_args+=(--replace)
  fi

  # Add image and command
  run_args+=("${IMAGE_NAME}:${IMAGE_TAG}")

  # Execute
  exec $runtime "${run_args[@]}" "${cmd_args[@]}"
}

# Run a CI workflow script inside the container
run_ci_script() {
  local runtime="$1"
  local script_content="$2"
  local extra_env="${3:-}"

  # Create temp script
  local temp_script="${SCRIPT_DIR}/.ci-temp.sh"
  cat >"$temp_script" <<EOF
#!/bin/bash
set -e

# Create conf.sh inside container
cat > ./scripts/conf.sh << 'CONF'
GCC_CC=gcc-15
GCC_CXX=g++-15
CLANG_CC=/usr/bin/clang-21
CLANG_CXX=/usr/bin/clang++-21
CONF
echo "NPROC=\${CI_CPUS:-\$(nproc)}" >> ./scripts/conf.sh
chmod +x ./scripts/conf.sh

echo "=== Using \${CI_CPUS:-\$(nproc)} CPUs (limited to prevent OOM) ==="

echo "=== Fetching Rust dependencies ==="
./scripts/fetch.sh

${script_content}
EOF
  chmod +x "$temp_script"

  # Build container args
  local -a container_args=(bash /workspace/scripts/.ci-temp.sh)

  # Run container (can't use run_container due to exec and cleanup needs)
  # Ensure cache volumes exist
  $runtime volume create hogd-ccache 2>/dev/null || true
  $runtime volume create hogd-cargo-registry 2>/dev/null || true
  $runtime volume create hogd-cargo-git 2>/dev/null || true

  local ssh_agent_args=""
  if [[ -n "${SSH_AUTH_SOCK:-}" ]]; then
    ssh_agent_args="-v ${SSH_AUTH_SOCK}:/ssh-agent:ro -e SSH_AUTH_SOCK=/ssh-agent"
  fi

  local mem_gb
  mem_gb=$(get_memory_gb)
  local max_cpus=$((mem_gb / 3))
  max_cpus=$((max_cpus > 8 ? 8 : max_cpus))
  max_cpus=$((max_cpus < 2 ? 2 : max_cpus))
  local mem_limit_gb=$((mem_gb - 2))
  mem_limit_gb=$((mem_limit_gb < 4 ? 4 : mem_limit_gb))
  local mount_opts
  mount_opts=$(get_mount_options)

  local tty_args="-it"
  if [[ ! -t 0 ]]; then
    tty_args=""
  fi

  log_info "Container: ${max_cpus} CPUs, ${mem_limit_gb}GB RAM (host: ${mem_gb}GB)"

  local result=0
  $runtime run ${tty_args} --rm \
    --name "${CONTAINER_NAME}" \
    --cpus="${max_cpus}" \
    --memory="${mem_limit_gb}g" \
    --memory-swap="${mem_limit_gb}g" \
    -v "${REPO_ROOT}:/workspace${mount_opts}" \
    -v "hogd-ccache:/root/.cache/ccache:rw" \
    -v "hogd-cargo-registry:/usr/local/cargo/registry:rw" \
    -v "hogd-cargo-git:/usr/local/cargo/git:rw" \
    ${ssh_agent_args} \
    ${extra_env} \
    -e "CCACHE_DIR=/root/.cache/ccache" \
    -e "CCACHE_MAXSIZE=5G" \
    -e "CARGO_NET_GIT_FETCH_WITH_CLI=true" \
    -e "MAKEFLAGS=-j${max_cpus}" \
    -e "CI_CPUS=${max_cpus}" \
    -e "NPROC=${max_cpus}" \
    -w /workspace \
    "${IMAGE_NAME}:${IMAGE_TAG}" \
    "${container_args[@]}" || result=$?

  # Cleanup
  rm -f "$temp_script"
  rm -f "${REPO_ROOT}/scripts/conf.sh"

  return $result
}

# CI: Run C++ checks
run_ci_cpp() {
  local runtime="$1"
  local no_test="$2"

  if [[ -n "$no_test" ]]; then
    log_info "Running C++ CI checks (format, build only - skipping tests)..."
  else
    log_info "Running C++ CI checks (format, build, test)..."
  fi
  init_submodules

  local script_content
  if [[ -n "$no_test" ]]; then
    script_content='
echo "=== Running format check ==="
./scripts/format.sh -c

echo "=== Building GCC Debug ==="
./scripts/build.sh -d bcheck/gcc -t Debug -c gcc -f "-Werror"

echo "=== Building LLVM Debug ==="
./scripts/build.sh -d bcheck/llvm -t Debug -c clang -f "-Werror"

echo "=== Building GCC Release ==="
./scripts/build.sh -d bcheck/gcc_release -t Release -c gcc

echo "=== ccache stats ==="
ccache -s

echo "=== All builds passed! ==="
'
  else
    script_content='
echo "=== Running C++ checks (format, build, test - skipping tidy) ==="
./scripts/check.sh -p -s

echo "=== All C++ checks passed! ==="
'
  fi

  run_ci_script "$runtime" "$script_content"
}

# CI: Run tidy checks
run_ci_tidy() {
  local runtime="$1"

  log_info "Running tidy checks..."
  init_submodules

  local script_content='
echo "=== Configuring build for tidy ==="
./scripts/build.sh -d build/llvm -t Debug -c clang -f "-Werror" -x "-DBUILD_BENCHMARKS=OFF" -n

echo "=== Building tidy targets ==="
cmake --build build/llvm --parallel "${CI_CPUS:-$(nproc)}" --target tidy_targets

echo "=== Running tidy checks ==="
./scripts/tidy-ubuntu.sh -d build/llvm

echo "=== All tidy checks passed! ==="
'

  run_ci_script "$runtime" "$script_content"
}

# CI: Run full checks
run_ci_full() {
  local runtime="$1"

  log_info "Running full build + tidy checks..."
  init_submodules

  local script_content='
echo "=== Configuring Clang build ==="
./scripts/build.sh -d build/llvm -t Debug -c clang -f "-Werror" -x "-DBUILD_BENCHMARKS=OFF" -n

echo "=== Building tidy targets ==="
cmake --build build/llvm --parallel "${CI_CPUS:-$(nproc)}" --target tidy_targets

echo "=== Running tidy checks ==="
./scripts/tidy-ubuntu.sh -d build/llvm

echo "=== All checks passed! ==="
'

  run_ci_script "$runtime" "$script_content"
}

# Clean up container resources
clean() {
  local runtime="$1"

  log_info "Cleaning up container resources..."
  $runtime rmi "${IMAGE_NAME}:${IMAGE_TAG}" 2>/dev/null || true
  $runtime volume rm hogd-ccache hogd-cargo-registry hogd-cargo-git 2>/dev/null || true
  rm -rf "${REPO_ROOT}/build" 2>/dev/null || true
  rm -f "${REPO_ROOT}/scripts/conf.sh" 2>/dev/null || true
  log_info "Cleanup complete"
}

# Show help
show_help() {
  cat <<EOF
Usage: $0 <command> [args...]

Container-based development and CI for HOGD.

Commands:
    shell               Start interactive shell in container
    build [ARGS]        Fetch deps + run ./scripts/build.sh
    test [ARGS]         Run the CMake test target
    <cmd>               Run arbitrary command in container

    ci cpp [--no-test]  Run C++ CI checks (format, build, test)
    ci tidy             Run tidy checks (clang-tidy, clippy, flake8)
    ci full             Run full CI (build + tidy)

    image               Build the container image
    image --no-cache    Rebuild image from scratch
    clean               Remove image and cache volumes

Examples:
    $0 shell                                # Interactive shell
    $0 build -n -d build -t Debug -c clang  # Configure clang debug build
    $0 build -d build -j8                   # Build the project
    $0 test                                 # Run tests
    $0 ci cpp                               # Run C++ CI locally
    $0 ci tidy                              # Run tidy checks locally

Environment:
    Container runtime: docker (OrbStack) or podman
    Image: ${IMAGE_NAME}:${IMAGE_TAG}

Volume mounts:
    Source:         /workspace
    ccache:         /root/.cache/ccache
    Cargo registry: /usr/local/cargo/registry
    Cargo git:      /usr/local/cargo/git
EOF
}

# Handle commands
handle_command() {
  local runtime="$1"
  shift

  case "${1:-}" in
  shell)
    run_container "$runtime"
    ;;
  build)
    shift
    run_container "$runtime" bash -c "./scripts/fetch.sh && ./scripts/build.sh $*"
    ;;
  test)
    shift
    run_container "$runtime" cmake --build build --target test "$@"
    ;;
  ci)
    shift
    case "${1:-}" in
    cpp)
      local no_test=""
      if [[ "${2:-}" == "--no-test" ]]; then
        no_test="yes"
      fi
      run_ci_cpp "$runtime" "$no_test"
      ;;
    tidy)
      run_ci_tidy "$runtime"
      ;;
    full)
      run_ci_full "$runtime"
      ;;
    *)
      log_error "Unknown CI command: ${1:-}"
      echo "Available: cpp, tidy, full"
      exit 1
      ;;
    esac
    ;;
  image)
    local no_cache=""
    if [[ "${2:-}" == "--no-cache" ]]; then
      no_cache="yes"
    fi
    ensure_image "$runtime" "$no_cache"
    ;;
  clean)
    clean "$runtime"
    ;;
  -h | --help | help)
    show_help
    ;;
  "")
    show_help
    exit 1
    ;;
  *)
    # Pass through any other command
    run_container "$runtime" "$@"
    ;;
  esac
}

# Main
main() {
  # Handle help before runtime detection
  if [[ "${1:-}" == "-h" || "${1:-}" == "--help" || "${1:-}" == "help" ]]; then
    show_help
    exit 0
  fi

  # Show help if no command given
  if [[ -z "${1:-}" ]]; then
    show_help
    exit 1
  fi

  local runtime
  runtime=$(detect_runtime)
  log_info "Using container runtime: $runtime"

  # For commands that need the image, ensure it exists
  case "${1:-}" in
  clean | image | -h | --help | help)
    # These don't need auto-build
    ;;
  *)
    ensure_image "$runtime"
    ;;
  esac

  handle_command "$runtime" "$@"
}

main "$@"
