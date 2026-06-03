#pragma once

#include "hogd/hogexec/common/status.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/test_ffi/test_bindings.h"

namespace hogd {

// Wrapper around a function pointer, implementing a contract mocked out via
// the test harness (or some other mechanism).
class MockContract {
public:
  InvokeStatus<void> invoke(Runtime::MethodID method_id);

  MockContract(ForeignEntryPoint entry_point, void *foreign_context)
    : entry_point_(entry_point), foreign_context_(foreign_context)
  {}

private:
  ForeignEntryPoint entry_point_;
  void *foreign_context_;
};

} // namespace hogd
