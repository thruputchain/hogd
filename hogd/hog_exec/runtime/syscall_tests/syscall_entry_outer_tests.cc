// Compile time tests
#include <cstddef>
#include <span>
#include <type_traits>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/hog_exec/runtime/syscall_entry_outer.h"
#include "types.hh"

namespace hogd {

namespace {

static_assert(
    std::is_same_v<
        decltype(make_offset_seqs<5, uint32_t, uint32_t, std::span<std::byte>,
                                  SHA256Hash &, uint32_t>()),
        std::index_sequence<5, 6, 7, 9, 10>>);

static_assert(
    std::is_same_v<decltype(make_offset_seqs<5>()), std::index_sequence<>>);

} // namespace

} // namespace hogd
