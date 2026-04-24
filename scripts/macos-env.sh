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

# Activate Python venv
source "$PROJECT_ROOT/.venv/bin/activate"

# Add Homebrew LLVM to PATH (clang-21)
export PATH="/opt/homebrew/opt/llvm/bin:$PATH"

# Add Homebrew LLD linker to PATH
export PATH="/opt/homebrew/opt/lld/bin:$PATH"

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
echo "      If running 'make' directly, source this file first."
