#!/usr/bin/env bash
# Artifact-only E2E Payment Event storage control.

ci_event_storage_arm() {
  case ${GIT_BRANCH:-} in
    codex/nightly-event-storage-ram) printf '%s\n' ram ;;
    codex/nightly-event-storage-disk) printf '%s\n' disk ;;
    *) return 1 ;;
  esac
}

ci_event_storage_peer() {
  python3 /opt/hog-ci/peer.py "$1" "$2"
}

ci_event_storage_prepare() {
  local medium=$1 root mount filesystem proof path
  case $medium in
    ram) root=/bench-ram/work_dir; mount=/bench-ram; filesystem=tmpfs ;;
    disk) root=/workspace; mount=/workspace; filesystem=ext4 ;;
    *) return 1 ;;
  esac

  proof=$(ci_event_storage_peer hog9 "
    set -euo pipefail
    root=$root
    [[ -d \$root && ! -L \$root && -O \$root && -w \$root ]]
    [[ \$(realpath -e -- \$root) == \$root ]]
    [[ \$(findmnt -n -o FSTYPE -T \$root) == $filesystem ]]
    [[ \$(findmnt -n -o TARGET -T \$root) == $mount ]]
    umask 077
    path=\$(mktemp -d -- \$root/nightly-event.XXXXXXXX)
    [[ -d \$path && ! -L \$path && -O \$path && -z \$(ls -A -- \$path) ]]
    [[ \$(stat -c %a -- \$path) == 700 && \$(realpath -e -- \$path) == \$path ]]
    filesystem=\$(findmnt -n -o FSTYPE -T \$path)
    mount=\$(findmnt -n -o TARGET -T \$path)
    [[ \$filesystem == $filesystem && \$mount == $mount ]]
    printf '%s\\n' \"\$path\" \"\$filesystem\" \"\$mount\" \"\$(stat -f -c %i -- \$path)\"
  ") || return
  local -a fields
  mapfile -t fields <<<"$proof"
  [[ ${#fields[@]} == 4 && ${fields[1]} == "$filesystem" && ${fields[2]} == "$mount" &&
     ${fields[3]} =~ ^[[:xdigit:]]+$ ]] || return 1
  path=${fields[0]}
  [[ $path =~ ^$root/nightly-event\.[[:alnum:]]{8}$ ]] || return 1
  printf 'host=hog9\nmedium=%s\nwork_dir=%s\nfstype=%s\nmount=%s\nfsid=%s\nowner_verified=1\n' \
    "$medium" "$path" "$filesystem" "$mount" "${fields[3]}" \
    > "${HOG_DUMP_DIR:?}/nightly-event-storage.txt" || return
  printf '%s\n' "$path"
}

ci_event_storage_log_path() {
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

ci_event_storage_read_log() {
  local mode=$1 path=$2 host stream
  case $mode in
    event) host=hog9; stream=stderr ;;
    coordinator) host=hog4; stream=stdout ;;
    *) return 1 ;;
  esac
  [[ $path =~ ^/tmp/hogd_${stream}_[0-9]+_[0-9]+_[[:alnum:]]{6}$ ]] || return 1
  ci_event_storage_peer "$host" "
    set -euo pipefail
    file=$path
    [[ -f \$file && ! -L \$file && -O \$file ]]
    [[ \$(realpath -e -- \$file) == \$file ]]
    python3 /workspace/scripts/ci/nightly_event_storage.py $mode < \$file
  "
}
