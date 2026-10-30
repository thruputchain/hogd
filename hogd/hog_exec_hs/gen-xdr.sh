#!/bin/sh
# Regenerate src/Hogd/Xdr from the protocol XDR sources, and
# src/Hogd/Exec/Runtime/SyscallMeta.hs from the syscall specs.
set -e
cd "$(dirname "$0")"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
python3 ../syscall_spec/gen_syscalls.py --output_dir="$TMP" \
  --syscall_yaml=../../protocol/syscalls.yaml \
  --hog_exec_yaml=../syscall_spec/hog_exec_syscalls.yaml --syscall_x
python3 ../syscall_spec/gen_syscalls.py --output_dir=src/Hogd/Exec/Runtime \
  --syscall_yaml=../../protocol/syscalls.yaml \
  --hog_exec_yaml=../syscall_spec/hog_exec_syscalls.yaml --syscall_meta_hs
P=../../protocol
FILES=$(sed -n '/add_xdr(/,/)/p' ../xdr/CMakeLists.txt | grep '\.x' | sed 's|^\s*||' \
  | grep -v '^syscall.x' \
  | sed "s|^\.\./\.\./protocol/|$P/|; s|^\([a-z_]*\.x\)$|../xdr/\1|")
cabal build -v0 xdrgen
rm -rf src/Hogd/Xdr src/Hogd/Xdr.hs
$(cabal list-bin xdrgen) src $FILES "$TMP/syscall.x" \
  ../hog_exec/components/library_exception.x ../../x23/x23/rpc_msg.x
