#pragma once

#include <cassert>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "contract.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/components/execution_mode.h"
#include "hogd/hogexec/runtime/event_server_proxy.h"
#include "hogd/hogexec/runtime/item/database_proxy.h"
#include "hogd/hogexec/runtime/item/storage_snapshot_cache.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/test_util/hogdb_wrapper.h"
#include "hogd/hogexec/runtime/test_util/test_event_aggregator.h"
#include "hogd/hogexec/runtime/wasm/wasmi_runtime.h"
#include "hogd/hogexec/runtime/wasm/wasmtime_runtime.h"
#include "transaction.hh"
#include "x23/sockio.h"

namespace hogd {

enum class WasmEngine {
  kWasmi,
  kWasmtime,
};

// Boilerplate for testing just the wasm runtime integrations.
// Required because of how the runtime context lazily loads wasm contracts --
// this setup is what suffices to create a SystemCallHandler that can do nothing
// except supply those contracts.
//
// The alternative would be to add an extra layer of indirection
// within the syscall handler.
class WasmTestFramework {
public:
  void make_syscall_handler(Contract const &contract)
  {
    auto *context = get_context();
    assert(context != nullptr);
    Transaction tx{};
    tx.contracts_to_deploy.push_back(contract);
    syscall_handler_ = std::make_unique<SystemCallHandler>(
        std::move(tx), snapshot_cache_,
        PerTransactionDatabaseProxy(db_proxy_, 0, 0),
        PerTransactionEventServerProxy(event_server_proxy_, 0, 0),
        ProposalModeCtor{}, *context);
  }

  // Initialize the global context and runtime context for parameterized tests
  void setup_wasm_context(WasmEngine engine, size_t wasmtime_engine_count = 1)
  {
    switch (engine) {
    case WasmEngine::kWasmi:
      global_context_.emplace<WasmiGlobalContext>();
      break;
    case WasmEngine::kWasmtime:
      global_context_.emplace<WasmtimeGlobalContext>(wasmtime_engine_count);
      break;
    }
  }

  void set_context(std::unique_ptr<RuntimeContext> context)
  {
    context_ = std::move(context);
  }

  // Link a function to the global context
  template<typename Fn> requires supported_host_fn_ptr<Fn>
  bool link_fn(std::string_view module_name, std::string_view fn_name, Fn f)
  {
    assert(!context_); // don't link functions after a context has been created
    if (std::holds_alternative<WasmiGlobalContext>(global_context_)) {
      return std::get<WasmiGlobalContext>(global_context_)
          .link_fn(module_name, fn_name, f);
    }
    return std::get<WasmtimeGlobalContext>(global_context_)
        .link_fn(module_name, fn_name, f);
  }

  // Get the runtime context (for creating runtimes)
  RuntimeContext *get_context()
  {
    if (context_) {
      return context_.get();
    }

    assert(!std::holds_alternative<std::monostate>(global_context_));

    if (std::holds_alternative<WasmiGlobalContext>(global_context_)) {
      context_ = std::make_unique<WasmiContext>(
          std::get<WasmiGlobalContext>(global_context_));
    }
    else {
      assert(std::holds_alternative<WasmtimeGlobalContext>(global_context_));
      context_ = std::make_unique<WasmtimeContext>(
          std::get<WasmtimeGlobalContext>(global_context_));
    }
    return context_.get();
  }

  // Convert WasmEngine enum to string for test parameter names
  static std::string engine_to_string(WasmEngine engine)
  {
    return engine == WasmEngine::kWasmi ? "Wasmi" : "Wasmtime";
  }

  SystemCallHandler *get_syscall_handler()
  {
    CHECK(syscall_handler_.get() != nullptr);
    return syscall_handler_.get();
  }

private:
  std::variant<std::monostate, WasmiGlobalContext, WasmtimeGlobalContext>
      global_context_ = std::monostate{};
  std::unique_ptr<RuntimeContext> context_;

  // This handler is used to load contracts to the runtime context.
  std::unique_ptr<SystemCallHandler> syscall_handler_;

  // Boilerplate irrelevant for the test, just for creating the syscall handler.
  HogdbWrapper hogdb_wrapper_;
  TestEventAggregator event_server_;
  TestEventAggregatorExecServer event_server_exec_rpc_ =
      TestEventAggregatorExecServer(event_server_);

  DatabaseProxy db_proxy_ = DatabaseProxy(
      LocalMockDB{.read_port_number_ = hogdb_wrapper_.exec_port()});
  StorageSnapshotCache snapshot_cache_ = StorageSnapshotCache(0, db_proxy_);
  EventServerProxy event_server_proxy_ = EventServerProxy(
      x23::HostPort{"localhost", event_server_exec_rpc_.start_server(0)});
};

} // namespace hogd
