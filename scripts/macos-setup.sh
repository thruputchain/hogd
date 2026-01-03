#!/bin/bash
# macOS Development Environment Setup for hogd
#
# This script sets up a development environment on macOS with minimal
# system-wide dependencies. It uses:
#   - Homebrew for native toolchain (llvm, bison, jemalloc, etc.)
#   - Python venv for Python dependencies (pyyaml, etc.)
#   - rustup for Rust toolchains
#
# Usage:
#   ./scripts/macos-setup.sh        # Check/install dependencies
#   source ./scripts/macos-env.sh   # Activate environment for current shell

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
VENV_DIR="$PROJECT_ROOT/.venv"

echo "=== hogd macOS Development Environment Setup ==="
echo ""

# Check for Homebrew
if ! command -v brew &> /dev/null; then
    echo "ERROR: Homebrew not found. Install from https://brew.sh/"
    exit 1
fi

echo "Checking Homebrew dependencies..."

# Required Homebrew packages
BREW_PACKAGES=(
    "llvm"        # clang-21, libc++, lld
    "bison"       # Parser generator (macOS version is outdated)
    "jemalloc"    # Memory allocator
    "openssl@3"   # TLS
    "lz4"         # Compression
    "zstd"        # Compression
    "wabt"        # WebAssembly tools (wat2wasm)
    "binaryen"    # WebAssembly optimizer (wasm-opt)
)

MISSING_PACKAGES=()
for pkg in "${BREW_PACKAGES[@]}"; do
    if ! brew list "$pkg" &> /dev/null; then
        MISSING_PACKAGES+=("$pkg")
    fi
done

if [ ${#MISSING_PACKAGES[@]} -gt 0 ]; then
    echo "Installing missing packages: ${MISSING_PACKAGES[*]}"
    brew install "${MISSING_PACKAGES[@]}"
else
    echo "All Homebrew packages installed."
fi

# Set up Python virtual environment
echo ""
echo "Setting up Python virtual environment..."
if [ ! -d "$VENV_DIR" ]; then
    python3 -m venv "$VENV_DIR"
    echo "Created venv at $VENV_DIR"
fi

# Install Python dependencies in venv
source "$VENV_DIR/bin/activate"
pip install --quiet --upgrade pip
pip install --quiet pyyaml flake8
echo "Python dependencies installed in venv."
deactivate

# Check Rust installation
echo ""
echo "Checking Rust installation..."
if ! command -v rustup &> /dev/null; then
    echo "ERROR: rustup not found. Install from https://rustup.rs/"
    exit 1
fi

# Install required Rust toolchains
RUST_TOOLCHAINS=("1.90.0" "stable" "nightly-2025-09-24")
for tc in "${RUST_TOOLCHAINS[@]}"; do
    if ! rustup toolchain list | grep -q "^$tc"; then
        echo "Installing Rust toolchain: $tc"
        rustup toolchain install "$tc"
    fi
done

# Install wasm32 target for nightly
if ! rustup target list --installed --toolchain nightly-2025-09-24 | grep -q wasm32-unknown-unknown; then
    echo "Installing wasm32-unknown-unknown target for nightly..."
    rustup target add wasm32-unknown-unknown --toolchain nightly-2025-09-24
fi

# Install rust-src for build-std
if ! rustup component list --installed --toolchain nightly-2025-09-24 | grep -q rust-src; then
    echo "Installing rust-src component for nightly..."
    rustup component add rust-src --toolchain nightly-2025-09-24
fi

# Install cbindgen
if ! command -v cbindgen &> /dev/null; then
    echo "Installing cbindgen..."
    cargo install cbindgen
fi

# Install clippy for required toolchains
for tc in "1.90.0" "stable"; do
    if ! rustup component list --installed --toolchain "$tc" | grep -q clippy; then
        echo "Installing clippy for $tc..."
        rustup component add clippy --toolchain "$tc"
    fi
done

echo "Rust toolchains configured."

# Generate the environment activation script
ENV_SCRIPT="$SCRIPT_DIR/macos-env.sh"
cat > "$ENV_SCRIPT" << 'EOF'
# macOS Development Environment for hogd
# Source this file: source ./scripts/macos-env.sh

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# Activate Python venv
source "$PROJECT_ROOT/.venv/bin/activate"

# Add Homebrew LLVM to PATH (clang-21)
export PATH="/opt/homebrew/opt/llvm/bin:$PATH"

# Add Homebrew bison to PATH (newer than macOS version)
export PATH="/opt/homebrew/opt/bison/bin:$PATH"

# Set compiler flags for LLVM
export CC="/opt/homebrew/opt/llvm/bin/clang"
export CXX="/opt/homebrew/opt/llvm/bin/clang++"
export LDFLAGS="-L/opt/homebrew/opt/llvm/lib/c++ -L/opt/homebrew/opt/llvm/lib"
export CPPFLAGS="-I/opt/homebrew/opt/llvm/include"

# For cmake to find packages
export CMAKE_PREFIX_PATH="/opt/homebrew/opt/llvm:/opt/homebrew/opt/openssl@3"

echo "hogd development environment activated"
echo "  Python: $(which python3)"
echo "  Clang:  $(clang --version | head -1)"
echo "  Rust:   $(rustc --version)"
EOF

echo ""
echo "=== Setup Complete ==="
echo ""
echo "To activate the environment for your current shell:"
echo "  source ./scripts/macos-env.sh"
echo ""
echo "Then configure and build:"
echo "  ./scripts/build.sh -n -d build -t Debug -c clang"
echo "  gmake -C build -j8"
