// item_change.h - modification tracking for DB items.
//
// Design Overview:
//
// Each ItemChange tracks modifications to an item. Conceptually, an ItemChange
// has two components: a reservation and a change. The reservation defines what
// should cause conflicts with other changes while the change modifies the
// item's value if committed. Reservations can be combined into their most
// restrictive intersection with narrow(). On the other hand, both reservations
// and changes can be combined together with merge().
//
// This distinction is useful because subtransactions must contain reservations
// that are at least as restrictive as the parent transaction for any given
// item. Otherwise, rolling back could fail because of a lock taken by another
// transaction. Therefore the reservation might have to be more restrictive
// than the change.

#pragma once

#include <array>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "account.hh"
#include "contract.hh"
#include "hogd/base/compiler.h"
#include "hogd/base/span.h"
#include "hogd/base/string.h"
#include "hogd/base/types.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "types.hh"
#include "x23/format.h"

namespace hogd {

// is_item is a concept that defines the required methods for a class that
// tracks a change to a DB item. Each class that implements this concept is an
// item type.
template<typename T>
concept is_item = requires(T t, HogDBDAO &db, MerkleTriePrefix &trie,
                           const ItemKey &key, const T &arg,
                           const StorageCostModel *cost_model) {
  // applies the change to the ramdb.
  { t.commit(db, trie, key) } -> std::same_as<HogDBStatus<void>>;
  // narrow() is called when a transaction wants to modify its pending change to
  // an item.  Makes the change's reservation at least as restrictive as arg.
  // However, does not merge the value change in arg. Instead, the narrowed
  // change replaces the prior change for the item (InvokeTX::request_change).
  // Returns true if narrowing was successful. If narrowing is unsuccessful,
  // then the change becomes exclusive and all future changes (merges)
  // attempted by other transactions for this item will fail.
  { t.narrow(arg) } -> std::same_as<bool>;
  // merges the change and reservation in arg (will fail if there is a
  // conflict). This combines a change from one transaction with a change from
  // another transaction.
  // Returns an upper bound of the TOTAL amount of storage tokens
  // that must be locked to support the item, after the merge.
  // THIS AMOUNT IS DETERMINISTIC (unless we implement retrocausal nnints).
  // The actual amount of storage tokens that are locked
  // will be reduced by the amount locked for the current snapshot.
  // IMPORTANT: merge() must be a NO-OP if the result is a HogDBError.
  { t.merge(arg, cost_model) } -> std::same_as<HogDBStatus<int128_t>>;
  // An upper bound on the amount of storage tokens needed to support the
  // item change, for the case where the change is the first one applied to
  // the item. I.e. this is the amount returned by t.merge() for t = empty
  // ItemChange.
  // IMPORTANT: Unless we implement retrocausal nnints,
  // this MUST return the same value as merge() (for the same reason that merge
  // must return a deterministic function of the snapshot item + the current
  // change).)
  { t.marginal_storage_cost(cost_model) } -> std::same_as<int128_t>;

  // reverts the change and reservation in arg that was previously merged.
  { t.undo_merge(arg) } -> std::same_as<void>;
  // returns the size in bytes of storage required in ramdb.
  { t.storage_size() } -> std::same_as<size_t>;

  // Some items, if necessary, have a commit_change method.
  // { t.commit_change(arg) } -> std::same_as<void>;

  // Returns the amount of storage tokens that are actually needed to
  // support the item, for all of the changes that are currently committed.
  // The caller is responsible for releasing the difference between the
  // amount returned by merge, and the difference in this cost before and
  // after a call to commit_change.
  //
  // Is not called if no changes have been committed.
  { t.committed_storage_cost(cost_model) } -> std::same_as<int128_t>;
} && std::formattable<T, char>;

template<typename T>
concept is_item_commit_required = requires(T t, const T &arg) {
  { t.commit_change(arg) } -> std::same_as<void>;
} && is_item<T>;

// NNInt128 commutes with other NNInt128's if base + minus >= 0 && base +
// plus does not overflow. The item's value at commit time is base + delta.
class NNInt128 {
public:
  NNInt128(int128_t base, int128_t delta) : base_(base), delta_(delta)
  {
    if (delta_ > 0) {
      plus_ = delta_;
      return;
    }
    minus_ = delta_;
  }

  HogDBStatus<void> commit(HogDBDAO &db, MerkleTriePrefix &trie,
                           const ItemKey &key) const;
  bool narrow(const NNInt128 &nnint);
  HogDBStatus<int128_t> merge(const NNInt128 &nnint,
                              const StorageCostModel *cost_model);
  void undo_merge(const NNInt128 &nnint);

  static size_t storage_size() { return sizeof(int128_t); }
  int128_t value() const { return base_ + delta_; }
  int128_t base() const { return base_; }
  int128_t delta() const { return delta_; }

  static int128_t committed_storage_cost(const StorageCostModel *cost_model)
  {
    return marginal_storage_cost(cost_model);
  }

  constexpr static int128_t
  marginal_storage_cost(const StorageCostModel *cost_model)
  {
    return cost_model->get_nnint_cost();
  }

private:
  int128_t base_;
  int128_t delta_;
  int128_t plus_{0};  // sum of all positive deltas (positive value)
  int128_t minus_{0}; // sum of all negative deltas (negative value)

  void assert_is_valid() const
  {
    // check for valid bounds.
    assert(minus_ <= 0);
    assert(plus_ >= 0);
    assert(minus_ <= delta_);
    assert(delta_ <= plus_);

    // check that the object is not in a conflict state.
    [[maybe_unused]] auto [ign, of] = overflowing_add(base_, plus_);
    assert(!of);
    assert(base_ + minus_ >= 0);
  }
};

} // namespace hogd

template<> struct std::formatter<hogd::NNInt128> {
  static constexpr auto parse(format_parse_context &ctx) { return ctx.begin(); }

  static auto format(const hogd::NNInt128 &nnint, auto &ctx)
  {
    return format_to(ctx.out(), "NNInt128{{base={}}}", nnint.value());
  }
};
static_assert(hogd::is_item<hogd::NNInt128>);

namespace hogd {

// Bytes is an item type that manages an opaque variable-length arrays of
// bytes. It commutes with other Bytes's if the array is equal.
struct Bytes {
public:
  Bytes(std::span<const std::byte> buf) : buf_(buf.begin(), buf.end()) {}

  HogDBStatus<void> commit(HogDBDAO &db, MerkleTriePrefix &trie,
                           const ItemKey &key) const;
  bool narrow(const Bytes &bytes) { return buf_ == bytes.buf_; }
  HogDBStatus<int128_t> merge(const Bytes &bytes,
                              const StorageCostModel *cost_model)
  {
    if (buf_ != bytes.buf_)
      return HogDBError(HogDBCode::kConflict);
    return marginal_storage_cost(cost_model);
  }
  static void undo_merge(const Bytes &bytes) {}

  size_t storage_size() const { return buf_.size(); }
  OwningSpan<const std::byte> value() const
  {
    std::vector<std::byte> buf(buf_);
    auto span = std::span<const std::byte>(buf);
    return OwningSpan(span, [ref = std::move(buf)] {});
  }

  int128_t committed_storage_cost(const StorageCostModel *cost_model) const
  {
    return marginal_storage_cost(cost_model);
  }

  int128_t marginal_storage_cost(const StorageCostModel *cost_model) const
  {
    return cost_model->get_bytestring_cost(buf_.size());
  }

private:
  std::vector<std::byte> buf_;
};

} // namespace hogd

template<> struct std::formatter<hogd::Bytes> {
  static constexpr auto parse(format_parse_context &ctx) { return ctx.begin(); }

  static auto format(const hogd::Bytes &bytes, auto &ctx)
  {
    auto value = bytes.value();
    auto hex = hogd::encode_hex(value);
    return format_to(ctx.out(), "Bytes{{hex:{}}}", hex);
  }
};
static_assert(hogd::is_item<hogd::Bytes>);

namespace hogd {

// Deletion is an item type that removes the item. It commutes with other
// Deletion's to the same item.
class Deletion {
public:
  static HogDBStatus<void> commit(HogDBDAO &db, MerkleTriePrefix &trie,
                                  const ItemKey &key);
  static bool narrow(const Deletion & /*del*/) { return true; }
  static HogDBStatus<int128_t> merge(const Deletion & /*del*/,
                                     const StorageCostModel *cost_model)
  {
    return marginal_storage_cost(cost_model);
  }
  static void undo_merge(const Deletion &del) {}
  static size_t storage_size() { return 0; }

  static int128_t committed_storage_cost(const StorageCostModel *cost_model)
  {
    return marginal_storage_cost(cost_model);
  }

  constexpr static int128_t
  marginal_storage_cost(const StorageCostModel * /*unused*/)
  {
    return 0;
  }
};

} // namespace hogd

template<> struct std::formatter<hogd::Deletion> {
  static constexpr auto parse(format_parse_context &ctx) { return ctx.begin(); }
  static auto format(const hogd::Deletion & /*unused*/, auto &ctx)
  {
    return format_to(ctx.out(), "Deletion");
  }
};

static_assert(hogd::is_item<hogd::Deletion>);

namespace hogd {

class DeployedContractData {
public:
  DeployedContractData(DeployedContractHash deployed_contract_hash)
    : deployed_contract_hash_(deployed_contract_hash)
  {}

  HogDBStatus<void> commit(HogDBDAO &db, MerkleTriePrefix &trie,
                           const ItemKey &key) const;
  bool narrow(const DeployedContractData &deployed_contract_data)
  {
    assert(deployed_contract_hash_ ==
           deployed_contract_data.deployed_contract_hash_);
    return true;
  }
  HogDBStatus<int128_t>
  merge(const DeployedContractData &deployed_contract_data,
        const StorageCostModel *cost_model)
  {
    assert(deployed_contract_hash_ ==
           deployed_contract_data.deployed_contract_hash_);
    return marginal_storage_cost(cost_model);
  }
  static void undo_merge(const DeployedContractData &deployed_contract_data) {}
  size_t storage_size() const { return deployed_contract_hash_.contract_size; }

  const DeployedContractHash &deployed_contract_hash() const
  {
    return deployed_contract_hash_;
  }

  int128_t committed_storage_cost(const StorageCostModel *cost_model) const
  {
    return marginal_storage_cost(cost_model);
  }

  int128_t marginal_storage_cost(const StorageCostModel *cost_model) const
  {
    return cost_model->get_contract_size_cost(
        deployed_contract_hash_.contract_size);
  }

private:
  DeployedContractHash deployed_contract_hash_;
};

} // namespace hogd

template<> struct std::formatter<hogd::DeployedContractData> {
  static constexpr auto parse(format_parse_context &ctx) { return ctx.begin(); }
  static auto format(const hogd::DeployedContractData &deployed_contract_data,
                     auto &ctx)
  {
    return format_to(ctx.out(),
                     "DeployedContractData{{deployed_contract_hash:{}}}",
                     x23::show(
                         deployed_contract_data.deployed_contract_hash()));
  }
};
static_assert(hogd::is_item<hogd::DeployedContractData>);

namespace hogd {

class NewContractData {
public:
  NewContractData(Contract new_contract)
    : new_contract_(std::move(new_contract))
  {}

  HogDBStatus<void> commit(HogDBDAO &db, MerkleTriePrefix & /*unused*/,
                           const ItemKey &key) const;
  bool narrow(const NewContractData &new_contract_data)
  {
    assert(new_contract_ == new_contract_data.new_contract_);
    return true;
  }
  HogDBStatus<int128_t> merge(const NewContractData &new_contract_data,
                              const StorageCostModel * /*unused*/)
  {
    assert(new_contract_ == new_contract_data.new_contract_);
    return 0;
  }
  static void undo_merge(const NewContractData &new_contract_data) {}
  static size_t storage_size()
  {
    // Storage costs for new contracts are accounted for when deploying them,
    // not in the actual contract item
    return 0;
  }

  static int128_t committed_storage_cost(const StorageCostModel * /*unused*/)
  {
    return 0;
  }

  const Contract &new_contract() const { return new_contract_; }

  constexpr static int128_t
  marginal_storage_cost(const StorageCostModel * /*unused*/)
  {
    return 0;
  }

private:
  Contract new_contract_;
};

} // namespace hogd

template<> struct std::formatter<hogd::NewContractData> {
  static constexpr auto parse(format_parse_context &ctx) { return ctx.begin(); }
  static auto format(const hogd::NewContractData &new_contract_data, auto &ctx)
  {
    return format_to(ctx.out(), "NewContractData{{new_contract:{}}}",
                     x23::show(new_contract_data.new_contract()));
  }
};
static_assert(hogd::is_item<hogd::NewContractData>);

namespace hogd {

class AuthDataItem {
public:
  AuthDataItem(AccountAuthData auth_data)
    : base_auth_data_(std::move(auth_data))
  {}

  void set_new_pk(uint32_t key_id, PublicKeyConfigOpt pk)
  {
    assert(key_id < MAX_SIGNERS);
    pk_changes_[key_id].pk_ = std::move(pk);
    assert(pk_changes_[key_id].refcount_ == 0);
    assert(!pk_changes_[key_id].exclusive_);
    pk_changes_[key_id].refcount_ = 1;
  }

  HogDBStatus<void> commit(HogDBDAO &db, MerkleTriePrefix &trie,
                           const ItemKey &key) const;

  void commit_change(AuthDataItem const &auth_data_item);
  bool narrow(const AuthDataItem &auth_data_item);

  HogDBStatus<int128_t> merge(const AuthDataItem &auth_data_item,
                              const StorageCostModel *cost_model);

  void undo_merge(const AuthDataItem &auth_data_item);

  // TODO(geoff): implement or remove storage_size entirely
  static size_t storage_size() { return 0; }

  int128_t committed_storage_cost(const StorageCostModel *cost_model) const;
  int128_t marginal_storage_cost(const StorageCostModel *cost_model) const;

  AccountAuthData build_result() const;

private:
  struct PublicKeyChange {
    PublicKeyChange(PublicKeyConfigOpt pk) : pk_(std::move(pk)) {}
    PublicKeyChange() = default;
    uint32_t refcount_{0};
    bool exclusive_{false};
    bool committed_{false};
    PublicKeyConfigOpt pk_ = nullptr;
  };

  std::array<PublicKeyChange, MAX_SIGNERS> pk_changes_;
  AccountAuthData base_auth_data_;

  friend struct std::formatter<AuthDataItem>;
};

} // namespace hogd

template<> struct std::formatter<hogd::AuthDataItem> {
  static constexpr auto parse(format_parse_context &ctx) { return ctx.begin(); }

  static auto format(const hogd::AuthDataItem &auth_data_item, auto &ctx)
  {
    format_to(ctx.out(), "AuthDataItem{{");
    for (size_t i = 0; i < hogd::MAX_SIGNERS; i++) {
      auto pk = auth_data_item.pk_changes_[i].pk_;
      if (pk) {
        format_to(ctx.out(), "{{index:{}, pk:{}, exclusive:{}, refcount:{}}}",
                  i, x23::show(pk), auth_data_item.pk_changes_[i].exclusive_,
                  auth_data_item.pk_changes_[i].refcount_);
      }
    }
    format_to(ctx.out(), "}}");
    return ctx.out();
  }
};
static_assert(hogd::is_item_commit_required<hogd::AuthDataItem>);

namespace hogd {

class ReplayCacheIt {
public:
  ReplayCacheIt(BlockHeight expired_height, ReplayCacheData cache);

  void assert_is_valid() const
  {
    // no duplicate nonces anywhere
    std::unordered_set<ReplayNonce> nonces{};
    nonces.insert_range(base_replay_cache_.uniquifiers |
                        std::views::transform(&Uniquifier::nonce));
    nonces.insert_range(additions_ | std::views::transform(&Uniquifier::nonce));
    assert(nonces.size() == base_replay_cache_.uniquifiers + additions_.size());

    // no expired uniquifiers - can't check this without acccess to dbtip
  }

  HogDBStatus<void> check_conflict(Uniquifier const &uniquifier);
  HogDBStatus<void> try_consume_uniquifier(Uniquifier const &uniquifier);

  HogDBStatus<void> commit(HogDBDAO &db, MerkleTriePrefix &trie,
                           const ItemKey &key) const;

  bool narrow(const ReplayCacheIt &replay_cache_item);

  HogDBStatus<int128_t> merge(const ReplayCacheIt &replay_cache_item,
                              const StorageCostModel *cost_model);

  void undo_merge(const ReplayCacheIt &replay_cache_item);

  // TODO(stutsman): implement or remove storage_size entirely
  static size_t storage_size() { return 0; }

  int128_t committed_storage_cost(const StorageCostModel *cost_model) const;
  int128_t marginal_storage_cost(const StorageCostModel *cost_model) const;

  ReplayCacheData build_result() const;

  BlockHeight expired_height_;
  ReplayCacheData base_replay_cache_;
  std::vector<Uniquifier> additions_;

  friend struct std::formatter<ReplayCacheIt>;
};

} // namespace hogd

template<> struct std::formatter<hogd::ReplayCacheIt> {
  static constexpr auto parse(format_parse_context &ctx) { return ctx.begin(); }

  static auto format(const hogd::ReplayCacheIt &replay_cache_item, auto &ctx)
  {
    format_to(ctx.out(), "ReplayCacheIt{{");
    format_to(ctx.out(), "base_replay_cache_: {}, ",
              x23::show(replay_cache_item.base_replay_cache_));
    format_to(ctx.out(), "additions_: [");
    for (auto const &addition : replay_cache_item.additions_)
      format_to(ctx.out(), "{},", x23::show(addition));
    format_to(ctx.out(), "]");
    return ctx.out();
  }
};

namespace hogd {

static_assert(hogd::is_item<hogd::ReplayCacheIt>);

// ItemChange represents a pending modification to an item.
class ItemChange {
public:
  ItemChange() = default;
  ItemChange(NNInt128 &&nnint) : type_(std::move(nnint)) {}
  ItemChange(Bytes &&bytes) : type_(std::move(bytes)) {}
  ItemChange(Deletion &&d) : type_(std::move(d)) {}
  ItemChange(DeployedContractData &&deployed_contract_data)
    : type_(std::move(deployed_contract_data))
  {}
  ItemChange(NewContractData &&new_contract_data)
    : type_(std::move(new_contract_data))
  {}
  ItemChange(AuthDataItem &&auth_data_item) : type_(std::move(auth_data_item))
  {}
  ItemChange(ReplayCacheIt &&replay_cache) : type_(std::move(replay_cache)) {}
  ~ItemChange() = default;

  // setters
  // This should only be called when the snapshot cost is explicitly knowable:
  // I.e. when the cost is 0 (for balances or other reserved item key types).
  void set_snapshot_cost(int128_t snapshot_cost)
  {
    snapshot_cost_ = snapshot_cost;
  }

  // getters
  std::optional<int128_t> to_nnint128() const;
  OwningSpan<const std::byte> to_bytes() const;
  std::optional<AuthDataItem> to_auth_data() const;
  std::optional<ReplayCacheIt> to_replay_cache() const;
  bool is_deletion() const { return std::holds_alternative<Deletion>(type_); }
  bool is_nnint128() const { return std::holds_alternative<NNInt128>(type_); }
  bool is_exclusive() const { return exclusive_; }
  bool is_create_contract() const
  {
    return std::holds_alternative<NewContractData>(type_);
  }
  std::optional<NNInt128> get_raw_nnint128() const;
  std::optional<SHA256Hash> get_raw_deployed_contract_hash() const;
  bool is_auth_data() const
  {
    return std::holds_alternative<AuthDataItem>(type_);
  }
  bool is_replay_cache() const
  {
    return std::holds_alternative<ReplayCacheIt>(type_);
  }

  // commit applies the item's changes to the ramdb.
  HogDBStatus<void> commit(HogDBDAO &db, MerkleTriePrefix &trie,
                           ItemKey key) const;

  // narrow combines the reservation of a change into this one.
  void narrow(const ItemChange &change);
  // merge merges the reservation and value of a change into this one. It
  // returns kConflict if the change does not commute.
  HogDBStatus<int128_t> merge(const ItemChange &change,
                              const StorageCostModel *cost_model);
  // undo_merge reverts a change that was merged with this one.
  void undo_merge(const ItemChange &change);
  // commit_change marks a change as committed.
  // The change MUST have been previously merged.
  void commit_change(const ItemChange &change);

  HogDBStatus<void> load_snapshot_cost(HogDBDAO &db, const ItemKey &key,
                                       const StorageCostModel *cost_model);
  HogDBStatus<int128_t>
  marginal_storage_cost(const StorageCostModel *cost_model) const;

  int128_t committed_storage_cost(const StorageCostModel *cost_model) const;
  auto get_type() const { return type_.index(); }

  bool no_storage_cost() const
  {
    // is_create_contract pays storage cost via the deployment entries
    return is_deletion() || is_create_contract();
  }

private:
  struct LazySnapshotCost {
    std::optional<int128_t> cost_;

    LazySnapshotCost &operator=(const LazySnapshotCost &other)
    {
      if (cost_ && other.cost_) {
        assert(*cost_ == *other.cost_);
      }
      if (!cost_) {
        cost_ = other.cost_;
      }
      return *this;
    }
    LazySnapshotCost &operator=(int128_t cost)
    {
      if (cost_) {
        assert(*cost_ == cost);
      }
      cost_ = cost;
      return *this;
    }

    bool has_value() const { return cost_.has_value(); }
    int128_t value() const
    {
      assert(cost_.has_value());
      return cost_.value();
    }
    auto &operator*(this auto &self)
    {
      assert(self.cost_.has_value());
      return *self.cost_;
    }
    operator bool() const { return cost_.has_value(); }
  };

  bool exclusive_{false};
  LazySnapshotCost snapshot_cost_;
  bool change_committed_{false};
  std::variant<std::monostate, NNInt128, Bytes, Deletion, DeployedContractData,
               NewContractData, AuthDataItem, ReplayCacheIt>
      type_;

  size_t storage_size() const;

  friend struct std::formatter<ItemChange>;
};

inline std::optional<int128_t>
ItemChange::to_nnint128() const
{
  if (const auto *tmp = std::get_if<NNInt128>(&type_); tmp)
    return tmp->value();
  return std::nullopt;
}

inline OwningSpan<const std::byte>
ItemChange::to_bytes() const
{
  if (const auto *tmp = std::get_if<Bytes>(&type_); tmp)
    return tmp->value();
  return {};
}

inline std::optional<AuthDataItem>
ItemChange::to_auth_data() const
{
  if (const auto *tmp = std::get_if<AuthDataItem>(&type_); tmp)
    return *tmp;
  return std::nullopt;
}

inline std::optional<NNInt128>
ItemChange::get_raw_nnint128() const
{
  if (const auto *tmp = std::get_if<NNInt128>(&type_); tmp)
    return *tmp;
  return std::nullopt;
}

inline std::optional<ReplayCacheIt>
ItemChange::to_replay_cache() const
{
  if (const auto *tmp = std::get_if<ReplayCacheIt>(&type_); tmp)
    return *tmp;
  return std::nullopt;
}

inline HogDBStatus<ItemChange>
make_balance_change(HogDBDAO &db, const Address &addr, int128_t delta)
{
  auto balance = db.read_address_balance(addr);
  if (!balance.has_value()) {
    if (balance.error() != HogDBCode::kNotFound) {
      return balance.error();
    }
    balance = 0;
  }
  auto out = ItemChange(NNInt128(*balance, delta));
  out.set_snapshot_cost(0);
  return out;
}

} // namespace hogd

template<> struct std::formatter<hogd::ItemChange> {
  static constexpr auto parse(format_parse_context &ctx) { return ctx.begin(); }
  static auto format(const hogd::ItemChange &item_change, auto &ctx)
  {
    std::visit(
        [&ctx](auto &&arg) {
          if constexpr (!std::same_as<std::decay_t<decltype(arg)>,
                                      std::monostate>) {
            format_to(ctx.out(), "{}", arg);
          }
          else {
            format_to(ctx.out(), "monostate");
          }
        },
        item_change.type_);
    return ctx.out();
  }
};
