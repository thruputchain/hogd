#pragma once

#include <optional>

#include "hogd/hog_exec/runtime/rv32/rv32_contract.h"

namespace hogd {

// Whatever a runtime builds while verifying a contract and needs again in
// order to instantiate it.
//
// For RV32, verifying *is* translating: proving an artifact is translatable
// means parsing and lowering it. Keeping that image here is what lets a
// same-transaction deployment instantiate the contract without lowering the
// identical bytes a second time (#3536). Runtimes with nothing to prepare
// leave this empty.
//
// Held by value wherever it is cached, so preparing a contract costs no
// allocation beyond the image itself. RuntimeContext forward-declares this
// type and takes it by reference, which is what keeps the rv32 machinery out
// of the widely-included runtime.h.
//
// One member because one runtime prepares anything today. When a second
// runtime needs a preparation this becomes a variant over the per-runtime
// images; it stays default-constructible either way, since an out-param has
// to be constructible before the context fills it.
struct PreparedContract {
  std::optional<Rv32Contract> rv32;
};

} // namespace hogd
