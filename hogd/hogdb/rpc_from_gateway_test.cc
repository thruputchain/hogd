// Smoke tests for HogDBGatewayRpc.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

// xdr types
#include "db_gateway.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"

#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/rpc_context.h"
#include "hogd/hogdb/rpc_from_gateway.h"
#include "hogd/ramdb/db.h"
#include "hogd/ramdb/db_status.h"

namespace hogd {

namespace {

constexpr std::size_t kTestRamBytes = 64UL * 1024 * 1024;

HogDBDAO
make_dao()
{
  ErrnoStatus<Directory> dir = Directory::open_temporary();
  CHECK(dir.has_value());
  DBStatus<std::unique_ptr<Database>> db =
      Database::open(dir->get_path(),
                     Database::estimate_heap_size(kTestRamBytes), 0);
  CHECK(db.has_value());
  return HogDBDAO(RAMDB(std::move(*db)));
}

// Builds the address+key pair that locates a HOG balance.  The
// FreeNativeTokenBalance key has no extra payload — the address alone
// disambiguates.
AddressAndKey
make_balance_query(ContractAddress const &addr)
{
  AddressAndKey query;
  query.address = addr;
  query.key.key_type(StorageKeyType::FreeNativeTokenBalance);
  return query;
}

} // namespace

TEST(HogDBGatewayRpcTest, GetItemsReturnsSeededBalance)
{
  HogDBDAO dao = make_dao();

  // Non-zero address so it can't collide with the zero address used by
  // the missing-balance test below.
  ContractAddress addr{};
  for (std::size_t i = 0; i < addr.size(); ++i)
    addr[i] = static_cast<std::uint8_t>(i + 1);
  ASSERT_TRUE(dao.write_address_balance(addr, int128_t{12345}).has_value());

  RPCContext ctx(std::move(dao), nullptr);

  AddressAndKeyBatch batch;
  batch.push_back(make_balance_query(addr));

  HogDBGatewayRpc gateway(ctx);
  DbGatewayResult result = gateway.dbgateway_get_items(batch);

  ASSERT_EQ(result.error(), RPCError::kRPCNone);
  ASSERT_EQ(result.payload().items.size(), 1U);
  ASSERT_EQ(result.payload().items[0].error(), RPCError::kRPCNone);
  ASSERT_EQ(result.payload().items[0].item().type(),
            StorageItemType::kItemTypeNonnegativeInt);
  EXPECT_EQ(from_xdr(result.payload().items[0].item().nnint()),
            int128_t{12345});
}

// Querying an address that was never written returns per-item
// kRPCNotFound while the outer batch result stays kRPCNone.
TEST(HogDBGatewayRpcTest, GetItemsMissingBalanceIsNotFound)
{
  HogDBDAO dao = make_dao();
  RPCContext ctx(std::move(dao), nullptr);

  // Zero address: make_dao() seeds nothing, so this address has no
  // balance row.
  ContractAddress addr{};
  AddressAndKeyBatch batch;
  batch.push_back(make_balance_query(addr));

  HogDBGatewayRpc gateway(ctx);
  DbGatewayResult result = gateway.dbgateway_get_items(batch);

  ASSERT_EQ(result.error(), RPCError::kRPCNone);
  ASSERT_EQ(result.payload().items.size(), 1U);
  EXPECT_EQ(result.payload().items[0].error(), RPCError::kRPCNotFound);
}

} // namespace hogd
