#include <print>
#include <string>

#include "hogd/base/log.h"
#include "hogd/init.h"

namespace hogd {

namespace {

int
indexer_init(const std::string &path, const std::string &event_server_addr)
{
  return 0;
}

} // namespace

} // namespace hogd

int
main(int argc, char *argv[])
{
  int ret = hogd::hogd_init(argc, argv);
  if (ret) {
    return ret;
  }
  hogd::info("starting indexer!");

  if (argc != 3) {
    std::println("usage: db_path event_server_ip:port]");
    return EXIT_FAILURE;
  }

  const std::string path = argv[1];
  const std::string event_server_addr = argv[2];
  return hogd::indexer_init(path, event_server_addr);
};
