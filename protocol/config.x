namespace hogd {

typedef hyper Milliseconds;
typedef hyper Seconds;

// ValidatorConfig holds the network configuration of a validator.
struct ValidatorConfig {
  ValidatorPrivateInfo private_info;
  QuorumSet quorum_set;
  ValidatorPublicInfo known_validators<>;
};

struct WalletConfig {
  JWTSecretKey secret_key;
  WalletUser users<>;
};

struct DiscoveryConfig {
  Milliseconds advertise_interval_msec;
  unsigned int advertise_par;
  Milliseconds snapshot_ttl_msec;
};

struct RouterConfig {
  unsigned int tree_fan_out;
};

struct DogwoodConfig {
  // navigator
  DogwoodInt32 route_count;
  Milliseconds route_time_to_live_msec;
  Milliseconds route_max_latency_msec;
  // endpoint caller
  DogwoodInt32 endpoint_max_pending;
  DogwoodInt32 endpoint_num_clients;
  Milliseconds endpoint_idle_ttl_msec;
  Milliseconds endpoint_dial_timeout_msec;
  Milliseconds endpoint_redial_delay_msec;
  Milliseconds endpoint_call_timeout_msec;
  // stream processing
  DogwoodInt32 stream_receiver_worker_count;
  DogwoodInt32 stream_sender_worker_count;
  // transmission processing
  DogwoodInt32 transmission_receiver_decode_worker_count;
  DogwoodInt32 transmission_sender_worker_count;
  //
  Milliseconds transmission_partial_max_ttl_msec;
  Milliseconds transmission_partial_idle_ttl_msec;
};

struct MempoolConfig {
  Milliseconds transaction_ttl_msec;
  hyper submission_lot_size;
  Milliseconds submission_lot_wait_msec;
};

struct CatchupConfig {
  Seconds catchup_block_ttl_sec;
  BlockHeight catchup_block_age;
};

struct BatonConfig {
  Milliseconds heartbeat_msec;
  Milliseconds nomination_ttl_msec;
};

struct Config {
  ValidatorConfig *validator_config;
  WalletConfig *wallet_config;
  DiscoveryConfig *discovery_config;
  RouterConfig *router_config;
  DogwoodConfig *dogwood_config;
  MempoolConfig *mempool_config;
  CatchupConfig *catchup_config;
  SoloConfig *solo_config;
  BatonConfig *baton_config;
};

} // namespace hogd
