// Compile time tests
#include <cstddef>
#include <span>
#include <tuple>

#include "hogd/base/types.h"
#include "hogd/hog_exec/runtime/syscall_entry_outer.h"
#include "types.hh"

namespace hogd {

namespace {

auto offsets = make_offset_seqs<5, uint32_t, uint32_t, std::span<std::byte>,
                                SHA256Hash &, uint32_t>();

static_assert(get<0>(offsets) == 5);
static_assert(get<1>(offsets) == 6);
static_assert(get<2>(offsets) == 7);
static_assert(get<3>(offsets) == 9);
static_assert(get<4>(offsets) == 10);

static_assert(std::tuple_size<decltype(offsets)>{} == 5);

static_assert(std::tuple_size<decltype(make_offset_seqs<5>())>{} == 0);

} // namespace

} // namespace hogd
