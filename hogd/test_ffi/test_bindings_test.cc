#include <cstring>
#include <gtest/gtest.h>
#include <span>
#include <utility>

#include "contract.hh"
#include "event.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/test_ffi/syscall_handler_wrapper.h"
#include "hogd/test_ffi/test_bindings.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/cleanup.h"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

namespace {

void *
make_db()
{
  auto db = create_db();
  EXPECT_EQ(db.ffi_error_code, kTestFFIErrorCodeNone);
  return db.db;
}

void *
make_runtime_context()
{
  auto rc = open_runtime_context();
  EXPECT_EQ(rc.ffi_error_code, kTestFFIErrorCodeNone);
  return rc.context;
}

void *
make_syscall_handler(void *db, void *rc, SignedTransaction const &stx)
{
  auto serialized_stx = x23::serialize(stx);
  auto result = open_syscall_handler(db, rc, serialized_stx.data(),
                                     serialized_stx.size());
  EXPECT_EQ(result.ffi_error_code, kTestFFIErrorCodeNone);
  return result.syscall_handler;
}

uint32_t
get_event_log_len_unwrapped(void *syscall_handler)
{
  auto result = get_event_log_len(syscall_handler);
  EXPECT_EQ(result.ffi_error_code, kTestFFIErrorCodeNone);
  return result.len;
}

} // namespace

// Simple tests to make sure that the exported test functions work on at least a
// basic level. This one, for example, catches an error in initializing hogd.
TEST(TestBindingsTest, LazyInitTest)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  free_db(db);
}

TEST(TestBindingsTest, MissingContractOnDeploymentTest)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  ContractAddress deployment_addr{};

  Contract contract{};
  auto hash = sha256(contract);

  // the contract size is for deployment costs, which we ignore in this test.
  auto result =
      setup_deploy_contract(db, deployment_addr.data(), hash.data(), 100);
  ASSERT_EQ(result, kTestFFIErrorCodeDeployNonexistentContract);
}

TEST(TestBindingsTest, MissingMockContractTest)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  Contract contract(ContractType::kMockTest);
  contract.mock_contract_id() = 1;

  auto bytes = x23::serialize(contract);

  // the contract size is for deployment costs, which we ignore in this test.
  auto result = setup_create_contract(rc, db, bytes.data(), bytes.size());
  ASSERT_EQ(result, kTestFFIErrorCodeCreateNonexistentMockContract);
}

namespace {
// Test mock contract that creates an event
// In this test, we do not use the method number nor the foreign_context.
ForeignInvokeResult
mock_contract_create_event(void * /*unused*/, uint64_t method_number)
{
  if (method_number != 0x1234) {
    return ForeignInvokeResult{
        .panic = std::to_underlying(InvokeErrorCode::kInvokeUnrecoverable)};
  }

  auto *context = SyscallHandlerContext::get_context();
  if (!context) {
    return ForeignInvokeResult{
        .panic = std::to_underlying(InvokeErrorCode::kInvokeUnrecoverable)};
  }

  context->to_handler().log_event(nullptr, nullptr, nullptr,
                                  std::as_bytes(std::span("test_event")));

  return ForeignInvokeResult{
      .panic = std::to_underlying(InvokeErrorCode::kInvokeNone)};
}
} // namespace

TEST(TestBindingsTest, MockContractCreateEventTest)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  ASSERT_EQ(register_new_contract(rc, 1, mock_contract_create_event, nullptr),
            kTestFFIErrorCodeNone);

  Contract contract(ContractType::kMockTest);
  contract.mock_contract_id() = 1;
  auto bytes = x23::serialize(contract);
  ASSERT_EQ(setup_create_contract(rc, db, bytes.data(), bytes.size()),
            kTestFFIErrorCodeNone);

  ContractAddress deployment_addr{};
  ASSERT_EQ(setup_deploy_contract(db, deployment_addr.data(),
                                  sha256(contract).data(), bytes.size()),
            kTestFFIErrorCodeNone);

  SignedTransaction stx{
      .tx = {.invocation =
                 {
                     .invoked_address = deployment_addr,
                     .entrypoint = entrypoint_from_method_id(0x1234),
                     .gas_limit = 1'000'000,
                     .calldata = {},
                 }},
      .signatures = {},
  };

  auto serialized_stx = x23::serialize(stx);
  auto *syscall_handler = make_syscall_handler(db, rc, stx);
  x23::Defer d3([syscall_handler] { close_syscall_handler(syscall_handler); });

  auto result = root_invoke(syscall_handler);
  EXPECT_EQ(result.result.panic,
            std::to_underlying(InvokeErrorCode::kInvokeNone));

  // Events not available until next block

  auto commit_res = try_commit_transaction(syscall_handler);
  EXPECT_EQ(commit_res.result_code, 1);
  EXPECT_EQ(commit_res.panic, 0);
  EXPECT_EQ(commit_res.ffi_error_code, kTestFFIErrorCodeNone);

  ASSERT_EQ(advance_new_block(db), kTestFFIErrorCodeNone);

  auto event_log_len = get_event_log_len_unwrapped(syscall_handler);
  EXPECT_GT(event_log_len, 0);

  x23::Buf event_log_buffer;
  // incorrect size to get event data
  EXPECT_NE(get_event_log(syscall_handler, event_log_buffer.data(),
                          event_log_buffer.size()),
            kTestFFIErrorCodeNone);

  event_log_buffer.resize(event_log_len + 1);
  EXPECT_NE(get_event_log(syscall_handler, event_log_buffer.data(),
                          event_log_buffer.size()),
            kTestFFIErrorCodeNone);

  event_log_buffer.resize(event_log_len);
  EXPECT_EQ(get_event_log(syscall_handler, event_log_buffer.data(),
                          event_log_buffer.size()),
            kTestFFIErrorCodeNone);

  TxEventLog event_log;
  x23::deserialize(&event_log, event_log_buffer);
  EXPECT_EQ(event_log.events.size(), 1);
  EXPECT_EQ(event_log.events[0].contract_address, deployment_addr);

  EventData event_data;
  x23::deserialize(&event_data,
                   x23::BufView{event_log.events[0].serialized_event});
  EXPECT_EQ(event_data.topics.size(), 0);
  EXPECT_EQ(event_data.event.size(), 11);
  EXPECT_EQ(std::memcmp(event_data.event.data(), "test_event", 11), 0);
}

} // namespace hogd
