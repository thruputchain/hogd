#include "hogd/coord/coord_core.h"

namespace hogd {

CoordinatorCore::CoordinatorCore(int nthreads)
  : pool_(nthreads), cp_(pool_), allocate_service_(cp_, executor_manager_)
{}

void
CoordinatorCore::start(std::string const &coord_server_port)
{
  allocate_service_.start(coord_server_port);
  db_manager_ = allocate_service_.make_database_manager();
  block_manager_ = std::make_unique<CoordinatorBlockManager>(*db_manager_);
}

} // namespace hogd
