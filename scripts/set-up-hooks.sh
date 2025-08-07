#!/bin/bash

SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
POST_CHECKOUT=${SCRIPT_DIR}/../.git/hooks/post-checkout
POST_MERGE=${SCRIPT_DIR}/../.git/hooks/post-merge

cp ${SCRIPT_DIR}/pull-submodules.sh ${POST_CHECKOUT}
chmod +x ${POST_CHECKOUT}

cp ${SCRIPT_DIR}/pull-submodules.sh ${POST_MERGE}
chmod +x ${POST_MERGE}
