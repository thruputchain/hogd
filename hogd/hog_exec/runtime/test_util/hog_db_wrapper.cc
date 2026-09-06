#include "hogd/hog_exec/runtime/test_util/hog_db_wrapper.h"

#include <cassert>
#include <memory>
#include <span>
#include <utility>

#include "consensus.hh"
#include "contract.hh"
#include "db_sched.hh"
#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_db/hog_types.h"
#include "hogd/ramdb/db.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "network_config.hh"
#include "rpc_types.hh"
#include "sched_db.hh"
#include "storage_item.hh"
#include "types.hh"
#include "x23/rpc.h"
#include "x23/rpc_common.h"
#include "x23/serialize.h"
#include "x23/sockio.h"

namespace hogd {

void
HogdbWrapper::prepare_block(BlockID block_id, BlockHeight tip_block_height,
                            NetworkConfig network_config,
                            BlockExecutionMode mode)
{
  const BlockID parent_id = block_id - 1;

  ConsensusValue value{};
  value.proposal_header.block_height = tip_block_height;
  value.proposal_header.txs_hash = sha256(block_id);
  ctx_.set_tip(DbTip{.id = parent_id,
                     .storage_long_hash = {},
                     .value = value,
                     .default_wallet = native_default_wallet_hash(),
                     .network_config = std::move(network_config)});

  if (!coord_client_.has_value()) {
    coord_client_.emplace(make_coord_client());
  }
  const DbSchedResult r =
      coord_client_->dbsched_create_snapshot(parent_id, block_id, mode);
  CHECK_EQ(r, DbSchedResult::DbSchedResultOK);
}

HogdbWrapper::coord_client_t
HogdbWrapper::make_coord_client()
{
  return make_srpc_client<DbSched1>(x23::HostPort{"localhost", coord_port()},
                                    false);
}

void
HogdbWrapper::grant_storage_token(ContractAddress const &address,
                                  int128_t amount)
{
  // Native-token balances are an always-nonnegative invariant; a negative
  // grant would persist a row that later balance reads assert on.
  CHECK_GE(amount, 0);
  auto ret = ctx_.get_db().write_address_balance(address, amount);
  CHECK(ret.has_value());
}

void
HogdbWrapper::write_item(AddressAndKey const &addrkey,
                         std::shared_ptr<const StorageItemWrapper> const &item)
{
  switch (item->type()) {
  case StorageItemType::kItemTypeBytestring:
  {
    auto ret =
        ctx_.get_db().write_bytes(make_item_key(addrkey),
                                  std::as_bytes(std::span(item->bytestring())));
    CHECK(ret.has_value());
    return;
  }
  case StorageItemType::kItemTypeNonnegativeInt:
  {
    auto ret =
        ctx_.get_db().write_nnint128(make_item_key(addrkey), item->nnint());
    CHECK(ret.has_value());
    return;
  }
  case StorageItemType::kItemTypeDeleted:
  {
    info("write_item: removing item");
    auto ret = ctx_.get_db().remove_item(make_item_key(addrkey));
    CHECK(ret.has_value());
    return;
  }
  default:
    panic("write_item on HogdbWrapper: unsupported item type");
  }
}

void
HogdbWrapper::write_contract(ContractAddress const &addr,
                             std::shared_ptr<const Contract> const &contract)
{
  assert(contract);
  auto bytes = x23::serialize(*contract);
  auto ret =
      ctx_.get_db().write_contract_code(addr, std::as_bytes(std::span(bytes)));
  CHECK(ret.has_value());
}

void
HogdbWrapper::write_deployment(ContractAddress const &addr,
                               DeployedContractHash const &deployment,
                               StorageCostModel const * /*unused*/)
{
  auto ret = ctx_.get_db().write_address_metadata(addr, deployment);
  CHECK(ret.has_value());
}

HogDbDAO
HogdbWrapper::open_db(size_t heap_bytes)
{
  Directory dir = Directory::open_temporary().value();
  auto db =
      Database::open(dir.get_path(), Database::estimate_heap_size(heap_bytes))
          .value();
  return HogDbDAO(RAMDB(std::move(db)));
}

std::unique_ptr<SchedDb1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>>
HogdbWrapper::make_sched_db_client()
{
  auto sched_db_port = test_sched_db_server_.start_server(/*port=*/0);
  return std::make_unique<
      SchedDb1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>>(
      make_rpc_client<SchedDb1>(x23::HostPort{"localhost", sched_db_port},
                                false)()
          .value());
}

} // namespace hogd
