#pragma once

#include <memory>

#include "hogd/hogexec/runtime/runtime.h"

namespace hogd {

std::shared_ptr<RuntimeContext> make_hogexec_precompiles_context();

std::shared_ptr<RuntimeContext> make_hogexec_context();

} // namespace hogd
