// clang-format off
%#include "types.hh"

namespace hogd {
// clang-format on

typedef opaque ProgramArg<>;
struct ServerConfig {
  opaque program_name<>;
  ProgramArg program_args<>;
  opaque cpu_affinity<>;
};

typedef opaque HogdGitHash<>;

enum CIServerErrorCode {
  kCIServerErrorCodeOK = 0,
  kCIServerErrorCodeAlreadyRunning = 1,
  kCIServerErrorCodeInternal = 2,
  kCIServerErrorCodeVersionMismatch = 3
};

union CIProcStatus switch (CIServerErrorCode error_code) {
case kCIServerErrorCodeOK:
  int exit_code;
default:
  // kCIServerErrorCodeAlreadyRunning indicates the process is still running
  // all other errors indicate CI failures
  void;
};

union CIWorkDirStatus switch (CIServerErrorCode error_code) {
case kCIServerErrorCodeOK:
  opaque work_dir<>;
default:
  void;
};

struct CIStartPaths {
  opaque stdout_path<>;
  opaque stderr_path<>;
};

union CIStartStatus switch (CIServerErrorCode error_code) {
case kCIServerErrorCodeOK:
  CIStartPaths paths;
default:
  void;
};

// Default port the CI control server listens on.
const kCIControlServerPort = 11111;

program CIServer
{
  version CIServer1
  {
    CIStartStatus ciserver_start(HogdGitHash, ServerConfig) = 0;
    CIServerErrorCode ciserver_stop(HogdGitHash) = 1;
    CIServerErrorCode ciserver_shutdown(HogdGitHash) = 2;
    // returns the status of the most recent process
    CIProcStatus ciserver_query_proc_status(HogdGitHash) = 3;
    CIWorkDirStatus ciserver_query_work_dir(HogdGitHash) = 4;
    CIServerErrorCode ciserver_clear_work_dir(HogdGitHash) = 5;
  }
  = 1;
}
= 0x11117668;

} // namespace hogd
