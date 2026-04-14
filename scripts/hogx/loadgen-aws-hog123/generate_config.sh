#!/usr/bin/env bash
set -euo pipefail

base_ip=96

mkdir -p .hogx/cluster

for cluster in 0 1; do
  dir=".hogx/cluster/$cluster"
  mkdir -p "$dir"

  start=$((cluster * 8))
  end=$((start + 7))

  {
    echo '{'
    echo '  "host_defs": ['

    first=true
    for i in $(seq "$start" "$end"); do
      ip=$((base_ip + i))
      full_ip="10.64.0.$ip"

      if [ "$first" = true ]; then
        first=false
      else
        echo '    ,'
      fi

      cat <<EOF
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
EOF
    done

    echo '  ],'
    echo '  "assembly_defs": []'
    echo '}'
  } > "$dir/cluster.json"
done
