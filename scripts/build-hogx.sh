#!/usr/bin/env bash

set -xeuo pipefail

SCRIPT_DIR=$(dirname $(readlink -f "$0"))
ROOT=$(pwd)

cd ${SCRIPT_DIR}/../hogx

cargo --config net.git-fetch-with-cli=true fetch --manifest-path Cargo.toml
cargo build --offline --locked -p hogx-agent --bin hogx-agent
cargo build --offline --locked -p hogx-client --bin hogx
cp ./target/debug/hogx-agent ${ROOT}/hogx-agent
cp ./target/debug/hogx ${ROOT}/hogx
