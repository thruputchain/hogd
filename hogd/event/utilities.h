#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "event.hh"
#include "hogd/base/types.h"
#include "types.hh"
#include "x23/x23.h"

namespace hogd {

// Utilities for handling events

// event must be at most length kMaxLogMessageLen
TxEmittedEvent make_tx_emitted_event(ContractAddress const &contract_address,
                                     uint32_t num_topics,
                                     std::span<const std::byte> event);

template<x23::is_xdr_struct T>
constexpr std::string xdr_event_string() {
    std::vector<std::string> args;
    [&]<size_t... I>(std::index_sequence<I...>) {
        (args.push_back(x23::xdr_type_name<std::tuple_element_t<I, T>>()), ...);
    }(std::make_index_sequence<std::tuple_size_v<T>>{});
    std::string args_joined;
    for (auto &arg : args) {
        args_joined += arg + ", ";
    }
    return x23::xdr_type_name<T>() + " (" + args_joined.substr(0, args_joined.size() - 2) + ")";
}

static_assert(xdr_event_string<TxEmittedEvent>() == "TxEmittedEvent (opaque[32], unsigned int, opaque<1024>)");

} // namespace hogd
