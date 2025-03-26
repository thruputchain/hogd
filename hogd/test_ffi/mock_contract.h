#pragma once

#include <string_view>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/test_ffi/test_bindings.h"

namespace hogd {

class MockContract {
public:
  InvokeStatus<int32_t> invoke(std::string_view method);

  MockContract(ForeignEntryPoint entry_point, void *foreign_context)
    : entry_point_(entry_point), foreign_context_(foreign_context)
  {}

private:
  ForeignEntryPoint entry_point_;
  void *foreign_context_;
};

} // namespace hogd
