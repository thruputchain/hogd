#include <gtest/gtest.h>

#include <cstddef>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include "consensus.hh"
#include "contract.hh"
#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/base/network_config_params.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_db/hog_types.h"
#include "hogd/hog_db/rpc_context.h"
#include "hogd/hog_db/startup_validation.h"
#include "hogd/hog_db/status.h"
#include "hogd/hog_mempool/config_hash.h"
#include "hogd/ramdb/db.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "network_config.hh"
#include "precompiles.hh"
#include "rpc_types.hh"
#include "storage_cost.hh"
#include "types.hh"
#include "x23/serialize.h"

namespace hogd {

namespace {

constexpr std::size_t kHeapSize = 10_MiB;
constexpr BlockTXID kBlock = 7;

// Past the height-1 ceremony, so the wallet-row rule is in force.
constexpr BlockHeight kTipHeight = 4;

Contract
native_default_wallet_contract()
{
  Contract contract;
  contract.contract_type(ContractType::kNative);
  contract.native_contract_id() =
      HogexecPrecompiledNativeContracts::kNativeContractDefaultWallet;
  return contract;
}

// What an honest node leaves behind: the wallet's contract row, the cost
// model, and a tip whose header states the config hash its configuration
// hashes to.
class RecoveredTipTest : public testing::Test {
protected:
  void SetUp() override
  {
    const ErrnoStatus<Directory> dir = Directory::open_temporary();
    ASSERT_TRUE(dir.has_value());
    db_path_ = dir->get_path();

    DBStatus<std::unique_ptr<Database>> db =
        Database::open(db_path_, Database::estimate_heap_size(kHeapSize), 0);
    ASSERT_TRUE(db.has_value());
    RPCContext ctx(HogDbDAO(RAMDB(std::move(*db))), nullptr);

    ctx.initialize_cost_model(cost_model_);
    ASSERT_TRUE(ctx.get_db().write_cost_model(cost_model_).has_value());

    const Contract wallet = native_default_wallet_contract();
    // Writing the row under a hash the genesis helper does not name would
    // make every case below pass for the wrong reason.
    ASSERT_EQ(sha256(wallet), native_default_wallet_hash().hash);
    const auto encoded = x23::serialize(wallet);
    ASSERT_TRUE(ctx.get_db()
                    .write_contract_code(sha256(wallet),
                                         std::as_bytes(std::span(encoded)))
                    .has_value());

    ctx.set_default_wallet(native_default_wallet_hash());
    ctx.set_network_config(stock_network_config());
    ConsensusValue value{};
    value.proposal_header.block_height = kTipHeight;
    value.proposal_header.config_hash = compute_config_hash(
        make_config_hash_preimage(cost_model_, stock_network_config(),
                                  native_default_wallet_hash()));
    const StorageLongHash state_hash = ctx.get_db().compute_full_db_hash();
    ASSERT_NE(state_hash, StorageLongHash{});
    ASSERT_TRUE(
        ctx.update_dbtip(kBlock, std::move(value), state_hash).has_value());
    ASSERT_TRUE(ctx.get_db().commit().has_value());
  }

  // The recovered database, reopened the way startup does.
  HogDbDAO reopen()
  {
    DBStatus<std::unique_ptr<Database>> db =
        Database::open(db_path_, Database::estimate_heap_size(kHeapSize), 0);
    CHECK(db.has_value());
    return HogDbDAO(RAMDB(std::move(*db)));
  }

  std::string db_path_;
  StorageCostModel cost_model_ = StorageCostModel::default_cost_model();
};

// NOLINTBEGIN(bugprone-unchecked-optional-access)

TEST_F(RecoveredTipTest, AcceptsTheTipItsHeaderCommitsTo)
{
  HogDbDAO db = reopen();
  const HogDbStatus<DbTip> tip = db.read_dbtip();
  ASSERT_TRUE(tip.has_value());
  EXPECT_EQ(tip->default_wallet, native_default_wallet_hash());
  EXPECT_TRUE(StartupValidation::tip_is_servable(db, *tip, cost_model_));
}

// #3926: `<DBTIP` is global metadata, so rewriting the wallet selector there
// leaves the state hash untouched. The config hash is what notices.
TEST_F(RecoveredTipTest, RejectsRewrittenDefaultWallet)
{
  HogDbDAO db = reopen();
  HogDbStatus<DbTip> tip = db.read_dbtip();
  ASSERT_TRUE(tip.has_value());

  const StorageLongHash state_hash = db.compute_full_db_hash();
  DeployedContractHash absent = native_default_wallet_hash();
  absent.hash.fill(0xE7);
  tip->default_wallet = absent;
  ASSERT_TRUE(db.write_dbtip(*tip).has_value());
  ASSERT_TRUE(db.commit().has_value());

  // Unchanged, which is why nothing else catches this.
  EXPECT_EQ(db.compute_full_db_hash(), state_hash);

  const HogDbStatus<DbTip> tampered = db.read_dbtip();
  ASSERT_TRUE(tampered.has_value());
  ASSERT_EQ(tampered->default_wallet, absent);
  EXPECT_FALSE(StartupValidation::tip_is_servable(db, *tampered, cost_model_));
}

// `<COSTMODEL` is the other unhashed row; startup reads the model it passes
// here straight off it, so a rewritten row arrives as a differing model.
TEST_F(RecoveredTipTest, RejectsRewrittenCostModel)
{
  HogDbDAO db = reopen();
  const HogDbStatus<DbTip> tip = db.read_dbtip();
  ASSERT_TRUE(tip.has_value());

  StorageCostModelXdr xdr = cost_model_.to_xdr();
  xdr.balance_cost++;
  EXPECT_FALSE(
      StartupValidation::tip_is_servable(db, *tip, StorageCostModel(xdr)));
}

// A wallet the header commits to but whose row is absent. Nothing before
// execution reads it, so otherwise the first root call finds out.
TEST_F(RecoveredTipTest, RejectsDefaultWalletWithNoContractRow)
{
  HogDbDAO db = reopen();
  HogDbStatus<DbTip> tip = db.read_dbtip();
  ASSERT_TRUE(tip.has_value());
  ASSERT_GT(tip->value.proposal_header.block_height, 0);

  DeployedContractHash absent = native_default_wallet_hash();
  absent.hash.fill(0xE7);
  tip->default_wallet = absent;
  tip->value.proposal_header.config_hash = compute_config_hash(
      make_config_hash_preimage(cost_model_, tip->network_config, absent));
  ASSERT_FALSE(db.read_contract_code(absent.hash).has_value());

  EXPECT_FALSE(StartupValidation::tip_is_servable(db, *tip, cost_model_));
}

// The same tip at height 0 is served: the genesis ceremony attaches that code
// in the height-1 block, so the row is legitimately absent until then.
TEST_F(RecoveredTipTest, AcceptsDefaultWalletTheCeremonyHasYetToDeploy)
{
  HogDbDAO db = reopen();
  HogDbStatus<DbTip> tip = db.read_dbtip();
  ASSERT_TRUE(tip.has_value());

  DeployedContractHash ceremony_deployed = native_default_wallet_hash();
  ceremony_deployed.hash.fill(0xE7);
  tip->value.proposal_header.block_height = 0;
  tip->default_wallet = ceremony_deployed;
  tip->value.proposal_header.config_hash = compute_config_hash(
      make_config_hash_preimage(cost_model_, tip->network_config,
                                ceremony_deployed));
  ASSERT_FALSE(db.read_contract_code(ceremony_deployed.hash).has_value());

  EXPECT_TRUE(StartupValidation::tip_is_servable(db, *tip, cost_model_));
}

// Refused before the hash is computed, rather than aborting inside
// make_config_hash_preimage.
TEST_F(RecoveredTipTest, RejectsNonCanonicalNetworkConfig)
{
  HogDbDAO db = reopen();
  HogDbStatus<DbTip> tip = db.read_dbtip();
  ASSERT_TRUE(tip.has_value());

  NetworkConfig gapped;
  gapped.v(0);
  gapped.v0().params.push_back(NetworkConfigParams::make_param(
      ConfigParamID::kConfigParamMaxCrossInvokeDepth, 256));
  ASSERT_FALSE(NetworkConfigParams::is_canonical(gapped.v0()));
  tip->network_config = gapped;

  EXPECT_FALSE(StartupValidation::tip_is_servable(db, *tip, cost_model_));
}

TEST_F(RecoveredTipTest, RejectsPersistedNegativeHeights)
{
  HogDbDAO db = reopen();
  HogDbStatus<DbTip> tip = db.read_dbtip();
  ASSERT_TRUE(tip.has_value());

  for (BlockHeight height : {BlockHeight{-1}, BlockHeight{-2},
                             std::numeric_limits<BlockHeight>::min()}) {
    tip->value.proposal_header.block_height = height;
    ASSERT_TRUE(db.write_dbtip(*tip).has_value());
    ASSERT_TRUE(db.commit().has_value());
    const auto recovered = db.read_dbtip();
    ASSERT_TRUE(recovered.has_value());
    EXPECT_FALSE(
        StartupValidation::tip_is_servable(db, *recovered, cost_model_));
  }
}

// The successor height would overflow the signed BlockHeight, so the maximum
// committed height is refused even though it is nonnegative (issue #4291).
TEST_F(RecoveredTipTest, RejectsPersistedMaximumHeight)
{
  HogDbDAO db = reopen();
  HogDbStatus<DbTip> tip = db.read_dbtip();
  ASSERT_TRUE(tip.has_value());

  tip->value.proposal_header.block_height =
      std::numeric_limits<BlockHeight>::max();
  ASSERT_TRUE(db.write_dbtip(*tip).has_value());
  ASSERT_TRUE(db.commit().has_value());
  const auto recovered = db.read_dbtip();
  ASSERT_TRUE(recovered.has_value());
  EXPECT_FALSE(StartupValidation::tip_is_servable(db, *recovered, cost_model_));
}

// A tampered default-wallet type must not survive the recovery round-trip
// into a servable tip (#4293).
TEST_F(RecoveredTipTest, RejectsDefaultWalletWithUnregisteredContractType)
{
  HogDbDAO db = reopen();
  HogDbStatus<DbTip> tip = db.read_dbtip();
  ASSERT_TRUE(tip.has_value());

  tip->default_wallet.contract_type =
      static_cast<ContractType>(std::numeric_limits<uint32_t>::max());
  // Recompute so the config-hash guard passes and the type check is what bites.
  tip->value.proposal_header.config_hash = compute_config_hash(
      make_config_hash_preimage(cost_model_, tip->network_config,
                                tip->default_wallet));
  ASSERT_TRUE(db.write_dbtip(*tip).has_value());
  ASSERT_TRUE(db.commit().has_value());

  HogDbDAO recovered_db = reopen();
  const HogDbStatus<DbTip> recovered = recovered_db.read_dbtip();
  ASSERT_TRUE(recovered.has_value());
  EXPECT_FALSE(StartupValidation::tip_is_servable(recovered_db, *recovered,
                                                  cost_model_));
}

// A recovered tip can keep the native wallet's code hash while rewriting its
// claimed type to the registered kRv32, recomputing the config hash so the
// earlier guards pass. Startup must bind the claimed type to the decoded code
// row and refuse the mismatch before a fallback reaches the rv32/native sink
// (#4306).
TEST_F(RecoveredTipTest, RejectsDefaultWalletTypeMismatchingContractRow)
{
  HogDbDAO db = reopen();
  HogDbStatus<DbTip> tip = db.read_dbtip();
  ASSERT_TRUE(tip.has_value());

  tip->default_wallet.contract_type = ContractType::kRv32;
  // Recompute so the config-hash guard passes and the type check is what bites.
  tip->value.proposal_header.config_hash = compute_config_hash(
      make_config_hash_preimage(cost_model_, tip->network_config,
                                tip->default_wallet));
  ASSERT_TRUE(db.write_dbtip(*tip).has_value());
  ASSERT_TRUE(db.commit().has_value());

  HogDbDAO recovered_db = reopen();
  const HogDbStatus<DbTip> recovered = recovered_db.read_dbtip();
  ASSERT_TRUE(recovered.has_value());
  // The retained code row still decodes as the native wallet.
  const HogDbStatus<OwningSpan<const std::byte>> code =
      recovered_db.read_contract_code(recovered->default_wallet.hash);
  ASSERT_TRUE(code.has_value());
  EXPECT_FALSE(StartupValidation::tip_is_servable(recovered_db, *recovered,
                                                  cost_model_));
}

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace

} // namespace hogd
