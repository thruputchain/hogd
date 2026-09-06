#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <utility>

#include "consensus.hh"
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
#include "hogd/hog_mempool/config_hash.h"
#include "hogd/ramdb/db.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "network_config.hh"
#include "storage_item.hh"
#include "types.hh"

namespace hogd {

// NOLINTBEGIN(bugprone-unchecked-optional-access)

// A genesis states max_bytestring_len, and the wire type only admits values up
// to kBytestringCeilingLen. Loading is the one place that bound is checked, so
// a chain naming more has to be refused here rather than aborting later on a
// write the syscall let through.
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
    set_config_param(config, ConfigParamID::kConfigParamMaxBytestringLen,
                     max_bytestring_len);

    const GenesisConfigurationBuilder builder;
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

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
