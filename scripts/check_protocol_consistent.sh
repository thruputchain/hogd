#!/bin/bash

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "Verifying protocol submodule commit synchronization..."
echo ""

# Get the commit hash for ./protocol
if [ ! -d "./protocol" ]; then
    echo -e "${RED}Error: ./protocol directory does not exist${NC}"
    exit 1
fi

cd ./protocol
PROTOCOL_COMMIT=$(git rev-parse HEAD)
PROTOCOL_COMMIT_SHORT=$(git rev-parse --short HEAD)
cd - > /dev/null

echo -e "📍 ./protocol commit: ${YELLOW}${PROTOCOL_COMMIT_SHORT}${NC} (${PROTOCOL_COMMIT})"

# Get the commit hash for ./groundhog-go/protocol-go/protocol
if [ ! -d "./groundhog-go/protocol-go/protocol" ]; then
    echo -e "${RED}Error: ./groundhog-go/protocol-go/protocol directory does not exist${NC}"
    exit 1
fi

cd ./groundhog-go/protocol-go/protocol
NESTED_PROTOCOL_COMMIT=$(git rev-parse HEAD)
NESTED_PROTOCOL_COMMIT_SHORT=$(git rev-parse --short HEAD)
cd - > /dev/null

echo -e "📍 ./groundhog-go/protocol-go/protocol commit: ${YELLOW}${NESTED_PROTOCOL_COMMIT_SHORT}${NC} (${NESTED_PROTOCOL_COMMIT})"
echo ""

# Compare the commits
if [ "$PROTOCOL_COMMIT" = "$NESTED_PROTOCOL_COMMIT" ]; then
    echo -e "${GREEN}✓ SUCCESS: Protocol submodule commits match!${NC}"
    exit 0
else
    echo -e "${RED}✗ FAILURE: Protocol submodule commits do NOT match!${NC}"
    echo ""
    echo "The two protocol submodules are pointing to different commits."
    echo "Please update them to point to the same commit."
    exit 1
fi