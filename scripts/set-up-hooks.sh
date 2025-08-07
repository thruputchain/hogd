#!/bin/bash

SCRIPT_DIR=$(dirname "$(readlink -f "$0")")

cp ${SCRIPT_DIR}/post-merge.sh ${SCRIPT_DIR}/../.git/hooks/post-merge
chmod +x ${SCRIPT_DIR}/../.git/hooks/post-merge
