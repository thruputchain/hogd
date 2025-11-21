echo 'set port env vars'
export CI_CONTROL_PORT=$(./${BUILD_DIR}/hogd/utils/get_port kCIControlServerPort)

echo 'make sure bench servers are running'
./scripts/wait-port.sh hog11 ${CI_CONTROL_PORT}

echo 'clone metrics history' # This is idempotent
./scripts/clone_metrics_history.sh

echo 'run metrics'
./scripts/metrics.sh ${BUILD_DIR}

echo 'stop any running processes'
./${BUILD_DIR}/hogd/hogexec/bench/control_cli hog11 ${CI_CONTROL_PORT} stop
