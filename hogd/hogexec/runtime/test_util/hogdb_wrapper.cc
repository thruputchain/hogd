#include "hogd/hogexec/runtime/test_util/hogdb_wrapper.h"

#include <cassert>
#include <memory>
#include <span>
#include <utility>

#include "contract.hh"
#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/types.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/ramdb/db.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "sched_db.hh"
#include "storage_item.hh"
#include "types.hh"
#include "x23/rpc.h"
#include "x23/rpc_common.h"
#include "x23/serialize.h"
#include "x23/sockio.h"

namespace hogd {

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
      ctx_.get_db().write_wasm_code(addr, std::as_bytes(std::span(bytes)));
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

HogDBDAO
HogdbWrapper::open_db()
{
  Directory dir = Directory::open_temporary().value();
  constexpr static size_t kDefaultHeapSize = 10_MiB;
  auto db = Database::open(dir.get_path(),
                           Database::estimate_heap_size(kDefaultHeapSize))
                .value();
  return HogDBDAO(RAMDB(std::move(db)));
}

std::unique_ptr<SchedDb1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>>
HogdbWrapper::make_sched_db_client()
{
  auto sched_db_port = test_sched_db_server_.start_server(/*port=*/0);
  return std::make_unique<
      SchedDb1::client<x23::RpcStream<x23::RpcLogger>::ClientRef>>(
      make_rpc_client<SchedDb1>(x23::HostPort{"localhost", sched_db_port},
                                false)());
}

} // namespace hogd
