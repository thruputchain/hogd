#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "hogd/base/types.h"
#include "retrocausal_queue.hh"
#include "storage_item.hh"
#include "x23/vec.h"

namespace hogd {

// A retrocausal queue as a snapshot server serves it: the metadata item read
// at the queue's key, plus its blocks by block number.
struct TestQueue {
  WireStorageItem item;
  std::map<uint64_t, RetrocausalQueueBlock> blocks;
};

QueueEntry make_test_queue_entry(uint32_t value);

// Builds a queue whose blocks are numbered from 1, one per element of
// [blocks], with nothing consumed.
TestQueue make_queue(std::vector<x23::Vec<QueueEntry>> const &blocks);

} // namespace hogd
