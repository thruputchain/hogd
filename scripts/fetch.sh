#!/bin/bash

SCRIPT_DIR=$(dirname $(readlink -f "$0"))
ROOT_DIR=$(realpath ${SCRIPT_DIR}/..)

cd ${ROOT_DIR}/built_contracts && cargo fetch -Zbuild-std
cd ${ROOT_DIR}/built_contracts && cargo fetch -Zbuild-std --target wasm32-unknown-unknown
cd ${ROOT_DIR}/hogd/rust && cargo fetch
