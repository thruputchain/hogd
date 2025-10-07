#include "hogd/hogdb/hogdb_dao.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>

// xdr types
#include "account.hh"
#include "db_sched.hh"
#include "hogd/base/log.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "storage_cost.hh"
#include "types.hh"

#include "hogd/base/bits.h"
#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/merkle.h"
#include "hogd/hogdb/status.h"
#include "hogd/ramdb/db.h"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

namespace {

constexpr std::string kGlobalTipKey = ">DBTIP";
constexpr std::string kGlobalCostModelKey = ">COSTMODEL";

// Warning: you may have to update this if you make changes to Prefix.
constexpr bool
is_state_item(std::span<const std::byte> key)
{
  if (key.empty()) {
    return false;
  }

  // skip global state.
  if (static_cast<char>(key[0]) == '>')
    return false;

  auto prefix = static_cast<Prefix>(key[0]);

  switch (prefix) {
  case Prefix::kHogAge:
    return false;
  case Prefix::kBalance:
  case Prefix::kAccountAuthData:
  case Prefix::kAddressMetadata:
  case Prefix::kContractDataKey:
  case Prefix::kWasmCodeRefcount:
  case Prefix::kWasmCode:
  case Prefix::kReplayCache:
    return true;
  default:
    error("unknown prefix: {}", std::to_underlying(prefix));
    return false;
  }
}

constexpr auto
make_address_key(const Address &addr, Prefix p)
{
  std::array<std::byte, kAddressSize + 1> ret;
  std::ranges::copy(addr.as_bytes(),
                    std::begin(std::as_writable_bytes(std::span(ret))) + 1);
  ret[0] = static_cast<std::byte>(p);
  return ret;
}

constexpr auto
make_wasm_code_key(const SHA256Hash &hash)
{
  std::array<std::byte, hash.size() + 1> ret;
  std::ranges::copy(std::as_bytes(std::span(hash)),
                    std::begin(std::span(ret)) + 1);
  ret[0] = static_cast<std::byte>(Prefix::kWasmCode);
  return ret;
}

constexpr auto
make_contract_data_key(std::span<const std::byte> key)
{
  x23::Vec<std::byte> buf;
  buf.resize_uninitialized(key.size() + 1);
  buf[0] = static_cast<std::byte>(Prefix::kContractDataKey);
  std::ranges::copy(key, std::begin(std::span(buf)) + 1);
  return buf;
}

} // namespace

void
HogDBDAO::load_trie(MerkleTriePrefix &trie)
{
  for_each_state_item([&trie](std::span<const std::byte> key, uint8_t type,
                              std::span<const std::byte> value) {
    std::span<const std::byte> k =
        MerkleTrieHelpers::add_type_to_merkle_key(key.subspan(1), type);
    CHECK(trie.insert(k, value));
  });
  trie.generate_top_level_hashes();
}

void
HogDBDAO::for_each_state_item(GetEachCallback fn)
{
  db_.get_each_parallel([&fn](std::span<const std::byte> key, uint8_t type,
                              std::span<const std::byte> value) {
    if (is_state_item(key))
      fn(key, type, value);
  });
}

HogDBStatus<DbTip>
HogDBDAO::read_dbtip()
{
  HogDBStatus<OwningSpan<const std::byte>> raw =
      db_.get_if_type(std::as_bytes(std::span(kGlobalTipKey)), 0);
  if (!raw)
    return raw.error();

  DbTip tip;
  try {
    x23::deserialize(&tip, x23::BufViewEnd(*raw));
  } catch (x23::XdrException &e) {
    return HogDBError(HogDBCode::kInternal);
  }
  return tip;
}

HogDBStatus<void>
HogDBDAO::write_dbtip(const DbTip &tip)
{
  auto buf = x23::serialize(tip);
  return db_.put(std::as_bytes(std::span(kGlobalTipKey)), 0,
                 std::as_bytes(std::span(buf)));
}

HogDBStatus<void>
HogDBDAO::write_cost_model(const StorageCostModel &cost_model)
{
  auto buf = x23::serialize(cost_model.to_xdr());
  auto key = std::as_bytes(std::span(kGlobalCostModelKey));
  if (db_.get(key)) {
    error("Multiple versions or changing cost models not supported yet");
    return HogDBError(HogDBCode::kInternal);
  }
  return db_.put(key, 0, std::as_bytes(std::span(buf)));
}

HogDBStatus<StorageCostModel>
HogDBDAO::read_cost_model()
{
  HogDBStatus<OwningSpan<const std::byte>> raw =
      db_.get_if_type(std::as_bytes(std::span(kGlobalCostModelKey)), 0);
  if (!raw)
    return raw.error();
  StorageCostModelXdr cost_model{};
  try {
    x23::deserialize(&cost_model, x23::BufViewEnd(*raw));
  } catch (x23::XdrException &e) {
    return HogDBError(HogDBCode::kInternal);
  }
  return StorageCostModel(cost_model);
}

HogDBStatus<int128_t>
HogDBDAO::read_address_balance(const Address &addr)
{
  int128_t val;
  auto key = make_address_key(addr, Prefix::kBalance);
  HogDBStatus<void> ret = db_.get_if_type(std::as_bytes(std::span(key)),
                                          kNNInt128, writable_byte_view(val));
  if (!ret)
    return ret.error();
  return le_to_cpu(val);
}

HogDBStatus<HogAge>
HogDBDAO::read_address_average_hog_age(const Address &addr)
{
  auto key = make_address_key(addr, Prefix::kHogAge);
  HogDBStatus<OwningSpan<const std::byte>> raw =
      db_.get_if_type(std::as_bytes(std::span(key)), 0);
  if (!raw)
    return raw.error();

  HogAge age;
  try {
    x23::deserialize(&age, x23::BufViewEnd(*raw));
  } catch (x23::XdrException &e) {
    return HogDBError(HogDBCode::kInternal);
  }
  return age;
}

HogDBStatus<ReplayCacheData>
HogDBDAO::read_address_replay_cache(Address const &addr)
{
  auto key = make_address_key(addr, Prefix::kReplayCache);
  HogDBStatus<OwningSpan<const std::byte>> raw =
      db_.get_if_type(std::as_bytes(std::span(key)), 0);
  if (!raw)
    return raw.error();

  ReplayCacheData rcd;
  try {
    x23::deserialize(&rcd, x23::BufViewEnd(*raw));
  } catch (x23::XdrException &e) {
    return HogDBError(HogDBCode::kInternal);
  }
  return rcd;
}

// TODO(jhumphri): Handle [block_id].
HogDBStatus<AccountAuthData>
HogDBDAO::read_address_account_auth_data(const Address &addr)
{
  auto key = make_address_key(addr, Prefix::kAccountAuthData);
  HogDBStatus<OwningSpan<const std::byte>> raw =
      db_.get_if_type(std::as_bytes(std::span(key)), 0);
  if (!raw)
    return raw.error();

  AccountAuthData auth;
  try {
    x23::deserialize(&auth, x23::BufViewEnd(*raw));
  } catch (x23::XdrException &e) {
    return HogDBError(HogDBCode::kInternal);
  }
  return auth;
}

HogDBStatus<DeployedContractHash>
HogDBDAO::read_address_metadata(const Address &addr)
{
  auto key = make_address_key(addr, Prefix::kAddressMetadata);
  HogDBStatus<OwningSpan<const std::byte>> raw =
      db_.get_if_type(std::as_bytes(std::span(key)), 0);

  if (!raw)
    return raw.error();

  DeployedContractHash deployed_contract_hash;
  try {
    x23::deserialize(&deployed_contract_hash, x23::BufViewEnd(*raw));
  } catch (x23::XdrException &e) {
    return HogDBError(HogDBCode::kInternal);
  }
  return deployed_contract_hash;
}

HogDBStatus<OwningSpan<const std::byte>>
HogDBDAO::read_wasm_code(const SHA256Hash &hash)
{
  auto key = make_wasm_code_key(hash);
  return db_.get_if_type(std::as_bytes(std::span(key)), 0);
}

HogDBStatus<int128_t>
HogDBDAO::read_nnint128(const ItemKey &key)
{
  if (key.is_address_balance())
    return read_address_balance(key.get_address());

  int128_t val;
  HogDBStatus<void> ret =
      db_.get_if_type(make_contract_data_key(key.as_bytes()), kNNInt128,
                      writable_byte_view(val));
  if (!ret)
    return ret.error();
  return le_to_cpu(val);
}

HogDBStatus<OwningSpan<const std::byte>>
HogDBDAO::read_bytes(const ItemKey &key)
{
  return db_.get_if_type(make_contract_data_key(key.as_bytes()), kBytes);
}

HogDBStatus<void>
HogDBDAO::write_address_balance(const Address &addr, int128_t val)
{
  auto key = make_address_key(addr, Prefix::kBalance);
  std::optional<HogDBStatus<DBItem>> before_item;
  if (!invariants_.empty()) {
    before_item = db_.get(std::as_bytes(std::span(key)));
  }

  auto ret = db_.put(std::as_bytes(std::span(key)), kNNInt128,
                     byte_view(cpu_to_le(val)));

  if (!invariants_.empty()) {
    auto after_item = db_.get(std::as_bytes(std::span(key)));
    for (auto &invariant : invariants_) {
      invariant->log_item_change(key, *before_item, after_item);
    }
  }
  return ret;
}

HogDBStatus<void>
HogDBDAO::write_address_average_hog_age(const HogAge &age)
{
  auto key = make_address_key(age.address, Prefix::kHogAge);
  auto buf = x23::serialize(age);
  return db_.put(std::as_bytes(std::span(key)), 0,
                 std::as_bytes(std::span(buf)));
}

// TODO(jhumphri): Handle [block_id].
HogDBStatus<void>
HogDBDAO::write_address_account_auth_data(const Address &addr,
                                          const AccountAuthData &auth)
{
  auto key = make_address_key(addr, Prefix::kAccountAuthData);
  auto buf = x23::serialize(auth);

  auto before_item = get_before_item(key);

  auto ret =
      db_.put(std::as_bytes(std::span(key)), 0, std::as_bytes(std::span(buf)));
  update_invariants_post(key, before_item);
  return ret;
}

HogDBStatus<void>
HogDBDAO::write_address_replay_cache(Address const &addr,
                                     ReplayCacheData const &replay_cache)
{
  auto key = make_address_key(addr, Prefix::kReplayCache);
  auto buf = x23::serialize(replay_cache);

  auto before_item = get_before_item(key);

  auto ret =
      db_.put(std::as_bytes(std::span(key)), 0, std::as_bytes(std::span(buf)));
  update_invariants_post(key, before_item);
  return ret;
}

HogDBStatus<void>
HogDBDAO::write_address_metadata(Address const &addr,
                                 const DeployedContractHash &hash)
{
  auto key = make_address_key(addr, Prefix::kAddressMetadata);
  auto buf = x23::serialize(hash);
  auto key_span = std::as_bytes(std::span(key));

  auto before_item = get_before_item(key_span);
  auto ret = db_.put(key_span, 0, std::as_bytes(std::span(buf)));

  update_invariants_post(key_span, before_item);
  return ret;
}

HogDBStatus<void>
HogDBDAO::write_wasm_code(const SHA256Hash &hash,
                          std::span<const std::byte> buf)
{
  auto key = make_wasm_code_key(hash);
  return db_.put(std::as_bytes(std::span(key)), 0, buf);
}

HogDBStatus<void>
HogDBDAO::write_nnint128(const ItemKey &key, int128_t val)
{
  if (key.is_address_balance())
    return write_address_balance(key.get_address(), val);
  assert(key.is_contract_data());

  auto db_key = make_contract_data_key(key.as_bytes());
  auto before_item = get_before_item(db_key);
  auto ret = db_.put(db_key, kNNInt128, byte_view(cpu_to_le(val)));
  update_invariants_post(db_key, before_item);
  return ret;
}

HogDBStatus<void>
HogDBDAO::write_bytes(const ItemKey &key, std::span<const std::byte> buf)
{
  auto db_key = make_contract_data_key(key.as_bytes());
  auto before_item = get_before_item(db_key);
  auto ret = db_.put(db_key, kBytes, buf);
  update_invariants_post(db_key, before_item);
  return ret;
}

HogDBStatus<size_t>
HogDBDAO::get_charged_storage_cost(const ItemKey &key,
                                   StorageCostModel const *cost_model)
{
  switch (key.key_type()) {
  case ItemKeyType::kItemKeyTypeFreeNativeTokenBalance:
    assert(key.is_address_balance());
    // this is not charged
    return HogDBError(HogDBCode::kNotFound);
  case ItemKeyType::kItemKeyTypeAccountAuthData:
    assert(key.is_account_auth_data());
    {
      auto r = read_address_account_auth_data(key.get_address());
      if (!r && r.error() != HogDBCode::kNotFound) {
        return r.error();
      }
      if (!r) {
        assert(r.error() == HogDBCode::kNotFound);
        return 0;
      }
      size_t active_keys = 0;
      for (size_t i = 0; i < MAX_SIGNERS; ++i) {
        if (r.value().public_keys[i]) {
          active_keys++;
        }
      }
      return cost_model->get_auth_data_cost(active_keys);
    }
  case ItemKeyType::kItemKeyTypeAddressMetadata:
    assert(key.is_address_metadata());
    {
      auto db_key =
          make_address_key(key.get_address(), Prefix::kAddressMetadata);
      auto item = db_.get(db_key);
      if (!item) {
        return HogDBError(HogDBCode::kNotFound);
      }
      DeployedContractHash deployed_contract_hash;
      try {
        x23::deserialize(&deployed_contract_hash,
                         x23::BufViewEnd(item.value().value));
      } catch (x23::XdrException &e) {
        error("failed to deserialize deployed contract hash: {}", e.what());
        return HogDBError(HogDBCode::kInternal);
      }
      return cost_model->get_contract_size_cost(
          deployed_contract_hash.contract_size);
    }
  case ItemKeyType::kItemKeyTypeContractData:
  {
    auto item = db_.get(make_contract_data_key(key.as_bytes()));
    if (!item) {
      return HogDBError(HogDBCode::kNotFound);
    }
    switch (item.value().type) {
    case kNNInt128:
      return cost_model->get_nnint_cost();
    case kBytes:
      return cost_model->get_bytestring_cost(item.value().value.size());
    default:
      error("unknown item type loaded from db: key={} type={}", key,
            item.value().type);
      return HogDBError(HogDBCode::kInternal);
    }
  }
  case ItemKeyType::kItemKeyTypeContractCode:
  case ItemKeyType::kItemKeyTypeContractRefcount:
    // these keys are not charged: storage costs for contracts are charged on
    // the deployment entries
    return HogDBError(HogDBCode::kNotFound);
  }
  error("unknown item key type: {}", std::to_underlying(key.key_type()));
  return HogDBError(HogDBCode::kInternal);
}

HogDBStatus<int8_t>
HogDBDAO::get_item_type(const ItemKey &key)
{
  if (!key.is_contract_data()) [[unlikely]] {
    error("item key is not contract data: {}", key.key_type());
    return HogDBError(HogDBCode::kInternal);
  }
  return db_.get_type(make_contract_data_key(key.as_bytes()));
}

HogDBStatus<DBItem>
HogDBDAO::read_contract_data_item(const ItemKey &key)
{
  if (!key.is_contract_data()) [[unlikely]] {
    error("item key is not contract data: {}", key.key_type());
    return HogDBError(HogDBCode::kInternal);
  }
  return db_.get(make_contract_data_key(key.as_bytes()));
}

HogDBStatus<void>
HogDBDAO::remove_item(const ItemKey &key)
{
  if (!key.is_contract_data()) [[unlikely]] {
    error("item key is not contract data: {}", key.key_type());
    return HogDBError(HogDBCode::kInternal);
  }
  auto db_key = make_contract_data_key(key.as_bytes());
  auto before_item = get_before_item(db_key);
  auto ret = db_.remove(db_key);
  update_invariants_post(db_key, before_item);
  return ret;
}

} // namespace hogd
