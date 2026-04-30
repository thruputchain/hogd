### launch coordinator-loadgen-hog8.sh
#!/bin/sh
set -e

if [ $# -ne 2 ]; then
  echo "Usage: $0 <chuckd_path> <scpd_path>"
  exit 1
fi
cat >config-hog8.json <<EOF

{
    "validator_config": {
        "private_info": {
            "public_info": {
                "public_key": "970c9ba081452fa7f094bbba51966f4a64c9108ea6fec6d1dcbbad38e25e0f56",
                "address": "hog8:50000"
            },
            "secret_key": "b9cb6de12b73f1969adeeb7c1102d9420f692da1c977cdeb15271c6a090d06b8970c9ba081452fa7f094bbba51966f4a64c9108ea6fec6d1dcbbad38e25e0f56"
        },
        "known_validators": [
            {
                "public_key": "970c9ba081452fa7f094bbba51966f4a64c9108ea6fec6d1dcbbad38e25e0f56",
                "address": "hog8:50000"
            }
        ]
    },
    "wallet_config": {
        "secret_key": "010203",
        "users": [
        ]
    },
    "discovery_config": {
        "advertise_interval_msec": "15000",
        "advertise_par": 5,
        "snapshot_ttl_msec": "21000"
    },
    "router_config": {
        "tree_fan_out": 5
    },
    "dogwood_config": {
        "route_count": 50,
        "route_time_to_live_msec": "60000",
        "route_max_latency_msec": "1000",
        "endpoint_max_pending": 800,
        "endpoint_num_clients": 16,
        "endpoint_idle_ttl_msec": "60000",
        "endpoint_dial_timeout_msec": "2000",
        "endpoint_redial_delay_msec": "100",
        "endpoint_call_timeout_msec": "2000",
        "stream_receiver_worker_count": 512,
        "stream_sender_worker_count": 32,
        "transmission_receiver_decode_worker_count": 512,
        "transmission_sender_worker_count": 32,
        "transmission_partial_max_ttl_msec": "10000",
        "transmission_partial_idle_ttl_msec": "1000"
    },
    "mempool_config": {
        "transaction_ttl_msec": "6000",
        "submission_lot_size": "1000",
        "submission_lot_wait_msec": "50"
    },
    "catchup_config": {
        "catchup_block_ttl_sec": "15",
        "catchup_block_age": "5"
    },
    "scp_config": {
        "max_timeout_ms": "1000",
        "initial_ballot_timeout_ms": "750",
        "increment_ballot_timeout_ms": "750",
        "initial_nomination_timeout_ms": "750",
        "increment_nomination_timeout_ms": "750",
        "quorum_set": {
            "threshold": 1,
            "validators": [
                {
                    "scheme": "Ed25519",
                    "ed25519": "970c9ba081452fa7f094bbba51966f4a64c9108ea6fec6d1dcbbad38e25e0f56"
                }
            ],
            "inner_sets": [
            ]
        }
    },
    "baton_config": {
        "skew_msec": "10",
        "heartbeat_msec": "600",
        "patience_msec": "3000",
        "max_catchup_height": 10000
    },
    "repair_config": {
        "base_delay_msec": "200",
        "max_delay_msec": "5000",
        "multiplier_nom": 3,
        "multiplier_denom": 2
    },
    "storage_config": {
        "reclaim_interval_msec": "300000",
        "max_catchup_blocks": 86400,
        "max_scp_slots": 86400
    }
}
EOF
rm -rf ./storage-hog8
$1 -l info --stack --config config-hog8.json \
  --instance 970c9b \
  --seed 1410441218572558875 \
  --metrics :2223 \
  bench coordinator_loadgen \
  --debug-port 5500 \
  --num-accts 100 \
  --txs-per-block 10000 \
  --num-blocks 100 \
  --storage-dir ./storage-hog8 \
  --out-addr :50000 \
  --loadgen-addr hog7:44400 \
  --sched-addr hog9:8070 \
  --scpd-path $2 \
  --scp-handle-addr 127.0.0.1:3300 \
  --scp-control-addr 127.0.0.1:4400 \
  --scp-is-validator true \
  > >(tee validator-hog8.report) \
  2> >(tee validator-hog8.log >&2)
