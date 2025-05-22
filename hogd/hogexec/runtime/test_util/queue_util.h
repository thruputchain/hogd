#pragma once

#include <vector>

#include "hogd/base/types.h"
#include "hogd/storage_item/retrocausal_queue.h"
#include "retrocausal_queue.hh"
#include "x23/vec.h"

namespace hogd {

QueueEntry make_test_queue_entry(uint8_t value);
RetrocausalQueue make_queue(std::vector<x23::Vec<QueueEntry>> const &blocks);

} // namespace hogd
