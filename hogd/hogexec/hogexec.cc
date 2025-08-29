#include <cstdlib>
#include <print>
#include <string>

#include "coord.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/interface/exec_coord_server.h"
#include "hogd/init.h"

namespace hogd {

namespace {

constexpr uint16_t kExecServerPort = 8070;

int
hogexec_init(const ExecCard &card, int poll_threads)
{
  ExecCoordServer exec(card, poll_threads);
  // TODO(jhumphri): Replace nullptr.
  exec.init_from_coord(nullptr);
  CHECK_EQ(exec.start_server(kExecServerPort), kExecServerPort);

  return 0;
}

} // namespace

} // namespace hogd

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  int ret = hogd::hogd_init(argc, argv);
  if (ret)
    return ret;
  hogd::info("starting hogexec!");

  if (argc < 3) {
    std::println("usage: [coordinator_ip:port] [num_poll_threads]");
    return EXIT_FAILURE;
  }

  const hogd::ExecCard card{.protocol = "blah", .upid = {}, .address = argv[1]};
  const int poll_threads = std::stoi(argv[2]);
  return hogd::hogexec_init(card, poll_threads);
};
