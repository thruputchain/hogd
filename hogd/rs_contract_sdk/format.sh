#!/bin/bash

MANIFEST_PATH="./Cargo.toml"

CF_FLAGS="" # or "--check"

exec cargo +nightly fmt ${CF_FLAGS} \
  --manifest-path=${MANIFEST_PATH} \
  --message-format human \
  --all
