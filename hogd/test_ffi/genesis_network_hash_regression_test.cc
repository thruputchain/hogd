// Regression test for the test_ffi chain's genesis network hash.
// See test_ffi_test_helpers.h.

#include <cstdint>
#include <utility>

#include <gtest/gtest.h>

#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/test_ffi/syscall_handler_wrapper.h"
#include "hogd/test_ffi/test_bindings.h"
#include "hogd/test_ffi/test_ffi_test_helpers.h"
#include "hogd/test_ffi/test_genesis_root_contract.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/cleanup.h"
#include "x23/serialize.h"

namespace hogd {

namespace {

// A network hash that is not all zeros, so a host that drops the caller's
// value and leaves the field default fails rather than coincidentally matching.
NetworkHash
test_network_hash()
{
  NetworkHash out{};
  for (size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<uint8_t>(i + 1);
  }
  return out;
}

// What GetNetworkHash reported inside the contract, recorded for the assertion
// after the invocation returns.
NetworkHash g_observed_network_hash{};

ForeignInvokeResult
mock_contract_read_network_hash(void * /*unused*/, uint64_t method_number)
{
  if (method_number != 0x1234) {
    return ForeignInvokeResult{
        .panic = std::to_underlying(InvokeErrorCode::Unrecoverable)};
  }

  auto *context = SyscallHandlerContext::get_context();
  if (!context) {
    return ForeignInvokeResult{
        .panic = std::to_underlying(InvokeErrorCode::Unrecoverable)};
  }

  g_observed_network_hash = context->to_handler().get_network_hash();

  return ForeignInvokeResult{.panic =
                                 std::to_underlying(InvokeErrorCode::None)};
}

} // namespace

// DBContext took a NetworkHash and used it only for the AuthServer, which
// verifies transaction signatures; the GenesisConfiguration it built hardcoded
// .network_hash = {}. The chain's genesis header therefore claimed a zero
// network hash, the db served that faithfully, and GetNetworkHash reported 32
// zero bytes to contracts -- a different network than the one the same host was
// checking signatures against.
//
// A contract binding an off-chain signed message to this network needs the
// value it reads and the value signatures are verified against to be the same,
// so this pins that what create_db() is given is what a contract sees.
TEST(TestBindingsTest, GenesisCarriesTheConfiguredNetworkHash)
{
  const NetworkHash expected = test_network_hash();
  g_observed_network_hash = NetworkHash{};

  auto genesis_root = x23::serialize(make_test_genesis_root_contract());
  auto db_result = create_db(nullptr, 0, expected.data(), genesis_root.data(),
                             genesis_root.size(), nullptr, 0);
  ASSERT_EQ(db_result.ffi_error_code, kTestFFIErrorCodeNone);
  ASSERT_NE(db_result.db, nullptr);
  void *db = db_result.db;
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  ContractAddress deployment_addr =
      deploy_mock_contract(db, rc, 1, mock_contract_read_network_hash, nullptr);

  SignedTransaction stx{
      .tx = {.invocation =
                 {
                     .invoked_address = deployment_addr,
                     .entrypoint = entrypoint_from_method_id(0x1234),
                     .gas_limit = 1'000'000,
                     .calldata = {},
                 },
             .uniquifier = {.block_expiry_height = INT64_MAX, .nonce = 0}},
      .signatures = {},
  };

  auto *syscall_handler = make_syscall_handler(db, rc, stx);
  ASSERT_NE(syscall_handler, nullptr);

  auto result = root_invoke(syscall_handler, 0);
  ASSERT_EQ(result.ffi_error_code, kTestFFIErrorCodeNone);
  free_ffi_data_buffer(result.serialized_output);
  free_ffi_data_buffer(result.serialized_events);
  free_ffi_data_buffer(result.serialized_trace);

  EXPECT_EQ(g_observed_network_hash, expected);
  EXPECT_NE(g_observed_network_hash, NetworkHash{});

  EXPECT_EQ(close_syscall_handler(syscall_handler), kTestFFIErrorCodeNone);
}

} // namespace hogd
