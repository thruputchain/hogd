#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <utility>

#include "consensus.hh"
#include "contract.hh"
#include "db_sched.hh"
#include "genesis.hh"
#include "hogd/base/io.h"
#include "hogd/base/network_config_params.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_db/rpc_context.h"
#include "hogd/hog_db/rpc_from_sched.h"
#include "hogd/hog_exec/common/limits.h"
#include "hogd/hog_exec/common/storage_output.h"
#include "hogd/hog_exec/runtime/rv32/program.h"
#include "hogd/hog_mempool/config_hash.h"
#include "hogd/ramdb/db.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "network_config.hh"
#include "storage_item.hh"
#include "types.hh"

namespace hogd {

// NOLINTBEGIN(bugprone-unchecked-optional-access)

namespace {
constexpr auto kContractCeiling =
    static_cast<uint32_t>(kContractSizeCeilingBytes);
} // namespace

// A genesis states max_bytestring_len, and the wire type only admits values up
// to kBytestringCeilingLen. Loading is the one place that bound is checked, so
// a chain naming more has to be refused here rather than aborting later on a
// write the syscall let through. The same goes for max_contract_size_bytes
// against what this build can store and commit.
class GenesisConfigValidationTest : public testing::Test {
protected:
  void SetUp() override
  {
    ErrnoStatus<Directory> dir = Directory::open_temporary();
    ASSERT_TRUE(static_cast<bool>(dir));

    constexpr size_t kHeapSize = 10_MiB;
    DBStatus<std::unique_ptr<Database>> db =
        Database::open(dir->get_path(), Database::estimate_heap_size(kHeapSize),
                       0);
    ASSERT_TRUE(db);
    ctx_opt_.emplace(HogDbDAO(RAMDB(std::move(*db))), nullptr);
  }

  // Loads a genesis stating `max_bytestring_len` and reports the result. The
  // header is built to match, so anything other than OK is this parameter's
  // check rather than the canonicity or hash gates ahead of it.
  DbSchedResult load_with_bytestring_limit(uint32_t max_bytestring_len)
  {
    NetworkConfig config = stock_network_config();
    NetworkConfigParams::set_param(config,
                                   ConfigParamID::kConfigParamMaxBytestringLen,
                                   max_bytestring_len);
    return load(config, std::nullopt);
  }

  // Loads a genesis stating `max_contract_size_bytes` and
  // `max_new_contracts_per_tx` and reports the result. Both, because the
  // commit-batch bound is on the pair.
  DbSchedResult load_with_contract_limits(uint32_t max_contract_size_bytes,
                                          uint32_t max_new_contracts_per_tx)
  {
    NetworkConfig config = stock_network_config();
    NetworkConfigParams::set_param(
        config, ConfigParamID::kConfigParamMaxContractSizeBytes,
        max_contract_size_bytes);
    NetworkConfigParams::set_param(
        config, ConfigParamID::kConfigParamMaxNewContractsPerTx,
        max_new_contracts_per_tx);
    return load(config, std::nullopt);
  }

  // Loads a genesis naming `default_wallet` and reports the result.
  DbSchedResult
  load_with_default_wallet(DeployedContractHash const &default_wallet)
  {
    return load(stock_network_config(), default_wallet);
  }

  // Loads a genesis built from `config`, overriding the default wallet where
  // one is given. The header is always built to match, so anything other than
  // OK comes from a check past the canonicity and hash gates.
  DbSchedResult load(NetworkConfig const &config,
                     std::optional<DeployedContractHash> const &default_wallet)
  {
    GenesisConfigurationBuilder builder;
    if (default_wallet.has_value()) {
      builder.set_default_wallet(*default_wallet);
    }
    const StorageCostModel cost_model = StorageCostModel::default_cost_model();
    GenesisConfiguration genesis =
        builder.build(NetworkHash{}, cost_model,
                      *load_stock_genesis_root_contract(), config);

    ConsensusValue value{};
    value.proposal_header.block_height = 0;
    value.proposal_header.network_hash = genesis.network_hash;
    value.proposal_header.config_hash = compute_config_hash(genesis);

    HogDbSchedRpc sched_rpc(*ctx_opt_);
    return sched_rpc.dbsched_load_genesis_configuration(std::move(genesis),
                                                        std::move(value))();
  }

  std::optional<RPCContext> ctx_opt_;
};

// The ceiling itself is admissible: it is the largest bytestring the wire
// format carries, so a chain may configure exactly it.
TEST_F(GenesisConfigValidationTest, AcceptsBytestringLimitAtCeiling)
{
  EXPECT_EQ(load_with_bytestring_limit(
                static_cast<uint32_t>(kBytestringCeilingLen)),
            DbSchedResult::DbSchedResultOK);
}

// One byte past it is refused. Without this the write path would admit a
// bytestring no peer could decode, and BytestringItem::validate would turn a
// configuration error into an abort.
TEST_F(GenesisConfigValidationTest, RejectsBytestringLimitAboveCeiling)
{
  EXPECT_EQ(load_with_bytestring_limit(
                static_cast<uint32_t>(kBytestringCeilingLen) + 1),
            DbSchedResult::DbSchedResultError);
}

// A database loads genesis once, so each case below ends in at most one
// successful load, after any it expects refused.

// A contract at the ceiling loads with the stock attachment count; one byte
// past it is refused.
TEST_F(GenesisConfigValidationTest, BoundsContractSizeLimitByCeiling)
{
  EXPECT_EQ(load_with_contract_limits(kContractCeiling + 1, 1),
            DbSchedResult::DbSchedResultError);
  const uint32_t stock_count =
      NetworkConfigParams(stock_network_config().v0()).max_new_contracts_per_tx;
  EXPECT_EQ(load_with_contract_limits(kContractCeiling, stock_count),
            DbSchedResult::DbSchedResultOK);
}

// The commit-batch bound is on what a transaction can realize, not the
// product of the two maxima, so a count at the ceiling still loads. Pinned
// from the constants.
TEST_F(GenesisConfigValidationTest, CommitBatchIsNotTheProductOfTheMaxima)
{
  constexpr size_t kBatchOverhead = 1_KiB;
  const auto product_fits = static_cast<uint32_t>(
      (kExecToDbMessageCap - kBatchOverhead) / kMaxSerializedContractBytes);
  ASSERT_GE(product_fits, 1U);
  EXPECT_EQ(load_with_contract_limits(kContractCeiling, product_fits + 1),
            DbSchedResult::DbSchedResultOK);
}

// Past the count whose per-row slack alone overflows the message, the pair is
// refused whatever the size, and one fewer is admitted.
TEST_F(GenesisConfigValidationTest,
       CommitBatchIsBoundedByWhatATransactionCarries)
{
  constexpr size_t kBatchOverhead = 1_KiB;
  const auto slack_fits = static_cast<uint32_t>(
      (kExecToDbMessageCap - kBatchOverhead - kContractSizeCeilingBytes) /
      kSerializedContractEnvelopeBytes);
  ASSERT_GT(slack_fits, 1U);
  EXPECT_EQ(load_with_contract_limits(kContractCeiling, slack_fits + 1),
            DbSchedResult::DbSchedResultError);
  EXPECT_EQ(load_with_contract_limits(1_KiB, slack_fits + 1),
            DbSchedResult::DbSchedResultError);
  EXPECT_EQ(load_with_contract_limits(kContractCeiling, slack_fits),
            DbSchedResult::DbSchedResultOK);
}

// Savepoint depth may be lowered freely and raised up to the executor's
// ceiling; past it the executor could not honour the configuration.
TEST_F(GenesisConfigValidationTest, BoundsSavepointDepthByExecutorCeiling)
{
  auto load_with_savepoint_depth = [&](uint32_t depth) {
    NetworkConfig config = stock_network_config();
    NetworkConfigParams::set_param(config,
                                   ConfigParamID::kConfigParamMaxSavepointDepth,
                                   depth);
    return load(config, std::nullopt);
  };
  EXPECT_EQ(load_with_savepoint_depth(kSavepointDepthCeiling + 1),
            DbSchedResult::DbSchedResultError);
  EXPECT_EQ(load_with_savepoint_depth(kSavepointDepthCeiling),
            DbSchedResult::DbSchedResultOK);
}

// Root commits need one savepoint even when child TryInvokes are disabled.
TEST_F(GenesisConfigValidationTest, RequiresSavepointForRootCommit)
{
  NetworkConfig config = stock_network_config();
  NetworkConfigParams::set_param(config,
                                 ConfigParamID::kConfigParamMaxSavepointDepth,
                                 0);
  EXPECT_EQ(load(config, std::nullopt), DbSchedResult::DbSchedResultError);
  NetworkConfigParams::set_param(config,
                                 ConfigParamID::kConfigParamMaxSavepointDepth,
                                 1);
  EXPECT_EQ(load(config, std::nullopt), DbSchedResult::DbSchedResultOK);
}

// The RV32 bounds may be raised up to what the VM provisions and translates.
TEST_F(GenesisConfigValidationTest, BoundsRv32LimitsByVmCeilings)
{
  auto load_with_rv32 = [&](ConfigParamID id, uint32_t value) {
    NetworkConfig config = stock_network_config();
    NetworkConfigParams::set_param(config, id, value);
    return load(config, std::nullopt);
  };
  EXPECT_EQ(load_with_rv32(ConfigParamID::kConfigParamMaxRv32CodeBytes,
                           rv32::kCodeCeilingBytes + 1),
            DbSchedResult::DbSchedResultError);
  EXPECT_EQ(load_with_rv32(ConfigParamID::kConfigParamMaxRv32MemoryBytes,
                           rv32::kMemoryCeilingBytes + 1),
            DbSchedResult::DbSchedResultError);
  EXPECT_EQ(load_with_rv32(ConfigParamID::kConfigParamMaxRv32MemoryBytes,
                           rv32::kMemoryCeilingBytes),
            DbSchedResult::DbSchedResultOK);
}

// The subtransaction output limit runs from one delta, which every root commit
// carries, up to what this build sends HogDb in one message.
TEST_F(GenesisConfigValidationTest, BoundsSubtransactionOutputLimit)
{
  auto load_with_output_limit = [&](size_t value) {
    NetworkConfig config = stock_network_config();
    NetworkConfigParams::set_param(
        config, ConfigParamID::kConfigParamMaxSubtransactionOutputBytes,
        static_cast<uint32_t>(value));
    return load(config, std::nullopt);
  };
  EXPECT_EQ(load_with_output_limit(kStorageOutputBytesPerDelta - 1),
            DbSchedResult::DbSchedResultError);
  EXPECT_EQ(load_with_output_limit(kSubtransactionOutputCeilingBytes + 1),
            DbSchedResult::DbSchedResultError);
  EXPECT_EQ(load_with_output_limit(kSubtransactionOutputCeilingBytes),
            DbSchedResult::DbSchedResultOK);
}

TEST_F(GenesisConfigValidationTest, AcceptsRv32CodeLimitAtVmCeiling)
{
  NetworkConfig config = stock_network_config();
  NetworkConfigParams::set_param(config,
                                 ConfigParamID::kConfigParamMaxRv32CodeBytes,
                                 rv32::kCodeCeilingBytes);
  EXPECT_EQ(load(config, std::nullopt), DbSchedResult::DbSchedResultOK);
}

// The stock wallet is written among the genesis prerequisites, so a genesis
// that does not override it loads.
TEST_F(GenesisConfigValidationTest, AcceptsStockDefaultWallet)
{
  EXPECT_EQ(load_with_default_wallet(native_default_wallet_hash()),
            DbSchedResult::DbSchedResultOK);
}

// So is one naming a wallet with no height-0 contract row. A chain may fund
// its accounts with its own wallet artifact and name that as its default, and
// the ceremony attaches that code in the height-1 block -- so requiring the
// row here would refuse a supported chain. validate_recovered_tip carries the
// requirement instead, from the ceremony onward.
TEST_F(GenesisConfigValidationTest, AcceptsDefaultWalletTheCeremonyDeploys)
{
  DeployedContractHash ceremony_deployed = native_default_wallet_hash();
  ceremony_deployed.hash.fill(0xE7);
  EXPECT_EQ(load_with_default_wallet(ceremony_deployed),
            DbSchedResult::DbSchedResultOK);
}

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
