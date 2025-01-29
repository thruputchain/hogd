#pragma once

#include "hogd/coord/allocate_service.h"
#include "hogd/coord/coord_block_manager.h"
#include "hogd/coord/database_manager.h"
#include "hogd/coord/executor_manager.h"

namespace hogd {

class CoordinatorCore {
public:
  CoordinatorCore(int nthreads);

  void start(std::string const &coord_server_port);

private:
  x23::ThreadPool pool_;
  x23::CoroPoll cp_;
  // not initialized until start() finishes
  std::unique_ptr<DatabaseManager> db_manager_;
  ExecutorManager executor_manager_;
  CoordinatorAllocateService allocate_service_;
  // not initialized until db_manager_ is initialized
  std::unique_ptr<CoordinatorBlockManager> block_manager_;
};

} // namespace hogd
