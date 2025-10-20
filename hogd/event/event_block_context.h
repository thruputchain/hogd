#pragma once

#include "event.hh"
#include "hogd/container/merkle/merkle_trie.h"

namespace hogd {

class EventBlockContext {
public:
  void add_event(TxEventLog const &event);

  SHA256Hash get_root_hash();

private:
  MerkleTriePrefix trie_;
};

} // namespace hogd
