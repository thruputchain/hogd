#!/usr/bin/env bash
set -Eeuo pipefail

echo 'set port env vars'
export CI_CONTROL_PORT=$(./${BUILD_DIR}/hogd/utils/get_port kCIControlServerPort)

cleanup() {
  local exit_code=$1

  echo 'cleanup control servers'
  ./$BUILD_DIR/hogd/hogexec/bench/control_cli hog1 $CI_CONTROL_PORT stop
  ./$BUILD_DIR/hogd/hogexec/bench/control_cli hog6 $CI_CONTROL_PORT stop
  ./$BUILD_DIR/hogd/hogexec/bench/control_cli hog7 $CI_CONTROL_PORT stop
  ./$BUILD_DIR/hogd/hogexec/bench/control_cli hog8 $CI_CONTROL_PORT stop
  ./$BUILD_DIR/hogd/hogexec/bench/control_cli hog11 $CI_CONTROL_PORT stop

  exit "$exit_code"
}

trap 'cleanup "$?"' EXIT
trap 'cleanup 130' INT TERM # 130 is conventional for SIGINT

echo 'make sure bench servers are running'
./scripts/wait-port.sh hog1 $CI_CONTROL_PORT
./scripts/wait-port.sh hog6 $CI_CONTROL_PORT
./scripts/wait-port.sh hog7 $CI_CONTROL_PORT
./scripts/wait-port.sh hog8 $CI_CONTROL_PORT
./scripts/wait-port.sh hog11 $CI_CONTROL_PORT

echo 'clone metrics history' # This is idempotent
./scripts/clone_metrics_history.sh

echo 'launch multiexec runner one exec'
./$BUILD_DIR/hogd/hogexec/bench/multiexec_runner -s hog7 -s hog8 -s hog1 -a 100 >multiexec_runner_log

echo 'launch multiexec runner two exec'
./$BUILD_DIR/hogd/hogexec/bench/multiexec_runner -s hog7 -s hog8 -s hog1 -s hog11 -a 100 >>multiexec_runner_log

echo 'launch multiexec runner three exec'
./$BUILD_DIR/hogd/hogexec/bench/multiexec_runner -s hog7 -s hog8 -s hog1 -s hog11 -s hog6 -a 100 >>multiexec_runner_log

echo 'log multiexec runner logs'
./scripts/log_metric.sh multiexec_interface multiexec_runner_log
