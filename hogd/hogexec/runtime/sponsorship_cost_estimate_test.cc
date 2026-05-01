#include <gtest/gtest.h>

#include <cassert>
#include <utility>

#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogexec/runtime/item/item_view.h"
#include "hogd/hogexec/runtime/sponsorship_cost_estimate.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "storage_item.hh"

namespace hogd {

class SponsorshipCostEstimateTest : public ::testing::Test {
protected:
  StorageCostModel cost_model_ = StorageCostModel::default_cost_model();
  // default_cost_model() values:
  //   base_item_cost              = 12000
  //   bytestring_cost_per_byte    = 10
  //   base_deploy_cost            = 12000
  //   deploy_cost_per_byte        = 10

  static constexpr StorageDeltaFlagSet kExclusive =
      std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagExclusiveLock);
  static constexpr StorageDeltaFlagSet kNonExclusive =
      std::to_underlying(StorageDeltaFlags::None);

  // Helper: create a Bytestring of a given length (contents don't matter).
  static Bytestring make_bytestring(size_t len)
  {
    Bytestring bs;
    bs.resize_uninitialized(len);
    return bs;
  }

  // Helper: create an ItemView containing a BytestringItem of a given length.
  static ItemView make_bytestring_view(size_t len, StorageDeltaFlagSet flags)
  {
    ItemView view;
    assert(view.set(BytestringItem{make_bytestring(len)}, flags));
    return view;
  }

  // Helper: create an ItemView containing an NNIntItem.
  static ItemView make_nnint_view(int128_t base_value,
                                  StorageDeltaFlagSet flags)
  {
    ItemView view;
    assert(view.set(NNIntItem{.base_value_ = to_xdr(base_value),
                              .add_value_ = to_xdr(int128_t{0})},
                    flags));
    return view;
  }

  // Helper: create an ItemView containing a DeletedItem.
  static ItemView make_deleted_view(StorageDeltaFlagSet flags)
  {
    ItemView view;
    assert(view.set(DeletedItem{}, flags));
    return view;
  }
};

// ---------------------------------------------------------------------------
// Bytestring writes to new keys (UnmodifiedItem, not loaded)
// ---------------------------------------------------------------------------

TEST_F(SponsorshipCostEstimateTest, BytestringNewKeyExclusive)
{
  ItemView view;
  constexpr size_t kLen = 100;
  StorageWriteInfo info{
      .type = StorageWriteInfo::WriteType::kBytestring,
      .bytestring_len = kLen,
      .flags = kExclusive,
  };
  // new_cost = 12000 + 100*10 = 13000, prior_cost = 0
  int128_t result = estimate_marginal_storage_cost(view, info, cost_model_);
  EXPECT_EQ(result, int128_t{13000});
}

TEST_F(SponsorshipCostEstimateTest, BytestringNewKeyNonExclusive)
{
  ItemView view;
  constexpr size_t kLen = 100;
  StorageWriteInfo info{
      .type = StorageWriteInfo::WriteType::kBytestring,
      .bytestring_len = kLen,
      .flags = kNonExclusive,
  };
  // new_cost = 13000, prior_cost = 0 => max(13000, 0) = 13000
  int128_t result = estimate_marginal_storage_cost(view, info, cost_model_);
  EXPECT_EQ(result, int128_t{13000});
}

// ---------------------------------------------------------------------------
// Bytestring overwrite same size (exclusive)
// ---------------------------------------------------------------------------

TEST_F(SponsorshipCostEstimateTest, BytestringOverwriteSameSizeExclusive)
{
  constexpr size_t kLen = 50;
  auto view = make_bytestring_view(kLen, kExclusive);

  StorageWriteInfo info{
      .type = StorageWriteInfo::WriteType::kBytestring,
      .bytestring_len = kLen,
      .flags = kExclusive,
  };
  // new_cost = 12000 + 50*10 = 12500, prior_cost = 12500 => 0
  int128_t result = estimate_marginal_storage_cost(view, info, cost_model_);
  EXPECT_EQ(result, int128_t{0});
}

// ---------------------------------------------------------------------------
// Bytestring overwrite with larger value (exclusive)
// ---------------------------------------------------------------------------

TEST_F(SponsorshipCostEstimateTest, BytestringOverwriteLargerExclusive)
{
  constexpr size_t kOldLen = 50;
  constexpr size_t kNewLen = 200;
  auto view = make_bytestring_view(kOldLen, kExclusive);

  StorageWriteInfo info{
      .type = StorageWriteInfo::WriteType::kBytestring,
      .bytestring_len = kNewLen,
      .flags = kExclusive,
  };
  // new_cost = 12000 + 200*10 = 14000
  // prior_cost = 12000 + 50*10 = 12500
  // delta = 14000 - 12500 = 1500
  int128_t result = estimate_marginal_storage_cost(view, info, cost_model_);
  EXPECT_EQ(result, int128_t{1500});
}

// ---------------------------------------------------------------------------
// Bytestring overwrite with smaller value (exclusive -> can be negative)
// ---------------------------------------------------------------------------

TEST_F(SponsorshipCostEstimateTest, BytestringOverwriteSmallerExclusive)
{
  constexpr size_t kOldLen = 200;
  constexpr size_t kNewLen = 50;
  auto view = make_bytestring_view(kOldLen, kExclusive);

  StorageWriteInfo info{
      .type = StorageWriteInfo::WriteType::kBytestring,
      .bytestring_len = kNewLen,
      .flags = kExclusive,
  };
  // new_cost = 12000 + 50*10 = 12500
  // prior_cost = 12000 + 200*10 = 14000
  // delta = 12500 - 14000 = -1500
  int128_t result = estimate_marginal_storage_cost(view, info, cost_model_);
  EXPECT_EQ(result, int128_t{-1500});
}

// ---------------------------------------------------------------------------
// Bytestring overwrite same size (non-exclusive)
// ---------------------------------------------------------------------------

TEST_F(SponsorshipCostEstimateTest, BytestringOverwriteSameSizeNonExclusive)
{
  constexpr size_t kLen = 50;
  auto view = make_bytestring_view(kLen, kNonExclusive);

  StorageWriteInfo info{
      .type = StorageWriteInfo::WriteType::kBytestring,
      .bytestring_len = kLen,
      .flags = kNonExclusive,
  };
  // new_cost = prior_cost => delta = 0, max(0, 0) = 0
  int128_t result = estimate_marginal_storage_cost(view, info, cost_model_);
  EXPECT_EQ(result, int128_t{0});
}

// ---------------------------------------------------------------------------
// Bytestring overwrite larger (non-exclusive) -> positive, same as exclusive
// ---------------------------------------------------------------------------

TEST_F(SponsorshipCostEstimateTest, BytestringOverwriteLargerNonExclusive)
{
  constexpr size_t kOldLen = 50;
  constexpr size_t kNewLen = 200;
  auto view = make_bytestring_view(kOldLen, kNonExclusive);

  StorageWriteInfo info{
      .type = StorageWriteInfo::WriteType::kBytestring,
      .bytestring_len = kNewLen,
      .flags = kNonExclusive,
  };
  // new_cost - prior_cost = 1500 > 0, so max(1500, 0) = 1500
  int128_t result = estimate_marginal_storage_cost(view, info, cost_model_);
  EXPECT_EQ(result, int128_t{1500});
}

// ---------------------------------------------------------------------------
// Bytestring overwrite smaller (non-exclusive) -> floored at 0
// ---------------------------------------------------------------------------

TEST_F(SponsorshipCostEstimateTest, BytestringOverwriteSmallerNonExclusive)
{
  constexpr size_t kOldLen = 200;
  constexpr size_t kNewLen = 50;
  auto view = make_bytestring_view(kOldLen, kNonExclusive);

  StorageWriteInfo info{
      .type = StorageWriteInfo::WriteType::kBytestring,
      .bytestring_len = kNewLen,
      .flags = kNonExclusive,
  };
  // new_cost - prior_cost = -1500, max(-1500, 0) = 0
  int128_t result = estimate_marginal_storage_cost(view, info, cost_model_);
  EXPECT_EQ(result, int128_t{0});
}

// ---------------------------------------------------------------------------
// NNInt writes
// ---------------------------------------------------------------------------

TEST_F(SponsorshipCostEstimateTest, NNIntNewKey)
{
  ItemView view;
  StorageWriteInfo info{
      .type = StorageWriteInfo::WriteType::kNNInt,
      .flags = kNonExclusive,
  };
  // new_cost = base_item_cost = 12000, prior_cost = 0
  int128_t result = estimate_marginal_storage_cost(view, info, cost_model_);
  EXPECT_EQ(result, int128_t{12000});
}

TEST_F(SponsorshipCostEstimateTest, NNIntExistingKey)
{
  auto view = make_nnint_view(100, kNonExclusive);

  StorageWriteInfo info{
      .type = StorageWriteInfo::WriteType::kNNInt,
      .flags = kNonExclusive,
  };
  // new_cost = 12000, prior_cost = 12000 => delta = 0
  int128_t result = estimate_marginal_storage_cost(view, info, cost_model_);
  EXPECT_EQ(result, int128_t{0});
}

// ---------------------------------------------------------------------------
// Delete operations
// ---------------------------------------------------------------------------

TEST_F(SponsorshipCostEstimateTest, DeleteExistingExclusive)
{
  constexpr size_t kLen = 80;
  auto view = make_bytestring_view(kLen, kExclusive);

  StorageWriteInfo info{
      .type = StorageWriteInfo::WriteType::kDelete,
      .flags = kExclusive,
  };
  // new_cost = 0, prior_cost = 12000 + 80*10 = 12800
  // delta = 0 - 12800 = -12800
  int128_t result = estimate_marginal_storage_cost(view, info, cost_model_);
  EXPECT_EQ(result, int128_t{-12800});
}

TEST_F(SponsorshipCostEstimateTest, DeleteExistingNonExclusive)
{
  constexpr size_t kLen = 80;
  auto view = make_bytestring_view(kLen, kNonExclusive);

  StorageWriteInfo info{
      .type = StorageWriteInfo::WriteType::kDelete,
      .flags = kNonExclusive,
  };
  // new_cost = 0, prior_cost = 12800 => delta = -12800, max(-12800, 0) = 0
  int128_t result = estimate_marginal_storage_cost(view, info, cost_model_);
  EXPECT_EQ(result, int128_t{0});
}

TEST_F(SponsorshipCostEstimateTest, DeleteNonexistent)
{
  ItemView view;
  StorageWriteInfo info{
      .type = StorageWriteInfo::WriteType::kDelete,
      .flags = kExclusive,
  };
  // new_cost = 0, prior_cost = 0 => delta = 0
  int128_t result = estimate_marginal_storage_cost(view, info, cost_model_);
  EXPECT_EQ(result, int128_t{0});
}

// ---------------------------------------------------------------------------
// Deploy contract
// ---------------------------------------------------------------------------

TEST_F(SponsorshipCostEstimateTest, DeployContractNew)
{
  constexpr uint32_t kContractSize = 500;
  ItemView view;
  StorageWriteInfo info{
      .type = StorageWriteInfo::WriteType::kDeployContract,
      .contract_size = kContractSize,
      .flags = kNonExclusive,
  };
  // new_cost = base_deploy + size * per_byte_deploy = 12000 + 500*10 = 17000
  // prior_cost = 0
  int128_t result = estimate_marginal_storage_cost(view, info, cost_model_);
  EXPECT_EQ(result, int128_t{17000});
}

// ---------------------------------------------------------------------------
// Blind write: UnmodifiedItem not loaded overestimates
// ---------------------------------------------------------------------------

TEST_F(SponsorshipCostEstimateTest, BlindWriteOverestimates)
{
  // An UnmodifiedItem that is NOT loaded (default-constructed). The function
  // should assume the key doesn't exist and charge full cost.
  ItemView view;
  constexpr size_t kLen = 100;
  StorageWriteInfo info{
      .type = StorageWriteInfo::WriteType::kBytestring,
      .bytestring_len = kLen,
      .flags = kNonExclusive,
  };
  // Even if the key actually exists in the DB, the blind write estimates
  // prior_cost = 0, so new_cost = 12000 + 100*10 = 13000
  int128_t result = estimate_marginal_storage_cost(view, info, cost_model_);
  EXPECT_EQ(result, int128_t{13000});
}

// ---------------------------------------------------------------------------
// Write to a DeletedItem (prior_cost = 0)
// ---------------------------------------------------------------------------

TEST_F(SponsorshipCostEstimateTest, DeletedItemThenWrite)
{
  auto view = make_deleted_view(kExclusive);

  constexpr size_t kLen = 100;
  StorageWriteInfo info{
      .type = StorageWriteInfo::WriteType::kBytestring,
      .bytestring_len = kLen,
      .flags = kExclusive,
  };
  // new_cost = 12000 + 100*10 = 13000, prior_cost(DeletedItem) = 0
  int128_t result = estimate_marginal_storage_cost(view, info, cost_model_);
  EXPECT_EQ(result, int128_t{13000});
}

// ---------------------------------------------------------------------------
// Edge case: zero-length bytestring
// ---------------------------------------------------------------------------

TEST_F(SponsorshipCostEstimateTest, BytestringZeroLength)
{
  ItemView view;
  StorageWriteInfo info{
      .type = StorageWriteInfo::WriteType::kBytestring,
      .bytestring_len = 0,
      .flags = kNonExclusive,
  };
  // new_cost = 12000 + 0*10 = 12000 (base cost only)
  int128_t result = estimate_marginal_storage_cost(view, info, cost_model_);
  EXPECT_EQ(result, int128_t{12000});
}

// ---------------------------------------------------------------------------
// Edge case: zero-size contract deploy
// ---------------------------------------------------------------------------

TEST_F(SponsorshipCostEstimateTest, DeployContractZeroSize)
{
  ItemView view;
  StorageWriteInfo info{
      .type = StorageWriteInfo::WriteType::kDeployContract,
      .contract_size = 0,
      .flags = kNonExclusive,
  };
  // new_cost = base_deploy + 0*10 = 12000
  int128_t result = estimate_marginal_storage_cost(view, info, cost_model_);
  EXPECT_EQ(result, int128_t{12000});
}

// ---------------------------------------------------------------------------
// NNInt overwrite existing NNInt with exclusive lock
// ---------------------------------------------------------------------------

TEST_F(SponsorshipCostEstimateTest, NNIntExistingKeyExclusive)
{
  auto view = make_nnint_view(999, kExclusive);

  StorageWriteInfo info{
      .type = StorageWriteInfo::WriteType::kNNInt,
      .flags = kExclusive,
  };
  // new_cost = 12000, prior_cost = 12000 => 0 (no change in cost)
  int128_t result = estimate_marginal_storage_cost(view, info, cost_model_);
  EXPECT_EQ(result, int128_t{0});
}

// ---------------------------------------------------------------------------
// Delete an NNInt item with exclusive lock
// ---------------------------------------------------------------------------

TEST_F(SponsorshipCostEstimateTest, DeleteNNIntExclusive)
{
  auto view = make_nnint_view(42, kExclusive);

  StorageWriteInfo info{
      .type = StorageWriteInfo::WriteType::kDelete,
      .flags = kExclusive,
  };
  // new_cost = 0, prior_cost = 12000 => delta = -12000
  int128_t result = estimate_marginal_storage_cost(view, info, cost_model_);
  EXPECT_EQ(result, int128_t{-12000});
}

} // namespace hogd
