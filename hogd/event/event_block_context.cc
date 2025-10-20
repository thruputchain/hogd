#include "hogd/event/event_block_context.h"

#include "event.hh"
#include "hogd/base/log.h"
#include "hogd/event/merkle.h"

namespace hogd {

void EventBlockContext::add_event(TxEventLog const& event) {

    CHECK(EventTrieHelpers::add_event_in_trie(trie_, event.proposed_tx_hash, event.events));

}

SHA256Hash EventBlockContext::get_root_hash() {
    return trie_.get_root_hash();
}

}