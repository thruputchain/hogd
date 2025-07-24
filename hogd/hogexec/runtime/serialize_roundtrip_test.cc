#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <span>
#include <utility>

#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/config.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/multiplexed_context.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "transaction.hh"

#if ENABLE_TEST_CONTRACTS

#include <memory>
#include <unordered_map>

#include "hogd/base/cpputil.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/native_runtime.h"
#include "hogd/hogexec/runtime/serialize_calldata.h"
#include "hogd/hogexec/runtime/status.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "test_contracts/integration_test_contracts/cxs_roundtrip/cxs_roundtrip.h"
#include "test_contracts/roundtrip.hh"
#include "types.hh"

namespace hogd {

class SerializeRoundtripTest : public testing::Test,
                               public NativeContractTestFramework {
public:
  SerializeRoundtripTest() : NativeContractTestFramework(true) {}

protected:
  void SetUp() override
  {
    wasm_runtime_context_ = make_hogexec_wasm_context(RuntimeEngine::kWasmi);
  }

  std::shared_ptr<RuntimeContext> wasm_runtime_context_;
};

namespace {
using cxs_client_t = ContractTraitClient<CxsRoundtrip>;

template<StringLiteral MethodName, typename T>
void
roundtrip_test(T const &value, SystemCallHandler &handler, cxs_client_t &client,
               bool &success)
{
  success = false;
  auto calldata = Serializer<SerializationFormat::kCxs>::serialize(value);
  ASSERT_TRUE(calldata.has_value());
  auto [res, _gas] = client.invoke<MethodName>(handler, *calldata);
  ASSERT_TRUE(res.has_value());
  ASSERT_TRUE(res.value().has_value());
  auto return_buf = res.value().value();
  auto roundtrip_value = Serializer<SerializationFormat::kCxs>::deserialize<T>(
      std::as_bytes(std::span(return_buf)));
  ASSERT_TRUE(roundtrip_value.has_value());
  ASSERT_EQ(roundtrip_value.value(), value);
  success = true;
}
} // namespace

TEST_F(SerializeRoundtripTest, CxsSerializeRoundtrip)
{
  auto contract = test::load_test_contract("cxs_roundtrip.wasm");
  auto cxs_contract_addr = make_contract_address(0);
  in_memory_db_.write_contract(sha256(*contract), contract);
  in_memory_db_.write_deployment(cxs_contract_addr, sha256(*contract));
  setup_grant_storage_token(cxs_contract_addr, 1'000'000'000);

  using cxs_client_t = ContractTraitClient<CxsRoundtrip>;

  cxs_client_t client(cxs_contract_addr);

  auto native_contract = std::make_unique<
      NativeContract>(std::unordered_map<MethodID, NativeContract::function_t>{
      {0,
       [&](SystemCallHandler &handler,
           NativeRuntime & /*unused*/) -> InvokeStatus<void> {
         bool success = false;
         ::test::Foo foo{0xABCD};
         roundtrip_test<"roundtrip_foo">(foo, handler, client, success);
         if (!success) {
           return InvokeError{InvokeErrorCode::kInvokeDeterministic};
         }

         return {};
       }},
      {1,
       [&](SystemCallHandler &handler,
           NativeRuntime & /*unused*/) -> InvokeStatus<void> {
         bool success = false;
         ::test::senum senum{::test::senum::SE_POSITIVE};
         roundtrip_test<"roundtrip_senum">(senum, handler, client, success);
         if (!success) {
           return InvokeError{InvokeErrorCode::kInvokeDeterministic};
         }
         return {};
       }},
      {2,
       [&](SystemCallHandler &handler,
           NativeRuntime & /*unused*/) -> InvokeStatus<void> {
         bool success = false;
         ::test::sunion sunion{::test::senum::SE_POSITIVE};
         sunion.pos() = 5;
         roundtrip_test<"roundtrip_sunion">(sunion, handler, client, success);
         if (!success) {
           return InvokeError{InvokeErrorCode::kInvokeDeterministic};
         }
         sunion.d(static_cast<::test::senum>(0)); // default case
         roundtrip_test<"roundtrip_sunion">(sunion, handler, client, success);
         if (!success) {
           return InvokeError{InvokeErrorCode::kInvokeDeterministic};
         }
         sunion.d(static_cast<::test::senum>(1000)); // default case
         roundtrip_test<"roundtrip_sunion">(sunion, handler, client, success);
         if (!success) {
           return InvokeError{InvokeErrorCode::kInvokeDeterministic};
         }
         return {};
       }},
      {3,
       [&](SystemCallHandler &handler,
           NativeRuntime & /*unused*/) -> InvokeStatus<void> {
         bool success = false;
         int32_t value = -1;
         roundtrip_test<"roundtrip_i32">(value, handler, client, success);
         if (!success) {
           return InvokeError{InvokeErrorCode::kInvokeDeterministic};
         }

         value = 1;
         roundtrip_test<"roundtrip_i32">(value, handler, client, success);
         if (!success) {
           return InvokeError{InvokeErrorCode::kInvokeDeterministic};
         }

         value = 0;
         roundtrip_test<"roundtrip_i32">(value, handler, client, success);
         if (!success) {
           return InvokeError{InvokeErrorCode::kInvokeDeterministic};
         }

         value = std::numeric_limits<int32_t>::max();
         roundtrip_test<"roundtrip_i32">(value, handler, client, success);
         if (!success) {
           return InvokeError{InvokeErrorCode::kInvokeDeterministic};
         }

         value = std::numeric_limits<int32_t>::min();
         roundtrip_test<"roundtrip_i32">(value, handler, client, success);
         if (!success) {
           return InvokeError{InvokeErrorCode::kInvokeDeterministic};
         }

         return {};
       }},
      {4,
       [&](SystemCallHandler &handler,
           NativeRuntime & /*unused*/) -> InvokeStatus<void> {
         bool success = false;
         ::test::sunion_no_void sunion{::test::senum::SE_NEGATIVE};
         sunion.neg() = true;
         roundtrip_test<"roundtrip_sunion_no_void">(sunion, handler, client,
                                                    success);
         if (!success) {
           return InvokeError{InvokeErrorCode::kInvokeDeterministic};
         }
         return {};
       }},
      {5,
       [&](SystemCallHandler &handler,
           NativeRuntime & /*unused*/) -> InvokeStatus<void> {
         bool success = false;
         ::test::uunion uunion(1);
         uunion.one() = true;
         roundtrip_test<"roundtrip_uunion">(uunion, handler, client, success);
         if (!success) {
           return InvokeError{InvokeErrorCode::kInvokeDeterministic};
         }
         uunion.d(2);
         uunion.two() = true;
         roundtrip_test<"roundtrip_uunion">(uunion, handler, client, success);
         if (!success) {
           return InvokeError{InvokeErrorCode::kInvokeDeterministic};
         }
         uunion.d(4);
         uunion.four() = "hello";
         roundtrip_test<"roundtrip_uunion">(uunion, handler, client, success);
         if (!success) {
           return InvokeError{InvokeErrorCode::kInvokeDeterministic};
         }

         return {};
       }},
  });

  std::unordered_map<uint32_t, std::unique_ptr<NativeContract>>
      native_contract_map;
  native_contract_map.emplace(0, std::move(native_contract));
  auto native_runtime_context =
      std::make_shared<NativeContext>(std::move(native_contract_map));

  // deploy the native contract
  auto native_contract_xdr = make_native_contract(0);
  auto native_contract_addr = make_contract_address(1);
  in_memory_db_.write_contract(sha256(*native_contract_xdr),
                               native_contract_xdr);
  in_memory_db_.write_deployment(native_contract_addr,
                                 sha256(*native_contract_xdr));

  auto multiplexed_context = std::make_shared<MultiplexedContext>(
      std::unordered_map<ContractType, std::shared_ptr<RuntimeContext>>{
          {ContractType::kNative, native_runtime_context},
          {ContractType::kWasmi, wasm_runtime_context_}});

  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  for (size_t i = 0; i < 6; i++) {
    auto handler = make_new_system_call_handler(
        SignedTransaction{
            .tx =
                Transaction{
                    .invocation =
                        Invocation{.invoked_address = native_contract_addr,
                                   .entrypoint = entrypoint_from_method_id(i),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}}}},
        multiplexed_context);
    auto res = handler.root_invoke();
    ASSERT_TRUE(res.is_root_success());
    auto commit_res = handler.try_commit_transaction();
    ASSERT_TRUE(commit_res.has_value());
    ASSERT_TRUE(*commit_res);
  }
  // NOLINTEND(bugprone-unchecked-optional-access)
}

} // namespace hogd

#endif
