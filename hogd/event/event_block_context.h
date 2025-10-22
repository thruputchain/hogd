#pragma once

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <sys/fcntl.h>
#include <vector>

#include "event.hh"
#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/base/perthread.h"
#include "hogd/base/string.h"
#include "hogd/base/types.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "rpc_types.hh"
#include "types.hh"
#include "x23/cleanup.h"

namespace hogd {

class EventBlockContext {
public:
  static std::string temp_events_filename(std::string_view work_directory,
                                          BlockID block_id)
  {
    return std::string(work_directory) + "/" + std::to_string(block_id) +
           ".events_temp";
  }
  static std::string final_events_filename(std::string_view work_directory,
                                           SHA256Hash hash)
  {
    return std::string(work_directory) + "/" +
           encode_hex(std::as_bytes(std::span(hash))) + ".events";
  }

  EventBlockContext(std::string_view work_directory, BlockID block_id)
    : trie_(std::make_unique<MerkleTriePrefix>()),
      work_directory_(work_directory), block_id_(block_id)
  {
    // Ensure the work_directory_ exists before creating the events file.
    auto dir = Directory::open(work_directory_);
    if (!dir) {
      panic("failed to open work directory: {}", dir.error());
    }
    auto filename = temp_events_filename(work_directory_, block_id_);
    auto fd = open(filename.c_str(), O_CREAT | O_WRONLY, 0666);
    if (fd == -1) {
      panic("failed to open events file: {}", strerror(errno));
    }
    events_file_ = x23::UniqueFd(fd);
  }

  ~EventBlockContext();

  void add_event(TxEventLog const &event);
  SHA256Hash get_root_hash();
  void finish_file_write();
  void finish_file(SHA256Hash const &hash);

private:
  void write_event(std::span<const uint8_t> data);
  void sync_file(std::unique_lock<std::mutex> &ul);
  // The rename is slow if there's already a file of the same hash.
  // (the time to delete the previous file).
  // In actual practice, it should be impossible for this to occur,
  // as the output files contain transaction hashes (which are uniquified)
  // unless multiple replicas make the exact same nomination.  We could deal with this edge case 
  // if this becomes a problem, but I suspect this will not matter significantly.
  void rename_file(std::unique_lock<std::mutex> &ul, SHA256Hash const &hash);

  std::unique_ptr<MerkleTriePrefix> trie_;
  std::string work_directory_;
  BlockID block_id_;

  struct EventContext {
    std::vector<uint8_t> buf;
    uint32_t event_count;
  };

  PerThread<EventContext> buffers_;

  std::atomic<size_t> pending_writes_{0};
  std::mutex file_mutex_;
  std::condition_variable file_cv_;
  x23::UniqueFd events_file_;
};

} // namespace hogd
