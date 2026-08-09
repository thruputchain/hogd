#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# On macOS, check if development environment is set up
if [[ "$OSTYPE" == "darwin"* ]]; then
  # Check if Python venv is activated by checking for pyyaml
  PYTHON_CHECK="${HOGD_PYTHON:-python3}"
  if ! "$PYTHON_CHECK" -c "import yaml" &>/dev/null; then
    # Try to auto-source the macos-env.sh if it exists
    MACOS_ENV_SCRIPT="$(dirname "$SCRIPT_DIR")/scripts/macos-env.sh"
    if [ -f "$MACOS_ENV_SCRIPT" ]; then
      echo "Automatically activating macOS development environment..."
      source "$MACOS_ENV_SCRIPT"

      # Verify it worked
      PYTHON_CHECK="${HOGD_PYTHON:-python3}"
      if ! "$PYTHON_CHECK" -c "import yaml" &>/dev/null; then
        echo ""
        echo "ERROR: Failed to activate development environment"
        echo "Please run: source ./scripts/macos-env.sh"
        exit 1
      fi
    else
      echo ""
      echo "=========================================="
      echo "ERROR: macOS development environment not set up"
      echo "=========================================="
      echo ""
      echo "You must activate the development environment before building."
      echo ""
      echo "Run this command:"
      echo "  source ./scripts/macos-env.sh"
      echo ""
      echo "If you haven't run setup yet, first run:"
      echo "  ./scripts/macos-setup.sh"
      echo ""
      exit 1
    fi
  fi
fi

source ${SCRIPT_DIR}/env.sh

unset NO_BUILD
CXX_FLAGS=
BUILD_DIR=build
BUILD_TYPE=Release
CMAKE_FLAGS=()
DISABLE_CCACHE=
BUILD_JOBS=
GENERATOR_FLAGS=()

print_usage() {
  printf "usage: scripts/build.sh [OPTION]...\n"
  printf -- "  -c sets the compiler (e.g., 'gcc', 'clang')\n"
  printf -- "  -f sets compiler flags (e.g., '-Werror')\n"
  printf -- "  -d sets the build directory (default is 'build')\n"
  printf -- "  -j sets the number of build jobs (default is NPROC)\n"
  printf -- "  -t sets the build type (can be 'Release' or 'Debug')\n"
  printf -- "  -x sets cmake additional flags\n"
  printf -- "  -n configure but don't run the build\n"
  printf -- "  -h disable ccache\n"
}

while getopts 'c:f:d:j:t:x:nh' flag; do
  case "${flag}" in
  c)
    if [[ "${OPTARG}" == "gcc" ]]; then
      CC="${GCC_CC}"
      CXX="${GCC_CXX}"
    elif [[ "${OPTARG}" == "clang" ]]; then
      CC="${CLANG_CC}"
      CXX="${CLANG_CXX}"
    fi
    ;;
  f) CXX_FLAGS="${OPTARG}" ;;
  d) BUILD_DIR="${OPTARG}" ;;
  j) BUILD_JOBS="${OPTARG}" ;;
  t)
    if [[ "${OPTARG}" == "Debug" || "${OPTARG}" == "Release" ]]; then
      BUILD_TYPE="${OPTARG}"
    else
      print_usage
      exit 1
    fi
    ;;
  x) CMAKE_FLAGS+=("${OPTARG}") ;;
  n) NO_BUILD=1 ;;
  h) DISABLE_CCACHE=1 ;;
  *)
    print_usage
    exit 1
    ;;
  esac
done

# Shift past the parsed options to get remaining arguments (build targets)
shift $((OPTIND - 1))

if [[ "$OSTYPE" == "darwin"* ]]; then
  if command -v xcrun >/dev/null 2>&1; then
    MACOS_SDKROOT="$(xcrun --sdk macosx --show-sdk-path 2>/dev/null || true)"
  fi
  if [[ -n "${MACOS_SDKROOT:-}" ]]; then
    CMAKE_INJECT=(-DCMAKE_OSX_SYSROOT="$MACOS_SDKROOT")
  fi
fi

HAS_GENERATOR_FLAG=
for flag in "${CMAKE_FLAGS[@]}"; do
  if [[ "$flag" == -G* ]]; then
    HAS_GENERATOR_FLAG=1
    break
  fi
done
if test -z "$HAS_GENERATOR_FLAG"; then
  if command -v ninja >/dev/null 2>&1; then
    GENERATOR_FLAGS=(-G Ninja)
  fi
fi

HAS_COLOR_DIAGNOSTICS_FLAG=
for flag in "${CMAKE_FLAGS[@]}"; do
  if [[ "$flag" == -DCMAKE_COLOR_DIAGNOSTICS=* ]]; then
    HAS_COLOR_DIAGNOSTICS_FLAG=1
    break
  fi
done
if test -z "$HAS_COLOR_DIAGNOSTICS_FLAG"; then
  CMAKE_FLAGS+=(-DCMAKE_COLOR_DIAGNOSTICS=ON)
fi

mkdir -p "$BUILD_DIR"

if test -z "$DISABLE_CCACHE" && command -v ccache >/dev/null 2>&1; then
  # Make cache keys portable across checkouts on different build hosts. Keep
  # explicit settings intact for callers that need different ccache behavior.
  BUILD_DIR_ABS="$(cd "$BUILD_DIR" && pwd -P)"
  export CCACHE_BASEDIR="${CCACHE_BASEDIR:-$BUILD_DIR_ABS:$ROOT_DIR}"
  export CCACHE_COMPILERCHECK="${CCACHE_COMPILERCHECK:-content}"
  ccache --max-size="${HOGD_CCACHE_MAXSIZE:-50G}"
  CMAKE_FLAGS+=(-DCMAKE_C_COMPILER_LAUNCHER=ccache
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
fi

# Do the build
CC="${CC}" CXX="${CXX}" $CMAKE -DCMAKE_CXX_FLAGS="${CXX_FLAGS}" \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -B "$BUILD_DIR" \
  "${GENERATOR_FLAGS[@]}" \
  "${CMAKE_INJECT[@]}" \
  "${CMAKE_FLAGS[@]}" \
  "$ROOT_DIR"
CONFIGURE_STATUS=$?

# A configure that failed generated no build system, so there is nothing left to
# do but report it. Under [-n] this script used to fall off the end with the
# status of its own [if], which bash makes zero when the condition is false: the
# caller was told the configure succeeded, and the failure resurfaced in a later
# step as
#
#     ninja: error: loading 'build.ninja': No such file or directory
#
# with cmake's own diagnostic sitting in a configure log that the caller never
# printed, because from its point of view the configure had passed.
if [[ "$CONFIGURE_STATUS" -ne 0 ]]; then
  echo "[$(date +'%Y-%m-%d %H:%M:%S')] Configure failed with exit code" \
    "$CONFIGURE_STATUS." >&2
  exit "$CONFIGURE_STATUS"
fi

if test -z "$NO_BUILD"; then
  if test -z "$BUILD_JOBS"; then
    BUILD_JOBS="$NPROC"
  fi

  BUILD_ARGS=(--build "$BUILD_DIR" --parallel "$BUILD_JOBS")
  if (($#)); then
    BUILD_ARGS+=(--target "$@")
  fi
  $CMAKE "${BUILD_ARGS[@]}"
  BUILD_STATUS=$?
  if [[ "$BUILD_STATUS" -eq 0 ]]; then
    echo "[$(date +'%Y-%m-%d %H:%M:%S')] Build completed successfully."
  else
    echo "[$(date +'%Y-%m-%d %H:%M:%S')] Build failed with exit code $BUILD_STATUS." >&2
  fi
  exit "$BUILD_STATUS"
fi

# Only the [-n] path reaches here, and it reaches it having configured cleanly.
exit 0
