// clang-format off
%#include "exec_coord.hh"
%#include "rpc_types.hh"
%#include "transaction.hh"
namespace hogd {
  // clang-format on

  // API exposed from the Coordinator for the Scheduler
  program CoordScheduler
  {
    version CoordScheduler1
    {

      void coordscheduler_null(void) = 0;

      // The scheduler calls coordscheduler_notify_vetted to indicate that the
      // given transactions are vetted, meaning the coordinator can broadcast
      // them to the network.
      void coordscheduler_notify_vetted(SignedTransactionBatch) = 10;
    }
    = 1;
  }
  = 0x11117667;

} // namespace hogd
