// Compile time tests
#include <cstddef>
#include <span>
#include <tuple>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/syscall_entry_outer.h"
#include "types.hh"

namespace hogd {

namespace {

auto offsets = make_offset_seqs<5, uint64_t, uint64_t, std::span<std::byte>, SHA256Hash&, uint64_t>();

static_assert(get<0>(offsets) == 5);
static_assert(get<1>(offsets) == 6);
static_assert(get<2>(offsets) == 7);
static_assert(get<3>(offsets) == 9);
static_assert(get<4>(offsets) == 10);

static_assert(std::tuple_size<decltype(offsets)>{} == 5);

}

}