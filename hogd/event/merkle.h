#pragma once

#include <span>

#include "event.hh"
#include "hogd/base/perthread.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "types.hh"
#include "x23/serialize.h"
#include "x23/vec.h"


namespace hogd {

class EventTrieHelpers {
public:
    // This class just contains static methods, so we do not allow you to
    // construct an instance of the class.
    EventTrieHelpers() = delete;

    static bool add_event_in_trie(MerkleTriePrefix &trie, SHA256Hash const &proposed_tx_hash, x23::Vec<TxEmittedEvent, 0xffffffff> const &events) {
        static PerThread<x23::Buf> serialize_buf;

        serialize_buf->clear();
        x23::serialize_to(&*serialize_buf, events);
        auto value = std::as_bytes(std::span(*serialize_buf));
        return trie.insert_or_update(std::as_bytes(std::span(proposed_tx_hash)), value);
    }
};

}