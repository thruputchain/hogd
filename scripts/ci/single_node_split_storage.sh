#!/usr/bin/env bash
# Private confirmation branch only: split the broad Combined tmpfs stage
# into the same disk/RAM backing as stock single-node Combined.
set -euo pipefail

split_storage_role() {
  local action=$1 host=$2 ram_root=$3 disk_root=$4 run=$5
  local ram_leaf= disk_leaf= writers=
  case "$host" in
    hog4) ram_leaf="$ram_root/ci_work_dir/chuckd-storage"; writers='chuckd([.]dev)?|scpd([.]dev)?' ;;
    hog9) ram_leaf="$ram_root/ci_work_dir/event"; disk_leaf="$disk_root/.ci/single-node-split-$run/indexer"; writers='hog-event|hog-indexer' ;;
    hog11) disk_leaf="$disk_root/.ci/single-node-split-$run/db"; writers='hog-db' ;;
    *) return 2 ;;
  esac
  [[ $(realpath -e "$ram_root") == "$ram_root" &&
     $(findmnt -rn -o TARGET,FSTYPE -T "$ram_root") == "$ram_root tmpfs" ]]
  [[ $(realpath -e "$ram_root/ci_work_dir") == "$ram_root/ci_work_dir" &&
     $(findmnt -rn -o TARGET,FSTYPE -T "$ram_root/ci_work_dir") == "$ram_root tmpfs" ]]
  [[ $(realpath -e "$disk_root") == "$disk_root" &&
     $(findmnt -rn -o TARGET -T "$disk_root") == "$disk_root" &&
     $(findmnt -rn -o FSTYPE -T "$disk_root") != tmpfs ]]
  if [[ $action == prepare ]]; then
    if [[ -n $ram_leaf ]]; then
      [[ ! -e $ram_leaf && ! -L $ram_leaf ]]
      printf 'SINGLE_NODE_STORAGE role=%s action=prepare path=%s backing=tmpfs mount=%s\n' "$host" "$ram_leaf" "$ram_root"
      df -Pk -- "$ram_root"
    fi
    if [[ -n $disk_leaf ]]; then
      [[ $(realpath -e "$disk_root/.ci") == "$disk_root/.ci" &&
         $(findmnt -rn -o TARGET -T "$disk_root/.ci") == "$disk_root" ]]
      mkdir -m 700 -- "${disk_leaf%/*}"
      mkdir -m 700 -- "$disk_leaf"
      [[ $(realpath -e "$disk_leaf") == "$disk_leaf" &&
         $(findmnt -rn -o TARGET -T "$disk_leaf") == "$disk_root" &&
         $(findmnt -rn -o FSTYPE -T "$disk_leaf") != tmpfs ]]
      printf 'SINGLE_NODE_STORAGE role=%s action=prepare path=%s backing=%s mount=%s\n' "$host" "$disk_leaf" "$(findmnt -rn -o FSTYPE -T "$disk_leaf")" "$disk_root"
    fi
  elif [[ $action == reclaim ]]; then
    local status=0
    pgrep -x "$writers" >/dev/null || status=$?
    [[ $status == 1 ]]
    # The normal stop/reap already ran. The fresh disk leaves stay isolated
    # from subsequent stages until the ordinary whole-run workspace cleanup.
    if [[ -n $ram_leaf && -d $ram_leaf ]]; then
      [[ $(realpath -e "$ram_leaf") == "$ram_leaf" &&
         $(findmnt -rn -o TARGET,FSTYPE -T "$ram_leaf") == "$ram_root tmpfs" ]]
      df -Pk -- "$ram_root"
      find "$ram_leaf" -xdev -mindepth 1 -delete
      rmdir -- "$ram_leaf"
    fi
    [[ -z $ram_leaf || ( ! -e $ram_leaf && ! -L $ram_leaf ) ]]
    printf 'SINGLE_NODE_STORAGE role=%s action=reclaim writers_stopped=yes ram_leaf_removed=yes ram_path=%s disk_path_retained=%s\n' "$host" "${ram_leaf:-none}" "${disk_leaf:-none}"
    df -Pk -- "$ram_root"
  else
    return 2
  fi
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
  [[ $# == 1 && ${HOGD_CI_CONTAINER:-0} == 1 &&
     ${HOGD_CI_OFFLINE:-0} == 1 &&
     ${HOGD_COMBINED_WORK_DIR:-} == /bench-combined/ci_work_dir/ &&
     ( ${GIT_BRANCH:-} == codex/single-commit-stock-storage-probe ||
        ${GIT_BRANCH:-} == codex/multinode-event-disk-probe ||
        ${GIT_BRANCH:-} == codex/multinode-event-tmpfs-probe ||
        ${GIT_BRANCH:-} == codex/multinode-remove-lock-probe ||
        ${GIT_BRANCH:-} == codex/multinode-cache-erase-detail-probe ) &&
     ${HOG_XID:-} =~ ^hogd-combined-metrics-[1-9][0-9]*-[1-9][0-9]*$ ]]
  split_storage_role "$1" "${HOG_HOST:?}" /bench-combined /workspace "$HOG_XID"
fi
