#!/usr/bin/env bash

# Run before the C++ build, while the worker's installation token is fresh.
# Discover modules from the checked-out submodule rather than assuming its
# workspace layout. Keep workspace/local replacements active for each module.
set -euo pipefail

# [go mod download] reaches the module proxy for every module in the graph, and
# that transfer is the one part of this script with nothing local to fall back
# on. A single reset mid-download -- one host of a multi-host job losing its
# connection to proxy.golang.org for a moment -- failed that host's script and
# with it a benchmark run whose builds had not started yet.
#
# Retry the download instead. It resumes from the module cache, so an attempt
# that fetched most of the graph leaves less for the next one, and a failure
# that is not the network costs only the attempts: they are few, the waits
# between them are short, and the caller still sees the last attempt's status
# and go's own stderr.
FETCH_GO_MODULES_ATTEMPTS=${FETCH_GO_MODULES_ATTEMPTS:-3}
# Doubles per retry. Long enough for a blip to pass, short enough that the
# attempts against a proxy that is genuinely down do not eat the job's budget.
FETCH_GO_MODULES_RETRY_DELAY_SEC=${FETCH_GO_MODULES_RETRY_DELAY_SEC:-5}

go_mod_download() {
  local dir=$1 attempt=1 delay=$FETCH_GO_MODULES_RETRY_DELAY_SEC status

  while true; do
    status=0
    (cd "$dir" && go mod download) || status=$?

    if [[ "$status" -eq 0 ]]; then
      return 0
    fi
    if [[ "$attempt" -ge "$FETCH_GO_MODULES_ATTEMPTS" ]]; then
      echo "fetch_go_modules: [go mod download] in $dir failed (status $status)" \
        "after $attempt attempt(s)" >&2
      return "$status"
    fi

    echo "fetch_go_modules: [go mod download] in $dir failed (status $status);" \
      "retrying in ${delay}s (attempt $attempt/$FETCH_GO_MODULES_ATTEMPTS)" >&2
    sleep "$delay"
    attempt=$((attempt + 1))
    delay=$((delay * 2))
  done
}

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
found=false
while IFS= read -r -d '' manifest; do
  found=true
  go_mod_download "$(dirname "$manifest")"
done < <(find "$ROOT_DIR/groundhog-go" -name .git -prune -o -name vendor -prune -o -name go.mod -print0)

if [[ "$found" != true ]]; then
  echo "No Go modules found in groundhog-go; cannot prefetch chuckd/scpd dependencies" >&2
  exit 1
fi
