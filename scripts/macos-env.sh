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

# Default build parallelism, derived from performance cores and RAM. Raw core
# count oversubscribes Apple silicon (efficiency cores) and RAM (~0.9 GB per
# translation unit); see scripts/build-jobs.sh for the measurements.
if [ -f "$SCRIPT_DIR/build-jobs.sh" ]; then
  . "$SCRIPT_DIR/build-jobs.sh"
fi

echo "hogd development environment activated"
echo "  Python: $HOGD_PYTHON"
echo "  Clang:  $(clang --version | head -1)"
echo "  Rust:   $(rustc --version) (default)"
echo "  Jobs:   ${HOGD_BUILD_JOBS:-unset} (override with HOGD_BUILD_JOBS or build.sh -j)"
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
