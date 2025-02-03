#include <gtest/gtest.h>

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "hogd/coord/coord_core.h"
#include "hogd/hogdb/hogdb_core.h"
#include "hogd/hogexec/hogexec_core.h"
#include "rpc_types.hh"

namespace hogd {

// Just check whether coordinator can startup/shutdown properly with db and
// executors This test has the coordinator start up last.
TEST(TestConnect, Basic)
{
  const std::string k_db_to_executor_port =
      std::to_string(kDBToExecutorPortNum);
  const std::string k_db_to_coord_port = std::to_string(kDBToCoordPortNum);
  const std::string k_exec_to_coord_port = std::to_string(kExecToCoordPortNum);
  const std::string k_coord_announce_port =
      std::to_string(kCoordAnnouncePortNum);

  std::mutex mtx;
  std::condition_variable cv;
  bool test_done = false;
  bool executor_connected = false;

  std::thread db_thread([&] {
    HogdbCore db_core(1, k_db_to_executor_port, k_db_to_coord_port);

    db_core.announce_to_coordinator("localhost", k_coord_announce_port);

    std::unique_lock lk(mtx);
    if (test_done) {
      return;
    }
    cv.wait(lk, [&] { return test_done; });
  });

  std::thread exec_thread([&] {
    HogexecCore exec_core = start_hogexec(1, k_exec_to_coord_port, "localhost",
                                          k_coord_announce_port);
    exec_core.announce_ready_to_coord("localhost", k_coord_announce_port);
    std::unique_lock lk(mtx);
    executor_connected = true;
    cv.notify_all();
    if (!test_done) {
      cv.wait(lk, [&] { return test_done; });
    }
    exec_core.await_shutdown();
  });
  {
    CoordinatorCore coord(1);
    coord.start(k_coord_announce_port);
    {
      std::unique_lock lk(mtx);
      if (!executor_connected) {
        cv.wait(lk, [&] { return executor_connected; });
      }
      test_done = true;
    }
  }
  cv.notify_all();
  exec_thread.join();
  db_thread.join();
} // namespace hogd

} // namespace hogd
