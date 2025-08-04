// hogdb.cc - handles initialization for hogdb

#include <cstddef>
#include <cstdlib>
#include <future>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>

// TODO(abelay): there is apparently a header dependency problem inside x23
// where x23/vec.h cannot be included first.  These generated headers are not
// needed, but included as a hack to avoid it.
#include "db_coord.hh" // NOLINT
#include "hogd/container/merkle/merkle_trie.h"
#include "rpc_types.hh" // NOLINT

#include "hogd/base/log.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/hogdb/db.h"
#include "hogd/hogdb/rpc_context.h"
#include "hogd/hogdb/rpc_from_coord.h"
#include "hogd/hogdb/rpc_from_exec.h"
#include "hogd/init.h"
#include "hogd/ramdb/db.h"
#include "hogd/ramdb/db_status.h"

namespace hogd {

namespace {

// port numbers used by servers hosted inside hogdb.
constexpr uint16_t kCoordServerPort = 8080;
constexpr uint16_t kExecServerPort = 8081;

int
hogdb_init(std::string_view path, size_t ram_bytes)
{
  // check the database's integrity and find the highest valid LSN.
  DBStatus<uint64_t> lsn = Database::check(path, ram_bytes);
  if (!lsn) {
    critical("Database check failed: path '{}', {}", path, lsn.error());
    return EXIT_FAILURE;
  }
  info("database '{}' passed integrity check; current LSN is {}", path, *lsn);

  // create the database, loading up to the highest valid LSN.
  // TODO(abelay): When sharding is supported, this will become a separate step.
  DBStatus<std::unique_ptr<Database>> ramdb =
      Database::open_lsn(path, *lsn, ram_bytes);
  if (!ramdb) {
    critical("Could not create database: path '{}', {}", path, ramdb.error());
    return EXIT_FAILURE;
  }

  // create the RPC context.
  HogDatabase db(std::move(*ramdb));
  MerkleTriePrefix trie;
  RPCContext ctx(db, trie);

  // insert each item in the database into the merkle trie.
  ctx.get_db().get_each_parallel([&trie](std::span<const std::byte> key,
                                         uint8_t type,
                                         std::span<const std::byte> value) {
    std::span<const std::byte> k =
        ItemChange::add_type_to_merkle_key(key, type);
    CHECK(trie.insert(k, value));
  });
  trie.generate_top_level_hashes();

  // start the coordinator server.
  info("starting coordinator server.");
  HogDBCoordServer coord_server(ctx);
  coord_server.start_server(kCoordServerPort);

  // start the executor server.
  info("starting executor server.");
  HogDBExecutorServer exec_server(ctx);
  ErrnoStatus<void> ret = exec_server.start_server(kExecServerPort);
  if (!ret) {
    critical("Failed to start executor server: {}", ret.error());
    return EXIT_FAILURE;
  }

  // block forever
  info("hogdb initialization complete.");
  std::promise<void>().get_future().wait();

  return EXIT_SUCCESS;
}

} // namespace

} // namespace hogd

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  int ret = hogd::hogd_init(argc, argv);
  if (ret)
    return ret;
  hogd::info("starting hogdb!");

  if (argc < 2) {
    std::println("usage: [db_path] [ram_bytes]");
    return EXIT_FAILURE;
  }

  std::string path = argv[1];
  size_t ram_bytes = std::stoul(argv[2], nullptr, 0);
  return hogd::hogdb_init(path, ram_bytes);
};
