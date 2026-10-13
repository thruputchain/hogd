#!/usr/bin/env bash

set +x
set -euo pipefail
umask 077

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <token> <config-path>" >&2
  exit 2
fi
token="$1"
config_path="$2"
# Preserve the bootstrap rewrite and other job-local settings.
touch "$config_path"
chmod 600 "$config_path"

# Full .git URLs prevent prefix collisions between protocol repositories.
# Explicit .git entries win over no-suffix HTTPS entries (longest match).
while read -r old new; do
  target="https://x-access-token:${token}@github.com/thruputchain/${new}.git"
  for source in "groundhogchain/$old" "thruputchain/$new"; do
    git config --file "$config_path" --add "url.${target}.insteadOf" "git@github.com:${source}.git"
    git config --file "$config_path" --add "url.${target}.insteadOf" "ssh://git@github.com/${source}.git"
    git config --file "$config_path" --add "url.${target}.insteadOf" "https://github.com/${source}.git"
    git config --file "$config_path" --add "url.${target}.insteadOf" "https://github.com/${source}"
  done
done <<'REPOSITORIES'
hogd hogd
chuckd chuckd
hogx hogx
protocol thruput-protocol
protocol-go thruput-protocol-go
protocol-rs thruput-protocol-rs
lib-go lib-go
groundhog-go thruput-go
integration_test_contracts thruput-integration-test-contracts
scp scp
sdk_rs thruput-sdk-rs
REPOSITORIES
