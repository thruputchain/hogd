#!/bin/bash

# Prints the byte size of each built contract's on-chain .rv32a artifact, one
# line per contract. Fails if any listed artifact is missing so a broken
# contract build cannot silently drop out of the metric history.

if [ $# -ne 1 ]; then
  echo "Usage: $0 <build_dir>"
  exit 1
fi

BUILD_DIR=$1

CONTRACTS=(
  fibonacci
  fibonacci_call
  itc_erc20
  authclaim_test
  cxs_roundtrip
  txs_roundtrip
  read_only_test
  manual_revert_test
  manual_revert_caller
  simple_wallet
  simple_wallet_authclaim
)

for contract in "${CONTRACTS[@]}"; do
  FILE="$BUILD_DIR/built_contracts/$contract.rv32a"
  if [ ! -f "$FILE" ]; then
    echo "File not found: $FILE" >&2
    exit 2
  fi
  if [[ "$(uname)" == "Darwin" ]]; then
    SIZE=$(stat -f%z "$FILE")
  else
    SIZE=$(stat -c%s "$FILE")
  fi
  echo "contract=$contract.rv32a contract_size=$SIZE"
done
