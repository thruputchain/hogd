#!/bin/bash

# Script to save core dumps and their associated binaries
# Usage: save_core_dumps.sh <destination_directory>

if (($# < 1)); then
  echo "Usage: save_core_dumps.sh <destination_directory>" >&2
  exit 2
fi

dest_dir="$1"
mkdir -p "$dest_dir"

# Find all core dumps
core_count=0
while IFS= read -r -d '' core_file; do
  echo "Processing core dump: $core_file"

  # Get the base name for the core file
  core_basename=$(basename "$core_file")

  # Copy the core dump
  cp "$core_file" "$dest_dir/$core_basename"

  # Extract the executable path from the core dump
  exe=$(gdb -batch -q -c "$core_file" -ex 'info auxv' 2>/dev/null |
    sed -n 's/.*AT_EXECFN[^"]*"\(.*\)"$/\1/p')

  if [ -n "$exe" ] && [ -f "$exe" ]; then
    echo "  Associated binary: $exe"
    # Copy the binary with _bin suffix
    cp "$exe" "$dest_dir/${core_basename}_bin"
    echo "  Saved: $core_basename and ${core_basename}_bin"
  else
    echo "  Warning: Could not find associated binary for $core_file"
  fi

  ((core_count++))
done < <(find . -name '*.core' -print0)

echo "Processed $core_count core dump(s)"
exit 0
