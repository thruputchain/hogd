#pragma once

#include <memory>

#include "hogd/hog_exec/runtime/runtime.h"

namespace hogd {

std::unique_ptr<RuntimeContext> make_hog_exec_precompiles_context();

std::unique_ptr<RuntimeContext> make_hog_exec_context();

} // namespace hogd
