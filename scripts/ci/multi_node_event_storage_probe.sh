#!/usr/bin/env bash
# Temporary artifact-only storage comparison. Both diagnostic branch refs use
# the same source SHA, stock host-LAN schedule and broad sanctioned tmpfs mount.
# Only the two Event data directories differ during the multi-node stage.

multi_node_storage_probe_mode() {
  case ${GIT_BRANCH:-} in
    codex/multinode-event-disk-probe) printf 'disk\n' ;;
    codex/multinode-event-tmpfs-probe|codex/multinode-remove-lock-probe) printf 'tmpfs\n' ;;
    *) return 1 ;;
  esac
}

multi_node_storage_probe_guard() {
  [[ ${HOGD_CI_CONTAINER:-0} == 1 && ${HOGD_CI_OFFLINE:-0} == 1 &&
     ${HOGD_COMBINED_WORK_DIR:-} == /bench-combined/ci_work_dir/ &&
     ${HOG_XID:-} =~ ^hogd-combined-metrics-[1-9][0-9]*-[1-9][0-9]*$ ]] || return 1
  multi_node_storage_probe_mode >/dev/null
}

multi_node_event_probe_dir() {
  local node=$1
  [[ $node == 0 || $node == 1 ]] || return 1
  multi_node_storage_probe_guard || return 1
  if [[ $(multi_node_storage_probe_mode) == tmpfs ]]; then
    printf '/bench-combined/multi-node-event-%s/node%s/event\n' "$HOG_XID" "$node"
  else
    printf '/workspace/ci_work_dir/mempool_multi_node_bench/node%s/event\n' "$node"
  fi
}

multi_node_probe_receipt() {
  local action=$1 node=$2 mode host disk_node baseline_file baseline= data_leaf= event_leaf= ram_parent= before= after= status= stopped=
  multi_node_storage_probe_guard
  mode=$(multi_node_storage_probe_mode)
  host=${HOG_HOST:?}
  case "$node:$host" in
    0:hog4|1:hog8) data_leaf=chuckd-storage ;;
    0:hog5|1:hog9) data_leaf=none ;;
    0:hog6|1:hog10) data_leaf=indexer; event_leaf=$(multi_node_event_probe_dir "$node") ;;
    0:hog7|1:hog11) data_leaf=hog-db ;;
    *) return 2 ;;
  esac
  disk_node="/workspace/ci_work_dir/mempool_multi_node_bench/node$node"
  [[ $(realpath -e /workspace) == /workspace &&
     $(findmnt -rn -o TARGET -T /workspace) == /workspace &&
     $(findmnt -rn -o FSTYPE -T /workspace) != tmpfs ]]
  [[ $(realpath -e "$disk_node") == "$disk_node" &&
     $(findmnt -rn -o TARGET -T "$disk_node") == /workspace ]]
  [[ $(realpath -e /bench-combined) == /bench-combined &&
     $(findmnt -rn -o TARGET,FSTYPE -T /bench-combined) == '/bench-combined tmpfs' &&
     $(stat -c %a /bench-combined) == 1777 &&
     $(df -Pk -- /bench-combined | awk 'NR==2{print $2}') == 50331648 ]]
  baseline_file="$disk_node/.multi-node-event-probe-baseline-$HOG_XID"
  before=$(df -Pk -- /bench-combined | awk 'NR==2{print $3}')
  [[ $before =~ ^[0-9]+$ ]]
  if [[ $action == prepare ]]; then
    # Broad tmpfs is shared with preceding stage scratch. Preserve it; only
    # insist on a fresh owned Event subtree and restore baseline allocation.
    [[ ! -e $baseline_file && ! -L $baseline_file ]]
    (set -o noclobber; printf '%s\n' "$before" >"$baseline_file")
    if [[ -n $event_leaf && $mode == tmpfs ]]; then
      ram_parent="/bench-combined/multi-node-event-$HOG_XID"
      [[ ! -e $ram_parent && ! -L $ram_parent ]]
      mkdir -m 700 -- "$ram_parent" "$ram_parent/node$node" "$event_leaf"
      [[ $(realpath -e "$event_leaf") == "$event_leaf" &&
         $(findmnt -rn -o TARGET,FSTYPE -T "$event_leaf") == '/bench-combined tmpfs' ]]
      [[ -z $(find "$event_leaf" -mindepth 1 -print -quit) ]]
    elif [[ -n $event_leaf ]]; then
      [[ $(realpath -e "$event_leaf") == "$event_leaf" &&
         $(findmnt -rn -o TARGET -T "$event_leaf") == /workspace ]]
      [[ -z $(find "$event_leaf" -mindepth 1 -print -quit) ]]
    fi
    printf 'MULTI_NODE_EVENT_STORAGE_V1 action=prepare mode=%s run=%s host=%s node=%s workspace_fstype=%s disk_root=%s data_leaf=%s event_path=%s ram_allocated_kib=%s\n' \
      "$mode" "$HOG_XID" "$host" "$node" "$(findmnt -rn -o FSTYPE -T /workspace)" "$disk_node" "$data_leaf" "${event_leaf:-none}" "$before"
    return
  fi
  [[ $action == reclaim && -f $baseline_file && ! -L $baseline_file &&
     $(realpath -e "$baseline_file") == "$baseline_file" ]]
  read -r baseline <"$baseline_file"
  [[ $baseline =~ ^[0-9]+$ ]]
  # The existing harness requests normal shutdown first; independently verify
  # all relevant benchmark writers have actually gone before inspecting data.
  for _ in {1..50}; do
    status=0
    pgrep -x 'loadgen_server|hog-exec|hog-event|hog-indexer|hog-db|hog-auth|hog-mempool|chuckd([.]dev)?|scpd([.]dev)?' >/dev/null || status=$?
    if [[ $status == 1 ]]; then stopped=yes; break; fi
    [[ $status == 0 ]]
    sleep .2
  done
  [[ $stopped == yes ]]
  before=$(df -Pk -- /bench-combined | awk 'NR==2{print $3}')
  [[ $before =~ ^[0-9]+$ ]]
  local data_kib=0 event_kib=0 data_present=no event_after_kib=0
  if [[ $data_leaf != none ]]; then
    if [[ -e $disk_node/$data_leaf || -L $disk_node/$data_leaf ]]; then
      [[ $(realpath -e "$disk_node/$data_leaf") == "$disk_node/$data_leaf" &&
         $(findmnt -rn -o TARGET -T "$disk_node/$data_leaf") == /workspace ]]
      data_kib=$(du -sk -- "$disk_node/$data_leaf" | awk '{print $1}')
      data_present=yes
    fi
  fi
  if [[ -n $event_leaf ]]; then
    [[ $(realpath -e "$event_leaf") == "$event_leaf" ]]
    event_kib=$(du -sk -- "$event_leaf" | awk '{print $1}')
    if [[ $mode == tmpfs ]]; then
      ram_parent="/bench-combined/multi-node-event-$HOG_XID"
      [[ $(findmnt -rn -o TARGET,FSTYPE -T "$event_leaf") == '/bench-combined tmpfs' ]]
      find "$event_leaf" -xdev -mindepth 1 -delete
      [[ -z $(find "$event_leaf" -mindepth 1 -print -quit) ]]
      rmdir -- "$event_leaf" "$ram_parent/node$node" "$ram_parent"
      [[ ! -e $ram_parent && ! -L $ram_parent ]]
    else
      [[ $(findmnt -rn -o TARGET -T "$event_leaf") == /workspace ]]
      event_after_kib=$event_kib
    fi
  fi
  after=$(df -Pk -- /bench-combined | awk 'NR==2{print $3}')
  [[ $after == "$baseline" ]]
  # Recheck any disk sibling; do not delete, relocate, or claim its bytes.
  if [[ $data_leaf != none && $data_present == yes ]]; then
    [[ $(realpath -e "$disk_node/$data_leaf") == "$disk_node/$data_leaf" &&
       $(du -sk -- "$disk_node/$data_leaf" | awk '{print $1}') == "$data_kib" ]]
  fi
  rm -- "$baseline_file"
  printf 'MULTI_NODE_EVENT_STORAGE_V1 action=reclaim mode=%s run=%s host=%s node=%s writers_stopped=yes disk_root=%s data_leaf=%s data_present=%s data_allocated_kib=%s event_path=%s event_allocated_kib=%s event_allocated_kib_after=%s ram_allocated_kib_baseline=%s ram_allocated_kib_before=%s ram_allocated_kib_after=%s\n' \
    "$mode" "$HOG_XID" "$host" "$node" "$disk_node" "$data_leaf" "$data_present" "$data_kib" "${event_leaf:-none}" "$event_kib" "$event_after_kib" "$baseline" "$before" "$after"
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
  set -euo pipefail
  [[ $# == 2 ]]
  multi_node_probe_receipt "$1" "$2"
fi
