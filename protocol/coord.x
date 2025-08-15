// clang-format off
%#include "nomination.hh"

namespace hogd {
  // clang-format on

  // ServerAddress is an IP (or hostname) and port address, separated by a
  // colon.
  typedef string ServerAddress<>;

  // UPID is a globally unique process identifier (function of start time, place
  // and randomness)
  typedef opaque UPID[8];

  // Protocol describes a specific protocol and version.
  typedef string Protocol<>;

  // CoordinatorCard uniquely describes a coordinator instance.
  struct CoordinatorCard {

    Protocol protocol;

    UPID upid;
  };

  // ExecCard uniquely describes an executor instance.
  struct ExecCard {

    Protocol protocol;

    UPID upid;

    ServerAddress address;
  };

  // ShardCard uniquely describes a database shard instance and its initial
  // state.
  struct ShardCard {

    Protocol protocol;

    UPID upid;

    BlockHeight block_height;

    unsigned int shard_count;

    ServerAddress address;

    // TODO: KeyPrefixes describes the key prefixes handled by this shard
    // KeyPrefixes key_prefixes;
  };

  struct IntroShardResult {
    // cc holds the identity of the coordinator that responded
    CoordinatorCard cc;
  };

  struct IntroExecResult {
    // cc holds the identity of the coordinator that responded
    CoordinatorCard cc;

    // exec_id is a unique 16-bit identifier assigned to the executor by the
    // coordinator.
    unsigned int exec_id;

    // shards describes a ready db cluster
    ShardCard shards<>;
  };

  program Coordinator
  {
    version Coordinator1
    {

      void coordinator_null(void) = 0;

      // coordinator_intro_shard is called by db shards to introduce themselves
      // to the coordinator.
      IntroShardResult coordinator_intro_shard(ShardCard) = 1;

      // coordinator_intro_exec is called by executors to introduce themselves
      // to the coordinator. It blocks until the db is ready to use.
      IntroExecResult coordinator_intro_exec(ExecCard) = 2;
    }
    = 1;
  }
  = 0x3eef0101;

} // namespace hogd
