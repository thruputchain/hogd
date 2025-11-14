#pragma once

#include <memory>

#include "hogd/hogexec/runtime/runtime.h"

namespace hogd {

std::unique_ptr<RuntimeContext> make_hogexec_precompiles_context();

std::unique_ptr<RuntimeContext> make_hogexec_context();

} // namespace hogd
