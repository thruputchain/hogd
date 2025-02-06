#pragma once

#include <map>
#include <string>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/mock_runtime_ffi.h"
#include "hogd/hogexec/runtime/status.h"

namespace hogd {

class MockContract {
public:
  InvokeStatus<int32_t> invoke(std::string const &method);

  void add_method(std::string const &name,
                  detail::ForeignEntryPoint entry_point)
  {
    contract_entrypoints_[name] = entry_point;
  }

private:
  std::map<std::string, detail::ForeignEntryPoint> contract_entrypoints_;
};

} // namespace hogd
