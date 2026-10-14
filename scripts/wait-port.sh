#!/bin/bash

# Wait for a port to become available on a host.
# Usage: ./scripts/wait-port.sh <host> <port> [timeout-sec]

set -e

if [ $# -lt 2 ]; then
  echo "Usage: $0 <host> <port> [timeout-sec]"
  echo "Example: $0 localhost 11111 60"
  exit 1
fi

HOST=$1
PORT=$2
TIMEOUT_SEC=${3:-${CI_WAIT_PORT_TIMEOUT_SEC:-60}}
START_SECONDS=$SECONDS

echo "Waiting for $HOST:$PORT to become available (timeout: ${TIMEOUT_SEC}s)..."

while true; do
  if nc -z "$HOST" "$PORT" 2>/dev/null; then
    echo "Port $HOST:$PORT is now available!"
    exit 0
  fi

  if [ "$TIMEOUT_SEC" != "0" ] && [ $((SECONDS - START_SECONDS)) -ge "$TIMEOUT_SEC" ]; then
    echo "Timed out waiting for $HOST:$PORT after ${TIMEOUT_SEC}s"
    exit 1
  fi

  sleep 0.2
done
