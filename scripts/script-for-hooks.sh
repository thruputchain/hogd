#!/bin/sh

# The [set-up-hooks.sh] script sets *this* script to run for certain git hooks.

REPO_ROOT="$(git rev-parse --show-toplevel)"

git submodule update --init --recursive
# Now fetch the rust crate dependencies.
"$REPO_ROOT/scripts/fetch.sh"
