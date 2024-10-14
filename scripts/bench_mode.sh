#!/bin/bash

# This script configures the machine for performance benchmarks. It only works
# on Linux and will block. If you send it SIGINT it will restore the config and
# exit.

# Set up environment
SCRIPT_DIR=$(dirname "$(readlink -f "$0")")

if [[ "$OSTYPE" != "linux-gnu"* ]]; then
  echo "Machine type unsupported"
  exit 1
fi

echo "Configuring machine for benchmarks..."
sudo cpupower frequency-set -g performance
echo "Press Ctrl-C to restore config."
sudo python3 ${SCRIPT_DIR}/cstate.py

echo "Restoring default power management configuration..."
sudo cpupower frequency-set -g powersave
