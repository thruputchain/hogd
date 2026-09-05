#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "hogd/base/network_config_params.h"
#include "hogd/base/types.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/native_runtime.h"
#include "hogd/hog_exec/runtime/native_syscall_context.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "hogd/hog_exec/runtime/syscall_inner.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "network_config.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "syscall.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"

namespace hogd {
// NOLINTBEGIN(bugprone-exception-escape)

namespace {

// Small enough that the over-limit cases stay cheap, and far below
// kBytestringCeilingLen so the two bounds cannot be confused for each other.
constexpr uint32_t kMaxBytestringLen = 256;

// Stands in for an item admitted when a higher limit was in force. Over the
// configured limit, under the ceiling -- which is the only combination that
// distinguishes the write bound from the decode bound.
constexpr size_t kLegacyItemLen = 4 * kMaxBytestringLen;

constexpr static StorageKeyContractData kKey{};

Bytestring
filled_bytestring(size_t len, uint8_t byte)
{
  Bytestring out;
  out.resize_uninitialized(len);
  std::fill(out.begin(), out.end(), byte);
  return out;
}

} // namespace

// kConfigParamMaxBytestringLen bounds what a contract may write, and nothing
// else. These pin the asymmetry that makes the parameter safe to lower: a
// write over the limit is refused, while an item already larger than it stays
// readable, because a contract has no way to shrink state it can no longer
// rewrite.
class BytestringLimitTest : public ::testing::Test,
                            public NativeContractTestFramework {
protected:
  BytestringLimitTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb, true, std::nullopt, genesis_network_config())
  {}

  void SetUp() override
  {
    setup_grant_storage_token(make_contract_address(0), 1'000'000'000);
  }

  // The contract-data addrkey for kKey, built from the raw key rather than a
  // string: these tests address the same slot the probes pass to the syscall.
  static AddressAndKey seeded_addrkey()
  {
    StorageKey full_key(StorageKeyType::ContractData);
    full_key.contract_data_key() = kKey;
    return AddressAndKey{.address = make_contract_address(0),
                         .key = std::move(full_key)};
  }

  // Runs `probe` as contract 0's entrypoint 0. The probe reports through
  // ADD_FAILURE and by failing its own frame, so a successful root is the pass
  // condition.
  void run_probe(NativeContract::function_t probe)
  {
    auto contract_obj = make_native_contract(0);
    write_contract(contract_obj);
    write_deployment(make_contract_address(0), contract_obj);

    std::unordered_map<Runtime::MethodID, NativeContract::function_t> fns;
    fns.emplace(0, std::move(probe));
    std::unordered_map<int32_t, std::unique_ptr<NativeContract>> contract_map;
    contract_map.emplace(0, std::make_unique<NativeContract>(std::move(fns)));

    auto runtime_context =
        std::make_unique<NativeContext>(std::move(contract_map));
    auto handler = make_new_system_call_handler(
        Transaction{.invocation =
                        Invocation{.invoked_address = make_contract_address(0),
                                   .entrypoint = entrypoint_from_method_id(0),
                                   .gas_limit = UINT64_MAX,
                                   .calldata = {}},
                    .uniquifier = make_new_uniquifier()},
        *runtime_context);
    ASSERT_TRUE(handler.root_invoke()().result_);
  }

private:
  static NetworkConfig genesis_network_config()
  {
    NetworkConfig config = stock_network_config();
    set_config_param(config, ConfigParamID::kConfigParamMaxBytestringLen,
                     kMaxBytestringLen);
    return config;
  }
};

// The limit is inclusive: the largest value a chain states is writable.
TEST_F(BytestringLimitTest, WriteAtConfiguredLimitIsAccepted)
{
  run_probe([](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
    auto value = filled_bytestring(kMaxBytestringLen, 'a');
    auto [res, _gas] = co_await syscall_BytestringSet_inner(
        ctx.handler(), kKey, std::as_bytes(std::span(value)),
        std::to_underlying(StorageDeltaFlags::None));
    if (!res.has_value() ||
        res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
      ADD_FAILURE() << "a write at the configured limit must be accepted";
      co_return InvokeError{InvokeErrorCode::Deterministic};
    }
    co_return InvokeStatus<void>{};
  });
}

// One byte past the limit is refused as bad input rather than truncated or
// charged, and leaves no item behind.
TEST_F(BytestringLimitTest, WriteOverConfiguredLimitIsRejected)
{
  run_probe([](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
    auto value = filled_bytestring(kMaxBytestringLen + 1, 'b');
    auto [res, _gas] = co_await syscall_BytestringSet_inner(
        ctx.handler(), kKey, std::as_bytes(std::span(value)),
        std::to_underlying(StorageDeltaFlags::None));
    if (!res.has_value()) {
      ADD_FAILURE() << "an over-limit write must be a syscall error, not a "
                       "system error";
      co_return InvokeError{InvokeErrorCode::Deterministic};
    }
    if (res.value() != SyscallErrorCode::kSyscallErrorCodeInvalidSyscallInput) {
      ADD_FAILURE() << "an over-limit write must be rejected as invalid input";
      co_return InvokeError{InvokeErrorCode::Deterministic};
    }

    // The rejection is a no-op: no partial item is left at the key.
    auto after = co_await ctx.handler().get_bytestring(kKey);
    if (!after.has_value()) {
      ADD_FAILURE() << "reading back after a rejected write failed outright";
      co_return InvokeError{InvokeErrorCode::Deterministic};
    }
    if (after.value().has_value()) {
      ADD_FAILURE() << "a rejected write must leave no item behind";
      co_return InvokeError{InvokeErrorCode::Deterministic};
    }
    co_return InvokeStatus<void>{};
  });
}

// An item already larger than the limit in force stays readable in full. This
// is what lets a chain lower the parameter without stranding state: the write
// bound is the configured limit, the decode bound is kBytestringCeilingLen.
TEST_F(BytestringLimitTest, ItemOverConfiguredLimitStaysReadable)
{
  auto addrkey = seeded_addrkey();
  auto seeded = filled_bytestring(kLegacyItemLen, 'c');
  write_item(addrkey,
             std::make_shared<StorageItemWrapper>(seeded,
                                                  get_default_cost_model()));
  advance_block();

  run_probe([seeded](
                NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
    auto got = co_await ctx.handler().get_bytestring(kKey);
    if (!got.has_value() || !got.value().has_value()) {
      ADD_FAILURE() << "an item larger than the configured limit must still "
                       "be readable";
      co_return InvokeError{InvokeErrorCode::Deterministic};
    }
    if (!std::ranges::equal(*got.value().value(), seeded)) {
      ADD_FAILURE() << "the over-limit item must read back in full, not "
                       "truncated to the configured limit";
      co_return InvokeError{InvokeErrorCode::Deterministic};
    }
    co_return InvokeStatus<void>{};
  });
}

// The same tolerance through the guest-facing syscall, which copies into a
// contract-supplied buffer rather than handing back the item.
TEST_F(BytestringLimitTest, BytestringGetSyscallReadsOverLimitItem)
{
  auto addrkey = seeded_addrkey();
  auto seeded = filled_bytestring(kLegacyItemLen, 'd');
  write_item(addrkey,
             std::make_shared<StorageItemWrapper>(seeded,
                                                  get_default_cost_model()));
  advance_block();

  run_probe([](NativeSyscallContext &ctx) -> x23::Async<InvokeStatus<void>> {
    std::vector<uint8_t> buffer(kLegacyItemLen);
    RuntimeMemoryLength written_len = 0;
    auto res = co_await ctx.syscall<SyscallNo::kBytestringGet>(
        kKey, std::as_writable_bytes(std::span(buffer)), written_len);
    if (!res.has_value() ||
        res.value() != SyscallErrorCode::kSyscallErrorCodeNone) {
      ADD_FAILURE() << "BytestringGet must succeed on an over-limit item";
      co_return InvokeError{InvokeErrorCode::Deterministic};
    }
    if (written_len != kLegacyItemLen) {
      ADD_FAILURE() << "expected " << kLegacyItemLen << " bytes, got "
                    << written_len;
      co_return InvokeError{InvokeErrorCode::Deterministic};
    }
    for (size_t i = 0; i < kLegacyItemLen; i++) {
      if (buffer[i] != 'd') {
        ADD_FAILURE() << "over-limit item read back with wrong contents at "
                      << i;
        co_return InvokeError{InvokeErrorCode::Deterministic};
      }
    }
    co_return InvokeStatus<void>{};
  });
}

// NOLINTEND(bugprone-exception-escape)
} // namespace hogd
