#!/bin/sh

set -xeuo pipefail

SCRIPT_DIR=/tmp/hogjob-hogci/groundhog-go/chuckd/scripts

ROOT=$(pwd)
cd ${SCRIPT_DIR}/../chuckd
go build
ls -la
mv ./chuckd $ROOT/chuckd.dev
cd $ROOT
