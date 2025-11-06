#!/bin/bash

if (($# < 1)); then
  echo "Usage: gdbc.sh [gdb options] <core-file>" >&2
  exit 2
fi

core="${@: -1}"
args=("${@:1:$#-1}")

# Make sure we can read file
: <"$core" || exit 1

exe=$(gdb -batch -q -c "$core" -ex 'info auxv' 2>/dev/null |
  sed -n 's/.*AT_EXECFN[^"]*"\(.*\)"$/\1/p')

echo $exe
