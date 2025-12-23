#!/bin/bash
# local-ci.sh - Build and run HOGD CI tests locally using Docker

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
IMAGE_NAME="localhost/hogd-ci"
IMAGE_TAG="latest"
CONTAINER_NAME="hogd-ci-runner"

# Detect container runtime (prefer podman, fall back to docker)
if command -v podman &> /dev/null; then
    CONTAINER_CMD="podman"
elif command -v docker &> /dev/null; then
    CONTAINER_CMD="docker"
else
    echo "Error: Neither podman nor docker found in PATH"
    exit 1
fi

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

usage() {
    cat <<EOF
Usage: $0 [COMMAND] [OPTIONS]

Commands:
    build           Build the Docker image
    shell           Start interactive shell in container
    tidy            Run tidy checks (clang-tidy + clippy + flake8)
    full            Run full build + tidy checks
    clean           Remove Docker image and ccache volume

Options:
    -h, --help      Show this help message
    --no-cache      Build Docker image without cache
    --rebuild       Force rebuild of Docker image before running

Examples:
    $0 build                    # Build the Docker image
    $0 shell                    # Interactive shell for debugging
    $0 tidy                     # Run tidy checks only
    $0 full                     # Full build + tidy (like CI)
    $0 full --rebuild           # Rebuild image, then run full checks
EOF
}

log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }

# Build the Docker image
build_image() {
    local no_cache_flag=""
    if [[ "$1" == "--no-cache" ]]; then
        no_cache_flag="--no-cache"
    fi

    log_info "Building container image: ${IMAGE_NAME}:${IMAGE_TAG}"
    $CONTAINER_CMD build ${no_cache_flag} \
        --build-arg UID="$(id -u)" \
        --build-arg GID="$(id -g)" \
        -t "${IMAGE_NAME}:${IMAGE_TAG}" \
        -f "${SCRIPT_DIR}/Dockerfile" \
        "${SCRIPT_DIR}"
}

# Initialize submodules
init_submodules() {
    cd "${REPO_ROOT}"

    # Check if SSH agent is available for private repos
    if [[ -n "${SSH_AUTH_SOCK:-}" ]] && ssh-add -l &>/dev/null; then
        log_info "SSH agent detected - initializing ALL submodules..."
        git submodule update --init --recursive
    else
        log_info "No SSH agent - initializing public submodules only..."
        # Public submodules that don't require SSH keys
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
                git submodule update --init --recursive "${submodule}" 2>/dev/null || \
                    log_warn "Failed to init ${submodule} (may require SSH or be empty)"
            fi
        done
    fi
}

# Run container with proper mounts
run_container() {
    local cmd="${1:-/bin/bash}"
    local interactive_flag="${2:--it}"

    # Ensure cache volumes exist
    $CONTAINER_CMD volume create hogd-ccache 2>/dev/null || true
    $CONTAINER_CMD volume create hogd-cargo-registry 2>/dev/null || true
    $CONTAINER_CMD volume create hogd-cargo-git 2>/dev/null || true

    # For podman rootless, don't specify -u (runs as root inside, maps to user outside)
    # For docker, we'd want -u but podman handles this automatically
    local ssh_agent_args=""
    if [[ -n "${SSH_AUTH_SOCK:-}" ]]; then
        ssh_agent_args="-v ${SSH_AUTH_SOCK}:/ssh-agent:ro -e SSH_AUTH_SOCK=/ssh-agent"
    fi

    # Limit CPUs to avoid OOM with clang-tidy (~3GB per process)
    # Use RAM/3GB, capped at 8
    local mem_gb=$(awk '/MemTotal/ {print int($2/1024/1024)}' /proc/meminfo 2>/dev/null || echo 16)
    local max_cpus=$((mem_gb / 3))
    max_cpus=$((max_cpus > 8 ? 8 : max_cpus))
    max_cpus=$((max_cpus < 2 ? 2 : max_cpus))

    $CONTAINER_CMD run ${interactive_flag} --rm --replace \
        --name "${CONTAINER_NAME}" \
        --cpus="${max_cpus}" \
        -v "${REPO_ROOT}:/workspace:rw" \
        -v "hogd-ccache:/root/.cache/ccache:rw" \
        -v "hogd-cargo-registry:/usr/local/cargo/registry:rw" \
        -v "hogd-cargo-git:/usr/local/cargo/git:rw" \
        ${ssh_agent_args} \
        -e "CCACHE_DIR=/root/.cache/ccache" \
        -e "CCACHE_MAXSIZE=5G" \
        -e "CARGO_NET_GIT_FETCH_WITH_CLI=true" \
        -w /workspace \
        "${IMAGE_NAME}:${IMAGE_TAG}" \
        ${cmd}
}

# Create conf.sh for compiler configuration
create_conf_sh() {
    cat > "${REPO_ROOT}/scripts/conf.sh" <<'EOF'
GCC_CC=gcc-14
GCC_CXX=g++-14
CLANG_CC=/usr/bin/clang-21
CLANG_CXX=/usr/bin/clang++-21
EOF
    chmod +x "${REPO_ROOT}/scripts/conf.sh"
}

# Run tidy checks
run_tidy() {
    log_info "Running tidy checks..."
    init_submodules
    create_conf_sh

    # Create temp script to avoid quoting issues
    cat > "${REPO_ROOT}/.ci-tidy.sh" << 'TIDY_SCRIPT'
#!/bin/bash
set -e

echo "=== Using $(nproc) CPUs (container limited to prevent OOM) ==="

echo "=== Fetching Rust dependencies ==="
./scripts/fetch.sh

echo "=== Configuring build for tidy ==="
./scripts/build.sh -d build/llvm -t Debug -c clang -f "-Werror" -x "-DBUILD_BENCHMARKS=OFF" -n

echo "=== Building tidy targets ==="
make -j $(nproc) -C build/llvm tidy_targets

echo "=== Running tidy checks ==="
./scripts/tidy-ubuntu.sh -d build/llvm

echo "=== All tidy checks passed! ==="
TIDY_SCRIPT
    chmod +x "${REPO_ROOT}/.ci-tidy.sh"

    run_container "bash /workspace/.ci-tidy.sh"
    rm -f "${REPO_ROOT}/.ci-tidy.sh"
}

# Run full build + tidy
run_full() {
    log_info "Running full build + tidy checks..."
    init_submodules
    create_conf_sh

    # Create temp script to avoid quoting issues
    cat > "${REPO_ROOT}/.ci-full.sh" << 'FULL_SCRIPT'
#!/bin/bash
set -e

echo "=== Fetching Rust dependencies ==="
cd /workspace/hogd/rust && cargo fetch
cd /workspace

echo "=== Configuring Clang build ==="
./scripts/build.sh -d build/llvm -t Debug -c clang -f "-Werror" -x "-DBUILD_BENCHMARKS=OFF" -n

echo "=== Building tidy targets ==="
make -j $(nproc) -C build/llvm tidy_targets

echo "=== Running tidy checks ==="
./scripts/tidy-ubuntu.sh -d build/llvm

echo "=== All checks passed! ==="
FULL_SCRIPT
    chmod +x "${REPO_ROOT}/.ci-full.sh"

    run_container "bash /workspace/.ci-full.sh"
    rm -f "${REPO_ROOT}/.ci-full.sh"
}

# Clean up
clean() {
    log_info "Cleaning up container resources..."
    $CONTAINER_CMD rmi "${IMAGE_NAME}:${IMAGE_TAG}" 2>/dev/null || true
    $CONTAINER_CMD volume rm hogd-ccache hogd-cargo-registry hogd-cargo-git 2>/dev/null || true
    rm -rf "${REPO_ROOT}/build" 2>/dev/null || true
    rm -f "${REPO_ROOT}/scripts/conf.sh" 2>/dev/null || true
    log_info "Cleanup complete"
}

# Parse arguments
COMMAND=""
NO_CACHE=""
REBUILD=""

while [[ $# -gt 0 ]]; do
    case $1 in
        build|shell|tidy|full|clean)
            COMMAND="$1"
            shift
            ;;
        --no-cache)
            NO_CACHE="--no-cache"
            shift
            ;;
        --rebuild)
            REBUILD="yes"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            log_error "Unknown option: $1"
            usage
            exit 1
            ;;
    esac
done

# Container runtime already checked at the top

# Handle rebuild flag
if [[ -n "$REBUILD" && "$COMMAND" != "build" ]]; then
    build_image
fi

# Execute command
case "$COMMAND" in
    build)
        build_image "$NO_CACHE"
        ;;
    shell)
        run_container "/bin/bash" "-it"
        ;;
    tidy)
        run_tidy
        ;;
    full)
        run_full
        ;;
    clean)
        clean
        ;;
    "")
        usage
        exit 1
        ;;
esac
