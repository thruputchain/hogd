#!/usr/bin/env bash
# Private artifact-only standalone payments source comparison.
set -euo pipefail
[[ $# == 2 && ${GIT_BRANCH:-} == codex/payment-sparse-phase-screen &&
   ${HOGD_CI_CONTAINER:-0} == 1 && ${HOGD_CI_OFFLINE:-0} == 1 &&
   ${HOG_XID:-} =~ ^hogd-metrics-[1-9][0-9]*-[1-9][0-9]*$ && ${GIT_COMMIT:-} =~ ^[a-f0-9]{40}$ ]]
role=$1 point=$2
case "$point" in before10|after10|before2|after2) ;; *) exit 2 ;; esac
case "$role:${HOG_HOST:-}" in
  event:hog8) writer=hog-event; binary=/workspace/release/hogd/hog_event/hog-event ;;
  db:hog11) writer=bench_server; binary=/workspace/release/hogd/hog_exec/bench/bench_server ;;
  *) exit 2 ;;
esac
[[ $(git -C /workspace rev-parse HEAD) == "$GIT_COMMIT" ]]
control=$(pgrep -x control_server); [[ $control =~ ^[0-9]+$ ]]
for _ in {1..100}; do
  status=0; pgrep -x "$writer" >/dev/null || status=$?
  [[ $status == 0 || $status == 1 ]]
  [[ $status != 1 ]] || break
  sleep .2
done
[[ $status == 1 ]]
# Only select known fields of the known run-local control process.
read -r cwd workdir tmpdir cpus < <(python3 - "$control" <<'PY'
import sys,os
from pathlib import Path
p=Path('/proc')/sys.argv[1]; args=p.joinpath('cmdline').read_bytes().split(b'\0')
assert args.count(b'-w')==1
work=args[args.index(b'-w')+1].decode()
assert work in ('ci_work_dir/','/workspace/ci_work_dir/')
cwd=os.readlink(p/'cwd'); assert cwd=='/workspace'
values=[v[7:] for v in p.joinpath('environ').read_bytes().split(b'\0') if v.startswith(b'TMPDIR=')]
assert len(values)<=1
tmp=(values[0] if values and values[0] else b'/tmp').decode(); assert tmp in ('/tmp','/tmp/')
cpus=next(l.partition(':')[2].strip() for l in p.joinpath('status').read_text().splitlines() if l.startswith('Cpus_allowed_list:'))
print(cwd,work,tmp,cpus)
PY
)
[[ $cwd == /workspace && $cpus =~ ^[0-9,-]+$ ]]
workspace_media=$(findmnt -rn -o FSTYPE -T /workspace/ci_work_dir)
tmp_media=$(findmnt -rn -o FSTYPE -T "$tmpdir")
[[ $workspace_media == ext4 && $tmp_media == ext4 ]]
sha=$(sha256sum "$binary" | awk '{print $1}')
control_sha=$(sha256sum /workspace/release/hogd/hog_exec/bench/control_server | awk '{print $1}')
[[ $sha =~ ^[a-f0-9]{64}$ && $control_sha =~ ^[a-f0-9]{64}$ ]]
printf 'PAYMENT_SOURCE_ROLE_V1 run=%s commit=%s host=%s role=%s point=%s owner_stopped=yes cpus=%s workdir=%s work_media=%s tmpdir=%s tmp_media=%s binary_sha256=%s control_sha256=%s\n' \
 "$HOG_XID" "$GIT_COMMIT" "$HOG_HOST" "$role" "$point" "$cpus" "$workdir" "$workspace_media" "$tmpdir" "$tmp_media" "$sha" "$control_sha"
