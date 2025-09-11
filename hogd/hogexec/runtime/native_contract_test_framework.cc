#include "hogd/hogexec/runtime/native_contract_test_framework.h"

#include <cassert>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <variant>

#include "db_coord.hh"
#include "genesis.hh"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogexec/runtime/storage_snapshot_cache.h"
#include "hogd/storage_item/storage_item_wrapper.h"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "types.hh"
#include "x23/rpc_common.h"
#include "x23/serialize.h"

namespace hogd {

// TODO(geoff): This should be refactored; right now initialize() technically
// opens a block before the genesis initialization.
void
NativeContractTestFramework::advance_block(BlockExecutionMode mode)
{
  {
    auto r = coord_wrapper_->client().dbcoord_close_snapshot(block_number_)();
    if (r.result() != DbResult::DbResultOK) {
      throw std::runtime_error("advance_block: dbcoord_close_snapshot failed");
    }
  }
  {
    auto r = coord_wrapper_->client().dbcoord_commit_snapshot(block_number_)();
    if (r.result() != DbResult::DbResultOK) {
      throw std::runtime_error("advance_block: dbcoord_commit_snapshot failed");
    }
  }

  {
    auto r = coord_wrapper_->client().dbcoord_create_snapshot(block_number_,
                                                              block_number_ + 1,
                                                              mode)();
    if (r != DbResult::DbResultOK) {
      throw std::runtime_error("advance_block: dbcoord_create_snapshot failed");
    }
  }
  block_number_++;

  storage_snapshot_cache_ =
      std::make_unique<StorageSnapshotCache>(block_number_, *db_proxy_);
}

void
NativeContractTestFramework::enable_logging()
{
  previous_logger_ =
      swap_logger(std::make_unique<ConsoleLogger>(LogLevel::kTrace));
  x23::RpcLogger::default_logfd = 1;
}

void
NativeContractTestFramework::setup_grant_storage_token(
    ContractAddress const &address, int128_t amount)
{
  assert(amount >= 0);
  assert(amount <= int128_t{kHogDivisibility} * int128_t{kTotalHogSupply});

  std::visit(
      [&](auto &db_wrapper) {
        db_wrapper.grant_storage_token(address, amount);
      },
      db_wrapper_);
}

void
NativeContractTestFramework::write_item(
    AddressAndKey const &addrkey,
    std::shared_ptr<const StorageItemWrapper> item)
{
  std::visit(
      [&](auto &db_wrapper) {
        db_wrapper.write_item(addrkey, std::move(item));
      },
      db_wrapper_);
}
void
NativeContractTestFramework::write_contract(
    std::shared_ptr<const Contract> const &contract)
{
  std::visit(
      [&](auto &db_wrapper) {
        db_wrapper.write_contract(sha256(*contract), contract);
      },
      db_wrapper_);
}
void
NativeContractTestFramework::write_deployment(ContractAddress const &addr,
                                              SHA256Hash const &deployment)
{
  std::visit(
      [&](auto &db_wrapper) { db_wrapper.write_deployment(addr, deployment); },
      db_wrapper_);
}

void
NativeContractTestFramework::run_genesis_initialization(
    GenesisBlock const &genesis_block)
{
  auto r = coord_wrapper_->client().dbcoord_load_genesis_block(genesis_block)();
  if (r != DbResult::DbResultOK) {
    throw std::runtime_error(
        "run_genesis_initialization: dbcoord_load_genesis_block failed");
  }
}

void
HogdbWrapper::grant_storage_token(ContractAddress const &address,
                                  int128_t amount)
{
  auto ret = db_.write_address_balance(address, amount);
  if (!ret) {
    throw std::runtime_error(
        "grant_storage_token: write_address_balance failed");
  }
}

void
HogdbWrapper::write_item(AddressAndKey const &addrkey,
                         std::shared_ptr<const StorageItemWrapper> const &item)
{
  switch (item->type()) {
  case StorageItemType::kItemTypeBytestring:
  {
    auto ret = db_.write_bytes(make_item_key(addrkey),
                               std::as_bytes(std::span(item->bytestring())));
    if (!ret) {
      throw std::runtime_error("write_item: write_bytes failed");
    }
    return;
  }
  case StorageItemType::kItemTypeNonnegativeInt:
  {
    auto ret = db_.write_nnint128(make_item_key(addrkey), item->nnint());
    if (!ret) {
      throw std::runtime_error("write_item: write_wasm_code failed");
    }
    return;
  }
  case StorageItemType::kItemTypeDeleted:
  {
    auto ret = db_.remove_item(make_item_key(addrkey));
    if (!ret) {
      throw std::runtime_error("write_item: remove_item failed");
    }
    return;
  }
  default:
    throw std::runtime_error(
        "write_item on HogdbWrapper: unsupported item type");
  }
}

void
HogdbWrapper::write_contract(ContractAddress const &addr,
                             std::shared_ptr<const Contract> const &contract)
{
  assert(contract);
  auto bytes = x23::serialize(*contract);
  auto ret = db_.write_wasm_code(addr, std::as_bytes(std::span(bytes)));
  if (!ret) {
    throw std::runtime_error("write_contract: write_contract failed");
  }
}

void
HogdbWrapper::write_deployment(ContractAddress const &addr,
                               SHA256Hash const &deployment)
{
  auto ret = db_.write_address_wasm_hash(addr, deployment);
  if (!ret) {
    throw std::runtime_error("write_deployment: write_deployment failed");
  }
}

} // namespace hogd
