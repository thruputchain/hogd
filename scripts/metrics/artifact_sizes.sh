#!/bin/bash

if [ $# -ne 1 ]; then
  echo "Usage: $0 <build_dir>"
  exit 1
fi

BUILD_DIR=$1

$BUILD_DIR/hogd/hog_exec/bench/wasm_stats \
  $BUILD_DIR/built_contracts/fibonacci.wasm \
  $BUILD_DIR/built_contracts/fibonacci_call.wasm \
  $BUILD_DIR/built_contracts/itc_erc20.wasm \
  $BUILD_DIR/built_contracts/initial_token_claim.wasm \
  $BUILD_DIR/built_contracts/authclaim_test.wasm \
  $BUILD_DIR/built_contracts/cxs_roundtrip.wasm \
  $BUILD_DIR/built_contracts/txs_roundtrip.wasm \
  $BUILD_DIR/built_contracts/read_only_test.wasm \
  $BUILD_DIR/built_contracts/manual_revert_test.wasm \
  $BUILD_DIR/built_contracts/manual_revert_caller.wasm \
  $BUILD_DIR/built_contracts/simple_wallet.wasm \
  $BUILD_DIR/built_contracts/simple_wallet_authclaim.wasm
