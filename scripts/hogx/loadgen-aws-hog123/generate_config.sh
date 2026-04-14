#!/usr/bin/env bash
set -euo pipefail

base_ip=96

# Create directories
mkdir -p .hogx/cluster

for i in $(seq 0 15); do
  dir=".hogx/cluster/$i"
  mkdir -p "$dir"

  ip=$((base_ip + i))
  full_ip="10.64.0.$ip"

  cat > "$dir/cluster.json" <<EOF
{
  "host_defs": [
    {
      "host_id": "aws-hog$i",
      "host_tags": [],
      "ssh_user": "",
      "ssh_address": "$full_ip",
      "ssh_port": null,
      "ssh_private_key": "",
      "host_config": {
        "public_ip": "$full_ip",
        "private_ip": "$full_ip",
        "agent_port": 11055,
        "pkg_dir": "/tmp/hogx/pkg",
        "app_dir": "/tmp/hogx/app",
        "var_dir": "/tmp/hogx/var",
        "tmp_dir": "/tmp/hogx/tmp"
      }
    }
  ],
  "assembly_defs": []
}
EOF
done
