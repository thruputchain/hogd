#!/bin/bash

CHUCKD_BINARY=~/go/bin/chuckd

if [ ! -x "$CHUCKD_BINARY" ]; then
  # Tell [go install] where the groundhog private repositories are.
  go env -w GOPRIVATE=github.com/groundhogchain/*
  # Tell [go install] to use SSH to access GitHub rather than HTTPS. This allows
  # it to use private key SSH authentication rather than requiring a private API
  # token from GitHub.
  git config --global url."ssh://git@github.com/".insteadOf "https://github.com/"
  # Install chuckd. This compiles the chuckd binary and places it at
  # [~/go/bin/chuckd].
  go install github.com/groundhogchain/chuckd/chuckd@latest
fi

# Execute chuckd.
"$CHUCKD_BINARY" help

# TODO(jhumphri): Launch the coordinator for the mempool.
# TODO(jhumphri): Launch the coordinator for the executor.
# TODO(jhumphri): Launch the mempool.
# TODO(jhumphri): Launch the executor.
# TODO(jhumphri): Launch the database that the mempool accesses.
# TODO(jhumphri): Launch the database that the executor accesses.
