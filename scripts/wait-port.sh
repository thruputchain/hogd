#!/bin/bash

# Wait for a port to become available on a host
# Usage: ./scripts/wait-port.sh <host> <port>

set -e

if [ $# -lt 2 ]; then
    echo "Usage: $0 <host> <port>"
    echo "Example: $0 localhost 11111"
    exit 1
fi

HOST=$1
PORT=$2

echo "Waiting for $HOST:$PORT to become available..."

while true; do
    if nc -z "$HOST" "$PORT" 2>/dev/null; then
        echo "Port $HOST:$PORT is now available!"
        exit 0
    fi
    
    sleep 0.2
done
