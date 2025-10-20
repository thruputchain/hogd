#pragma once

#include <cstddef>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <fcntl.h>
#include <sys/fcntl.h>

#include "event.hh"
#include "hogd/base/log.h"
#include "hogd/base/perthread.h"
#include "hogd/base/string.h"
#include "hogd/base/types.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "rpc_types.hh"
#include "types.hh"
#include "x23/cleanup.h"
#include "x23/vec.h"
#include "hogd/base/io.h"


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
    : work_directory_(work_directory), block_id_(block_id)
  {
    info("opening work directory: {}", work_directory_);
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

  void add_event(TxEventLog const &event);
  SHA256Hash get_root_hash();
  void finish_file();

private:
  void write_event(std::span<const std::byte> data);
  void sync_file(std::unique_lock<std::mutex> &ul);
  void rename_file(std::unique_lock<std::mutex> &ul, SHA256Hash const &hash);

  MerkleTriePrefix trie_;
  std::string work_directory_;
  BlockID block_id_;

  struct EventContext {
    x23::Buf buf;
    uint32_t event_count;
  };

  PerThread<EventContext> buffers_;

  std::mutex file_mutex_;
  x23::UniqueFd events_file_;
};

} // namespace hogd
