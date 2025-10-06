#!/bin/bash

# Get the repo root (absolute path)
REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" || {
  echo "Error: not inside a Git repository."
  exit 1
}

WATCHER_DIR="$REPO_ROOT"
BUILD_CMD="$REPO_ROOT/scripts/build.sh"
BUILD_ARGS=("$@") # capture all script arguments

# Make sure inotify-tools is installed
command -v inotifywait >/dev/null 2>&1 || {
  echo "Error: inotifywait not found. Install inotify-tools."
  exit 1
}

echo "Watching $WATCHER_DIR for changes to .cc, .h, .x, or .txt files..."
echo "Will kill any in-progress build and start a new one on each matching event."
echo "Build command: $BUILD_CMD ${BUILD_ARGS[*]}"

BUILD_PID=""
BUILD_PGID=""

start_build() {
  echo "[$(date +'%Y-%m-%d %H:%M:%S')] Starting build..."
  # Run build with arguments, in its own process group
  setsid bash -lc "exec $BUILD_CMD ${BUILD_ARGS[*]}" &
  BUILD_PID=$!
  BUILD_PGID=$BUILD_PID
}

stop_build() {
  if [[ -n "${BUILD_PID:-}" ]] && kill -0 "$BUILD_PID" 2>/dev/null; then
    echo "[$(date +'%Y-%m-%d %H:%M:%S')] Stopping running build (pid=$BUILD_PID)..."
    kill -TERM -"$BUILD_PGID" 2>/dev/null
    for _ in {1..30}; do
      sleep 0.1
      kill -0 "$BUILD_PID" 2>/dev/null || break
    done
    if kill -0 "$BUILD_PID" 2>/dev/null; then
      echo "Build didn't exit; force-killing..."
      kill -KILL -"$BUILD_PGID" 2>/dev/null
    fi
    wait "$BUILD_PID" 2>/dev/null
  fi
  BUILD_PID=""
  BUILD_PGID=""
}

cleanup() {
  echo
  echo "Stopping watches..."
  stop_build
  exit 0
}
trap cleanup INT TERM

# Start the build on launch.
start_build

# Watch for file changes.
while read -r file event; do
  case "$file" in
  *.cc | *.h | *.x | *.txt)
    echo "[$(date +'%Y-%m-%d %H:%M:%S')] Change detected: $event on $file"
    stop_build
    start_build
    ;;
  esac
done < <(inotifywait -m -r -e modify,delete --format '%w%f %e' "$WATCHER_DIR")
