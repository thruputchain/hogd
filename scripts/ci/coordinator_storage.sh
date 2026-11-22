#!/usr/bin/env bash
# Helpers for the two artifact-only E2E Payment coordinator storage arms.

ci_coordinator_storage_arm() {
  case ${GIT_BRANCH:-} in
    codex/nightly-coordinator-storage-ram) printf '%s\n' ram ;;
    codex/nightly-coordinator-storage-disk) printf '%s\n' disk ;;
    *) return 1 ;;
  esac
}

ci_coordinator_storage_peer() {
  python3 /opt/hog-ci/peer.py hog4 "$1"
}

ci_coordinator_storage_prepare() {
  local medium=$1 root filesystem path
  case $medium in
    ram) root=/bench-ram/work_dir; filesystem=tmpfs ;;
    disk) root=/workspace; filesystem=ext4 ;;
    *) return 1 ;;
  esac

  path=$(ci_coordinator_storage_peer "
    set -euo pipefail
    root=$root
    [[ -d \$root && ! -L \$root && -O \$root && -w \$root ]]
    [[ \$(realpath -e -- \$root) == \$root ]]
    [[ \$(findmnt -n -o FSTYPE -T \$root) == $filesystem ]]
    [[ \$(findmnt -n -o TARGET -T \$root) == ${root%/work_dir} ]]
    umask 077
    dir=\$(mktemp -d -- \$root/chuckd-storage.XXXXXXXX)
    [[ -d \$dir && -O \$dir && ! -L \$dir ]]
    [[ \$(stat -c %a -- \$dir) == 700 && -z \$(ls -A -- \$dir) ]]
    [[ \$(findmnt -n -o FSTYPE -T \$dir) == $filesystem ]]
    printf '%s\\n' \"\$dir\"
  ")
  [[ $path =~ ^$root/chuckd-storage\.[[:alnum:]]{8}$ ]]
  printf 'host=hog4\nmedium=%s\nmount=%s\nfstype=%s\nowner_verified=1\n' \
    "$medium" "${root%/work_dir}" "$filesystem" \
    > "${HOG_DUMP_DIR:?}/nightly-coordinator-storage.txt"
  printf '%s\n' "$path"
}

ci_coordinator_storage_log_path() {
  local stream=$1 output=$2 key
  local -a paths
  case $stream in
    stdout) key=CONTROL_STDOUT_PATH ;;
    stderr) key=CONTROL_STDERR_PATH ;;
    *) return 1 ;;
  esac
  mapfile -t paths < <(sed -n "s/^$key=//p" <<<"$output")
  [[ ${#paths[@]} == 1 &&
      ${paths[0]} =~ ^/tmp/hogd_${stream}_[0-9]+_[0-9]+_[[:alnum:]]{6}$ ]] || return 1
  printf '%s\n' "${paths[0]}"
}

ci_coordinator_storage_parse_log() {
  local stream=$1 path=$2
  [[ $stream == stdout || $stream == stderr ]] || return 1
  [[ $path =~ ^/tmp/hogd_${stream}_[0-9]+_[0-9]+_[[:alnum:]]{6}$ ]] || return 1
  ci_coordinator_storage_peer "
    set -euo pipefail
    file=$path
    [[ -f \$file && ! -L \$file && -O \$file ]]
    [[ \$(realpath -e -- \$file) == \$file ]]
    python3 /workspace/scripts/ci/coordinator_storage_stages.py $stream < \$file
  "
}
