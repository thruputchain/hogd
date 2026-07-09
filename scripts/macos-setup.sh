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
if ! command -v brew &>/dev/null; then
  echo "ERROR: Homebrew not found. Install from https://brew.sh/"
  exit 1
fi

echo "Checking Homebrew dependencies..."

# Required Homebrew packages
BREW_PACKAGES=(
  "llvm"      # clang-21, libc++
  "lld"       # LLVM linker (separate from llvm package on Homebrew)
  "cmake"     # Build system generator
  "ninja"     # Default CMake build system
  "bison"     # Parser generator (macOS version is outdated)
  "python"    # Python for venv-backed build tools
  "jemalloc"  # Memory allocator
  "openssl@3" # TLS
  "zlib"      # Compression
  "bzip2"     # Compression
  "lz4"       # Compression
  "zstd"      # Compression
  "snappy"    # Compression (for RocksDB)
  "wabt"      # WebAssembly tools (wat2wasm)
  "binaryen"  # WebAssembly optimizer (wasm-opt)
  "ccache"    # CCache (for faster builds)
)

MISSING_PACKAGES=()
for pkg in "${BREW_PACKAGES[@]}"; do
  if ! brew list "$pkg" &>/dev/null; then
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
  BREW_PYTHON="$(brew --prefix python)/bin/python3"
  "$BREW_PYTHON" -m venv "$VENV_DIR"
  echo "Created venv at $VENV_DIR"
fi
if [ ! -e "$VENV_DIR/bin/python3" ] && [ -x "$VENV_DIR/bin/python" ]; then
  ln -s python "$VENV_DIR/bin/python3"
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
if ! command -v rustup &>/dev/null; then
  echo ""
  echo "=========================================="
  echo "ERROR: rustup is not installed"
  echo "=========================================="
  echo ""
  echo "rustup is required to install and manage Rust toolchains."
  echo ""
  echo "To install rustup, run:"
  echo ""
  echo "  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh"
  echo ""
  echo "Or visit https://rustup.rs/ for more information."
  echo ""
  echo "After installation, restart your shell or run:"
  echo "  source \$HOME/.cargo/env"
  echo ""
  echo "Then re-run this setup script."
  echo ""
  exit 1
fi

# Install required Rust toolchains. Keep in sync with the in-repo
# rust-toolchain.toml files and .devcontainer/Dockerfile:
#   - 1.93.0             hogd/rust + CMakeLists Rust_TOOLCHAIN
#   - 1.96.0             pre-staged for the planned stable-Rust unification
#   - nightly-2026-02-12 built_contracts (wasm32 + build-std)
RUST_TOOLCHAINS=("1.93.0" "1.96.0" "nightly-2026-02-12")
for tc in "${RUST_TOOLCHAINS[@]}"; do
  if ! rustup toolchain list | grep -q "^$tc"; then
    echo "Installing Rust toolchain: $tc"
    rustup toolchain install "$tc"
  fi
done

# Install wasm32 target for nightly
if ! rustup target list --installed --toolchain nightly-2026-02-12 | grep -q wasm32-unknown-unknown; then
  echo "Installing wasm32-unknown-unknown target for nightly..."
  rustup target add wasm32-unknown-unknown --toolchain nightly-2026-02-12
fi

# Install rust-src for build-std
if ! rustup component list --installed --toolchain nightly-2026-02-12 | grep -q rust-src; then
  echo "Installing rust-src component for nightly..."
  rustup component add rust-src --toolchain nightly-2026-02-12
fi

# Install the wasm + rv32 contract backends for 1.96 (the planned stable unification)
for target in wasm32-unknown-unknown riscv32im-unknown-none-elf; do
  if ! rustup target list --installed --toolchain 1.96.0 | grep -q "$target"; then
    echo "Installing $target target for 1.96.0..."
    rustup target add "$target" --toolchain 1.96.0
  fi
done

# Install cbindgen
CBINDGEN_VERSION=0.29.2
if command -v cbindgen >/dev/null 2>&1 && cbindgen --version | grep -qx "cbindgen ${CBINDGEN_VERSION}"; then
  echo "cbindgen ${CBINDGEN_VERSION} already installed"
else
  echo "Installing cbindgen ${CBINDGEN_VERSION}..."
  cargo install --locked --force --version "${CBINDGEN_VERSION}" cbindgen
fi

# Install bindgen CLI for test_ffi Rust bindings generation
if ! command -v bindgen &>/dev/null; then
  echo "Installing bindgen-cli..."
  cargo install bindgen-cli
fi

# Install clippy for required toolchains
for tc in "1.93.0" "1.96.0"; do
  if ! rustup component list --installed --toolchain "$tc" | grep -q clippy; then
    echo "Installing clippy for $tc..."
    rustup component add clippy --toolchain "$tc"
  fi
done

echo "Rust toolchains configured."

# Fetch cargo dependencies (requires rust-src to be installed first)
echo ""
echo "Fetching cargo dependencies..."
"$SCRIPT_DIR/fetch.sh"

# Generate the environment activation script
ENV_SCRIPT="$SCRIPT_DIR/macos-env.sh"
cat >"$ENV_SCRIPT" <<'EOF'
# macOS Development Environment for hogd
# Source this file: source ./scripts/macos-env.sh

if [ -n "${ZSH_VERSION:-}" ]; then
  eval 'SCRIPT_PATH="${(%):-%x}"'
elif [ -n "${BASH_SOURCE[0]:-}" ]; then
  SCRIPT_PATH="${BASH_SOURCE[0]}"
else
  SCRIPT_PATH="$0"
fi

SCRIPT_DIR="$(cd "$(dirname "$SCRIPT_PATH")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
VENV_DIR="$PROJECT_ROOT/.venv"

# Activate Python venv
if [ ! -d "$VENV_DIR" ]; then
  echo "ERROR: Python virtual environment not found at $VENV_DIR"
  echo "Run: ./scripts/macos-setup.sh"
  return 1 2>/dev/null || exit 1
fi

if [ ! -e "$VENV_DIR/bin/python3" ] && [ -x "$VENV_DIR/bin/python" ]; then
  ln -s python "$VENV_DIR/bin/python3" 2>/dev/null || true
fi

if [ -x "$VENV_DIR/bin/python3" ]; then
  export HOGD_PYTHON="$VENV_DIR/bin/python3"
else
  export HOGD_PYTHON="$VENV_DIR/bin/python"
fi

source "$VENV_DIR/bin/activate"
export VIRTUAL_ENV="$VENV_DIR"
export PATH="$VENV_DIR/bin:$PATH"
if [ ! -x "$VENV_DIR/bin/python3" ] && [ -x "$VENV_DIR/bin/python" ]; then
  HOGD_PYTHON_SHIM_DIR="${TMPDIR:-/tmp}/hogd-python-${USER:-$(id -u)}"
  mkdir -p "$HOGD_PYTHON_SHIM_DIR"
  cat >"$HOGD_PYTHON_SHIM_DIR/python3" <<PYTHON3_SHIM
#!/bin/sh
exec "$VENV_DIR/bin/python" "\$@"
PYTHON3_SHIM
  chmod +x "$HOGD_PYTHON_SHIM_DIR/python3"
  export PATH="$HOGD_PYTHON_SHIM_DIR:$PATH"
fi

# Add Homebrew LLVM to PATH (clang-21)
export PATH="/opt/homebrew/opt/llvm/bin:$PATH"

# Add Homebrew LLD linker to PATH
export PATH="/opt/homebrew/opt/lld/bin:$PATH"

# Add Homebrew bison to PATH (newer than macOS version)
export PATH="/opt/homebrew/opt/bison/bin:$PATH"

# Some venv activation scripts contain stale absolute paths after checkout moves.
export PATH="$VENV_DIR/bin:$PATH"

# Set compiler flags for LLVM
export CC="/opt/homebrew/opt/llvm/bin/clang"
export CXX="/opt/homebrew/opt/llvm/bin/clang++"
export LDFLAGS="-L/opt/homebrew/opt/llvm/lib/c++ -L/opt/homebrew/opt/llvm/lib"
export CPPFLAGS="-I/opt/homebrew/opt/llvm/include"

# For cmake to find packages
export CMAKE_PREFIX_PATH="/opt/homebrew/opt/llvm:/opt/homebrew/opt/openssl@3:/opt/homebrew/opt/zlib:/opt/homebrew/opt/bzip2:/opt/homebrew/opt/lz4:/opt/homebrew/opt/zstd:/opt/homebrew/opt/snappy:/opt/homebrew/opt/jemalloc"

echo "hogd development environment activated"
echo "  Python: $HOGD_PYTHON"
echo "  Clang:  $(clang --version | head -1)"
echo "  Rust:   $(rustc --version) (default)"
# Show the project toolchain version to verify rustup is working
TOOLCHAIN_VERSION=$(grep 'channel' "$PROJECT_ROOT/hogd/rust/rust-toolchain.toml" 2>/dev/null | cut -d'"' -f2)
if [ -n "$TOOLCHAIN_VERSION" ]; then
  TOOLCHAIN_RUSTC=$(rustc +"$TOOLCHAIN_VERSION" --version 2>/dev/null)
  if [ -n "$TOOLCHAIN_RUSTC" ]; then
    echo "          $TOOLCHAIN_RUSTC (project toolchain)"
  else
    echo "          WARNING: toolchain $TOOLCHAIN_VERSION not installed"
  fi
fi
echo ""
echo "Note: ./scripts/build.sh automatically activates this environment."
echo "      If running 'cmake --build' directly, source this file first."
EOF

echo ""
echo "=========================================="
echo "=== Setup Complete ==="
echo "=========================================="
echo ""
echo "IMPORTANT: You must activate the development environment before building!"
echo ""
echo "Run this command in your shell:"
echo "  source ./scripts/macos-env.sh"
echo ""
echo "This activates the Python venv and sets up the build environment."
echo "You need to source this file in each new shell session."
echo ""
echo "Then configure and build:"
echo "  ./scripts/build.sh -n -d build -t Debug -c clang"
echo "  cmake --build build --parallel 8"
echo ""
echo "Tip: Add this to your shell profile (~/.zshrc or ~/.bashrc):"
echo "  alias hogd-env='source \$(git rev-parse --show-toplevel)/scripts/macos-env.sh'"
echo ""
