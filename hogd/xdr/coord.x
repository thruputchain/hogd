namespace hogd {

const kCoordPort = 10078;

program CoordProg
{
  version CoordV1
  {
    void null(void) = 0;
  }
  = 1;
}
= 0x1d88d309;

enum CoordinatorAllocateStatus { kReady = 0, kRetry = 1, kCatastropic = 2 };

struct DatabaseInfo {
  // TODO(geoff): more complex initialization details for sharded db
  string database_address<>;
  string database_port<>;
};

struct ExecutorInitialization {
  unsigned int executor_id; // uint16_t
  DatabaseInfo database_info;
};

union ExpectedExecutorInitialization switch (CoordinatorAllocateStatus status)
{
case kReady:
  ExecutorInitialization executor_init;
default:
  void;
};

struct ExecutorReadyAnnouncement {
  string executor_address<>;
  string executor_port<>;
  unsigned int nthreads;
};

program CoordinatorAllocate
{
  version CoordinatorAllocateV1
  {
    ExpectedExecutorInitialization request_new_executor(void) = 0;
    CoordinatorAllocateStatus executor_announce_ready(
        ExecutorReadyAnnouncement) = 1;
    CoordinatorAllocateStatus database_announce_ready(DatabaseInfo) = 2;
  }
  = 1;
}
= 0x1d88d30a;

} // namespace hogd
