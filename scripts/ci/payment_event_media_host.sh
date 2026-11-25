#!/usr/bin/env bash
# Private diagnostic, restricted to one owned Event data leaf on hog8.
set -euo pipefail
[[ $# == 2 && ${GIT_BRANCH:-} == codex/payment-event-media-abba &&
   ${HOGD_CI_CONTAINER:-0} == 1 && ${HOGD_CI_OFFLINE:-0} == 1 &&
   ${HOGD_COMBINED_WORK_DIR:-} == /bench-combined/ci_work_dir/ &&
   ${HOG_XID:-} =~ ^hogd-combined-metrics-[1-9][0-9]*-[1-9][0-9]*$ ]]
action=$1 arm=$2
case "$arm" in 1-disk|4-disk) root=/workspace/.ci; expected=ext4 ;; 2-ram|3-ram) root=/bench-combined; expected=tmpfs ;; *) exit 2 ;; esac
writers_stopped() {
  local writer=$1 status
  for _ in {1..100}; do
    status=0; pgrep -x "$writer" >/dev/null || status=$?
    if [[ $status == 1 ]]; then return 0; fi
    [[ $status == 0 ]]
    sleep .2
  done
  return 1
}
if [[ $action == db-receipt ]]; then
  [[ ${HOG_HOST:-} == hog11 ]]
  writers_stopped bench_server
  control_pid=$(pgrep -x control_server)
  [[ $control_pid =~ ^[0-9]+$ ]]
  # Read only the inherited path selector, never dump the control environment.
  tmpdir=$(python3 - "$control_pid" <<'PYTMP'
import sys
from pathlib import Path
fields=Path(f'/proc/{sys.argv[1]}/environ').read_bytes().split(b'\0')
values=[v.partition(b'=')[2] for v in fields if v.startswith(b'TMPDIR=')]
assert len(values)<=1
print((values[0] if values and values[0] else b'/tmp').decode())
PYTMP
  )
  [[ $tmpdir == /tmp || $tmpdir == /tmp/ ]]
  backing=$(findmnt -rn -o FSTYPE -T "$tmpdir")
  [[ -n $backing ]]
  read -r capacity used free < <(df -Pk "$tmpdir" | awk 'NR==2{print $2,$3,$4}')
  [[ $capacity =~ ^[0-9]+$ && $used =~ ^[0-9]+$ && $free =~ ^[0-9]+$ && $free -ge 10485760 ]]
  db_binary=$(sha256sum /workspace/release/hogd/hog_exec/bench/bench_server | awk '{print $1}')
  [[ $db_binary =~ ^[a-f0-9]{64}$ ]]
  printf 'PAYMENT_EVENT_DB_V1 run=%s arm=%s host=hog11 stopped=yes tmpdir=%s backing=%s capacity_kib=%s used_kib=%s free_kib=%s binary_sha256=%s\n' \
    "$HOG_XID" "$arm" "$tmpdir" "$backing" "$capacity" "$used" "$free" "$db_binary"
  exit
fi
[[ ${HOG_HOST:-} == hog8 ]]
[[ $(realpath -e /workspace/.ci) == /workspace/.ci && $(findmnt -rn -o TARGET,FSTYPE -T /workspace/.ci) == '/workspace ext4' &&
   $(realpath -e /bench-combined) == /bench-combined && $(findmnt -rn -o TARGET,FSTYPE -T /bench-combined) == '/bench-combined tmpfs' &&
   $(stat -c %a /bench-combined) == 1777 && $(df -Pk /bench-combined | awk 'NR==2{print $2}') == 50331648 ]]
leaf="$root/payment-event-abba-$HOG_XID-$arm"
meta="/workspace/.ci/payment-event-abba-meta-$HOG_XID-$arm"
ram_usage() { df -Pk /bench-combined | awk 'NR==2{print $3}'; }
siblings() {
  # Only the shared scratch mount's direct sibling identities are observed.
  # Disk diagnostics and control-server logs may grow outside our private leaf.
  find /bench-combined -mindepth 1 -maxdepth 1 ! -name "${leaf##*/}" -printf '%f\t%i\n' | LC_ALL=C sort
}
if [[ $action == prepare ]]; then
  writers_stopped hog-event
  [[ ! -e $leaf && ! -L $leaf && ! -e $meta && ! -L $meta ]]
  mkdir -m 700 -- "$meta" "$leaf"
  [[ $(realpath -e "$leaf") == "$leaf" && $(findmnt -rn -o FSTYPE -T "$leaf") == "$expected" ]]
  ram_usage >"$meta/baseline"
  siblings >"$meta/siblings"
  # Earlier stock 10-account Event fixture sampled 6.81 GiB peak; allow at least 10 GiB free.
  read -r capacity used free < <(df -Pk "$leaf" | awk 'NR==2{print $2,$3,$4}')
  [[ $capacity =~ ^[0-9]+$ && $used =~ ^[0-9]+$ && $free =~ ^[0-9]+$ && $free -ge 10485760 ]]
  event_binary=$(sha256sum /workspace/release/hogd/hog_event/hog-event | awk '{print $1}')
  [[ $event_binary =~ ^[a-f0-9]{64}$ ]]
  printf 'PAYMENT_EVENT_MEDIA_V1 action=prepare run=%s arm=%s host=hog8 path=%s backing=%s capacity_kib=%s baseline_mount_kib=%s baseline_ram_kib=%s binary_sha256=%s\n' \
    "$HOG_XID" "$arm" "$leaf" "$expected" "$capacity" "$used" "$(cat "$meta/baseline")" "$event_binary"
  exit
fi
[[ -d $meta && ! -L $meta && $(realpath -e "$meta") == "$meta" && -d $leaf && ! -L $leaf && $(realpath -e "$leaf") == "$leaf" && $(findmnt -rn -o FSTYPE -T "$leaf") == "$expected" ]]
read -r baseline <"$meta/baseline"; [[ $baseline =~ ^[0-9]+$ ]]
case "$action" in
sample)
  [[ ! -e $meta/peak && ! -e $meta/stop && ! -e $meta/final ]]
  python3 - "$meta" "$leaf" <<'PY'
import os, sys, time
from pathlib import Path
meta,leaf=map(Path,sys.argv[1:]); start=time.monotonic(); peak=0; samples=0
while True:
    st=os.statvfs(leaf); used=(st.f_blocks-st.f_bfree)*st.f_frsize//1024
    peak=max(peak,used); samples+=1
    val=f'{peak} {samples}\n'; (meta/'peak.pending').write_text(val); (meta/'peak.pending').replace(meta/'peak')
    if (meta/'stop').exists():
        (meta/'final.pending').write_text(val); (meta/'final.pending').replace(meta/'final'); break
    if time.monotonic()-start>900: raise SystemExit('sampler exceeded bounded lifetime')
    time.sleep(2)
PY
  ;;
sample-ready)
  for _ in {1..50}; do [[ -f $meta/peak && ! -L $meta/peak ]] && exit 0; sleep .2; done; exit 1
  ;;
reclaim)
  writers_stopped hog-event
  if [[ -f $meta/peak && ! -L $meta/peak ]]; then
    [[ ! -L $meta/stop ]]; if [[ ! -e $meta/stop ]]; then (set -o noclobber; : >"$meta/stop"); fi
    for _ in {1..50}; do [[ -f $meta/final && ! -L $meta/final ]] && break; sleep .2; done
    [[ -f $meta/final && ! -L $meta/final ]]; read -r peak samples <"$meta/final"
  else
    # Only reachable if preparation succeeded but sampling never started.
    peak=0; samples=0
  fi
  [[ $peak =~ ^[0-9]+$ && $samples =~ ^[0-9]+$ ]]
  siblings | cmp - "$meta/siblings"
  before=$(ram_usage); final=$(du -sk -- "$leaf" | awk '{print $1}')
  [[ $before =~ ^[0-9]+$ && $final =~ ^[0-9]+$ ]]
  find "$leaf" -xdev -mindepth 1 -delete
  rmdir -- "$leaf"
  [[ ! -e $leaf && ! -L $leaf ]]
  siblings | cmp - "$meta/siblings"
  after=$(ram_usage); [[ $after == "$baseline" ]]
  event_binary=$(sha256sum /workspace/release/hogd/hog_event/hog-event | awk '{print $1}')
  [[ $event_binary =~ ^[a-f0-9]{64}$ ]]
  printf 'PAYMENT_EVENT_MEDIA_V1 action=reclaim run=%s arm=%s host=hog8 path=%s backing=%s writers_stopped=yes siblings_preserved=yes peak_mount_kib=%s samples=%s final_leaf_kib=%s ram_before_kib=%s ram_after_kib=%s baseline_ram_kib=%s binary_sha256=%s\n' \
    "$HOG_XID" "$arm" "$leaf" "$expected" "$peak" "$samples" "$final" "$before" "$after" "$baseline" "$event_binary"
  rm -f -- "$meta/baseline" "$meta/siblings" "$meta/peak" "$meta/final" "$meta/stop"
  rmdir -- "$meta"
  ;;
*) exit 2 ;;
esac
