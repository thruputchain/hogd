#!/bin/sh
set -e

# The [set-up-hooks.sh] script sets *this* script to run for certain git hooks.

REPO_ROOT="$(git rev-parse --show-toplevel)"

git -C "$REPO_ROOT" submodule update --init --recursive
# Now fetch the rust crate dependencies.
"$REPO_ROOT/scripts/fetch.sh"
