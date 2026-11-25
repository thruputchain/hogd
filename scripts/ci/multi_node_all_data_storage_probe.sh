#!/usr/bin/env bash
# Artifact-only third arm of the paired multi-node storage diagnostic. The
# broad trusted manual mount already exists; this owns only a stage subtree.

multi_node_all_data_dir() {
  local node=$1 leaf=$2
  [[ ${GIT_BRANCH:-} == codex/multinode-all-data-tmpfs-probe &&
     ${HOGD_CI_CONTAINER:-0} == 1 && ${HOGD_CI_OFFLINE:-0} == 1 &&
     ${HOGD_COMBINED_WORK_DIR:-} == /bench-combined/ci_work_dir/ &&
     ${HOG_XID:-} =~ ^hogd-combined-metrics-[1-9][0-9]*-[1-9][0-9]*$ &&
     ( $node == 0 || $node == 1 ) ]] || return 1
  case "$leaf" in event|indexer|hog-db|chuckd-storage) ;; *) return 2 ;; esac
  printf '/bench-combined/multi-node-all-data-%s/node%s/%s\n' "$HOG_XID" "$node" "$leaf"
}

multi_node_all_data_host() {
  case "$1:$2" in
    0:hog4|0:hog6|0:hog7|1:hog8|1:hog10|1:hog11) return 0 ;;
    *) return 1 ;;
  esac
}

multi_node_all_data_receipt() {
  local action=$1 node=$2 host=${HOG_HOST:?} disk_node meta parent ram_node baseline before after status stopped=no peak samples leaf kib final_kib=0
  local -a leaves=()
  # Path helper performs all branch/run guards, including fixed leaf names.
  multi_node_all_data_dir "$node" event >/dev/null
  case "$node:$host" in
    0:hog4|1:hog8) leaves=(chuckd-storage) ;;
    0:hog5|1:hog9) leaves=() ;;
    0:hog6|1:hog10) leaves=(event indexer) ;;
    0:hog7|1:hog11) leaves=(hog-db) ;;
    *) return 2 ;;
  esac
  disk_node="/workspace/ci_work_dir/mempool_multi_node_bench/node$node"
  meta="$disk_node/.multi-node-all-data-$HOG_XID"
  parent="/bench-combined/multi-node-all-data-$HOG_XID"
  ram_node="$parent/node$node"
  [[ $(realpath -e /workspace) == /workspace &&
     $(findmnt -rn -o TARGET -T /workspace) == /workspace &&
     $(findmnt -rn -o FSTYPE -T /workspace) != tmpfs &&
     $(realpath -e "$disk_node") == "$disk_node" &&
     $(findmnt -rn -o TARGET -T "$disk_node") == /workspace &&
     $(realpath -e /bench-combined) == /bench-combined &&
     $(findmnt -rn -o TARGET,FSTYPE -T /bench-combined) == '/bench-combined tmpfs' &&
     $(stat -c %a /bench-combined) == 1777 &&
     $(df -Pk -- /bench-combined | awk 'NR==2{print $2}') == 50331648 ]]
  before=$(df -Pk -- /bench-combined | awk 'NR==2{print $3}')
  [[ $before =~ ^[0-9]+$ ]]

  # Compare only direct sibling identities, never traverse sibling data.
  all_data_siblings() {
    find /bench-combined -mindepth 1 -maxdepth 1 ! -name "${parent##*/}" -printf '%f\t%i\n' | LC_ALL=C sort
  }

  if [[ $action == prepare ]]; then
    [[ ! -e $meta && ! -L $meta && ! -e $parent && ! -L $parent ]]
    mkdir -m 700 -- "$meta"
    printf '%s\n' "$before" >"$meta/baseline"
    all_data_siblings >"$meta/siblings"
    if ((${#leaves[@]})); then
      mkdir -m 700 -- "$parent" "$ram_node"
      for leaf in "${leaves[@]}"; do
        # Event's stock disk path is an empty placeholder created by the
        # unchanged stage setup. All other persistent disk leaves are absent.
        if [[ $leaf == event ]]; then
          [[ -d $disk_node/$leaf && ! -L $disk_node/$leaf &&
             $(realpath -e "$disk_node/$leaf") == "$disk_node/$leaf" &&
             -z $(find "$disk_node/$leaf" -mindepth 1 -print -quit) ]]
        else
          [[ ! -e $disk_node/$leaf && ! -L $disk_node/$leaf ]]
        fi
        mkdir -m 700 -- "$ram_node/$leaf"
        [[ $(realpath -e "$ram_node/$leaf") == "$ram_node/$leaf" &&
           $(findmnt -rn -o TARGET,FSTYPE -T "$ram_node/$leaf") == '/bench-combined tmpfs' ]]
        printf 'MULTI_NODE_ALL_DATA_LEAF_V1 action=prepare run=%s host=%s node=%s leaf=%s path=%s fstype=tmpfs\n' \
          "$HOG_XID" "$host" "$node" "$leaf" "$ram_node/$leaf"
      done
    fi
    printf 'MULTI_NODE_ALL_DATA_V1 action=prepare run=%s host=%s node=%s workspace_fstype=%s disk_root=%s leaf_count=%s capacity_kib=50331648 baseline_kib=%s\n' \
      "$HOG_XID" "$host" "$node" "$(findmnt -rn -o FSTYPE -T /workspace)" "$disk_node" "${#leaves[@]}" "$before"
    return
  fi

  [[ -d $meta && ! -L $meta && $(realpath -e "$meta") == "$meta" ]]
  read -r baseline <"$meta/baseline"
  [[ $baseline =~ ^[0-9]+$ ]]
  if [[ $action == sample ]]; then
    ((${#leaves[@]})) || return 2
    [[ ! -e $meta/peak && ! -e $meta/stop && ! -e $meta/final ]]
    # The tiny dedicated sampler runs in the run-local peer, not on the
    # driver network; statvfs measures mount occupancy in constant time.
    python3 - "$meta" "$baseline" <<'PY'
import os, sys, time
from pathlib import Path
meta = Path(sys.argv[1]); baseline = int(sys.argv[2])
peak = 0; samples = 0; start = time.monotonic()
while True:
    st = os.statvfs('/bench-combined')
    capacity = st.f_blocks * st.f_frsize // 1024
    used = (st.f_blocks - st.f_bfree) * st.f_frsize // 1024
    if capacity != 50331648 or not baseline <= used <= capacity:
        raise SystemExit('invalid stage mount usage')
    peak = max(peak, used); samples += 1
    value = f'{peak} {samples}\n'
    (meta/'peak.pending').write_text(value)
    (meta/'peak.pending').replace(meta/'peak')
    if (meta/'stop').exists():
        (meta/'final.pending').write_text(value)
        (meta/'final.pending').replace(meta/'final')
        break
    if time.monotonic()-start > 600:
        raise SystemExit('stage sampler exceeded bounded lifetime')
    time.sleep(2)
PY
    return
  fi
  if [[ $action == sample-stop ]]; then
    ((${#leaves[@]})) || return 2
    [[ ! -L $meta/stop ]]
    if [[ ! -e $meta/stop ]]; then (set -o noclobber; : >"$meta/stop"); fi
    return
  fi
  if [[ $action == sample-ready ]]; then
    ((${#leaves[@]})) || return 2
    for _ in {1..50}; do [[ -f $meta/peak && ! -L $meta/peak ]] && return 0; sleep .2; done
    return 1
  fi

  [[ $action == reclaim ]]
  # Stop writers before sampling final occupancy/deleting any stage data.
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
  all_data_siblings | cmp - "$meta/siblings"
  peak=$baseline; samples=0
  if ((${#leaves[@]})); then
    # A sampler that failed or never started is not a successful receipt.
    [[ ! -e $meta/stop && ! -L $meta/stop ]]
    (set -o noclobber; : >"$meta/stop")
    for _ in {1..50}; do [[ -f $meta/final && ! -L $meta/final ]] && break; sleep .2; done
    [[ -f $meta/final && ! -L $meta/final ]]
    read -r peak samples <"$meta/final"
    [[ $peak =~ ^[0-9]+$ && $samples =~ ^[0-9]+$ && $samples -ge 2 &&
       $peak -ge $before && $peak -le 50331648 ]]
    [[ $(realpath -e "$parent") == "$parent" && $(realpath -e "$ram_node") == "$ram_node" ]]
    for leaf in "${leaves[@]}"; do
      [[ $(realpath -e "$ram_node/$leaf") == "$ram_node/$leaf" &&
         $(findmnt -rn -o TARGET,FSTYPE -T "$ram_node/$leaf") == '/bench-combined tmpfs' ]]
      if [[ $leaf == event ]]; then
        [[ -d $disk_node/$leaf && ! -L $disk_node/$leaf &&
           $(realpath -e "$disk_node/$leaf") == "$disk_node/$leaf" &&
           -z $(find "$disk_node/$leaf" -mindepth 1 -print -quit) ]]
      else
        [[ ! -e $disk_node/$leaf && ! -L $disk_node/$leaf ]]
      fi
      kib=$(du -sk -- "$ram_node/$leaf" | awk '{print $1}')
      [[ $kib =~ ^[0-9]+$ && $kib -gt 0 ]]
      final_kib=$((final_kib+kib))
      printf 'MULTI_NODE_ALL_DATA_LEAF_V1 action=reclaim run=%s host=%s node=%s leaf=%s path=%s allocated_kib=%s disk_absent_or_empty=yes\n' \
        "$HOG_XID" "$host" "$node" "$leaf" "$ram_node/$leaf" "$kib"
    done
    for leaf in "${leaves[@]}"; do
      find "$ram_node/$leaf" -xdev -mindepth 1 -delete
      rmdir -- "$ram_node/$leaf"
    done
    rmdir -- "$ram_node" "$parent"
  fi
  [[ ! -e $parent && ! -L $parent ]]
  all_data_siblings | cmp - "$meta/siblings"
  after=$(df -Pk -- /bench-combined | awk 'NR==2{print $3}')
  [[ $after == "$baseline" ]]
  printf 'MULTI_NODE_ALL_DATA_V1 action=reclaim run=%s host=%s node=%s writers_stopped=yes siblings_preserved=yes leaf_count=%s final_leaf_kib=%s baseline_kib=%s peak_kib=%s samples=%s before_kib=%s after_kib=%s\n' \
    "$HOG_XID" "$host" "$node" "${#leaves[@]}" "$final_kib" "$baseline" "$peak" "$samples" "$before" "$after"
  rm -f -- "$meta/baseline" "$meta/siblings" "$meta/peak" "$meta/final" "$meta/stop"
  rmdir -- "$meta"
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
  set -euo pipefail
  [[ $# == 2 ]]
  multi_node_all_data_receipt "$1" "$2"
fi
