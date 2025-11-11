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
#include <memory>
#include <optional>
#include <ranges>
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
#include "storage_item.hh"
#include "transaction.hh"
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
  // narrow() is called when a transaction wants to modify its pending change to
  // an item.  Makes the change's reservation at least as restrictive as arg.
  // However, does not merge the value change in arg. Instead, the narrowed
  // change replaces the prior change for the item (InvokeTX::request_change).
  // Returns true if narrowing was successful. If narrowing is unsuccessful,
  // then the change becomes exclusive and all future changes (merges)
  // attempted by other transactions for this item will fail.
  { t.narrow(arg) } -> std::same_as<bool>;

} && std::formattable<T, char>;

template<is_item T> class PointerItem : public std::unique_ptr<T> {
public:
  template<typename... Args>
  PointerItem(Args &&...args)
    : std::unique_ptr<T>(new T(std::forward<Args>(args)...))
  {}

  PointerItem(PointerItem &&other) noexcept
    : std::unique_ptr<T>(std::move(other)) {};

  PointerItem &operator=(PointerItem &&other) noexcept
  {
    std::unique_ptr<T>::operator=(std::move(other));
    return *this;
  }

  // ItemChange requires a copy constructor;
  // This may be nontrivially expensive; we should correspondinginly increase
  // gas costs for item types that require this pointer indirection.
  PointerItem(const PointerItem &other)
    : std::unique_ptr<T>(new T(*other.get()))
  {}
  PointerItem &operator=(const PointerItem &other)
  {
    std::unique_ptr<T>::reset(new T(*other.get()));
    return *this;
  }

  bool narrow(const PointerItem &pointer_item)
  {
    return this->get()->narrow(*pointer_item.get());
  }

private:
  friend struct std::formatter<PointerItem<T>>;
  friend class ItemChange;
};

} // namespace hogd

template<hogd::is_item T> struct std::formatter<hogd::PointerItem<T>> {
  static constexpr auto parse(format_parse_context &ctx) { return ctx.begin(); }

  static auto format(const hogd::PointerItem<T> &pointer_item, auto &ctx)
  {
    return format_to(ctx.out(), "{}", *pointer_item.get());
  }
};

namespace hogd {

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

  bool narrow(const NNInt128 &nnint);
 
  int128_t value() const { return base_ + delta_; }

  int128_t base() const { return base_; }
  int128_t delta() const { return delta_; }
  int128_t plus() const { return plus_; }
  int128_t minus() const { return minus_; }

  bool operator==(const NNInt128 &other) const = default;

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
    return format_to(ctx.out(),
                     "NNInt128{{base={}, delta={}, plus={}, minus={}}}",
                     nnint.base(), nnint.delta(), nnint.plus(), nnint.minus());
  }
};
static_assert(hogd::is_item<hogd::NNInt128>);

namespace hogd {

// Bytes is an item type that manages an opaque variable-length arrays of
// bytes. It commutes with other Bytes's if the array is equal.
struct Bytes {
public:
  Bytes(std::span<const std::byte> buf) : buf_(buf.begin(), buf.end()) {}

  bool narrow(const Bytes &bytes) { return buf_ == bytes.buf_; }
 
  OwningSpan<const std::byte> value() const
  {
    std::vector<std::byte> buf(buf_);
    auto span = std::span<const std::byte>(buf);
    return OwningSpan(span, [ref = std::move(buf)] {});
  }

  std::span<const std::byte> value_view() const
  {
    return std::span<const std::byte>(buf_);
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
 
  const DeployedContractHash &deployed_contract_hash() const
  {
    return deployed_contract_hash_;
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

  bool narrow(const NewContractData &new_contract_data)
  {
    assert(new_contract_ == new_contract_data.new_contract_);
    return true;
  }

  const Contract &new_contract() const { return new_contract_; }

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
    pk_changes_[key_id].emplace(std::move(pk));
  }

  bool narrow(const AuthDataItem &auth_data_item) const;

  AccountAuthData const &base_auth_data() const { return base_auth_data_; }

private:

  std::array<std::optional<PublicKeyConfigOpt>, MAX_SIGNERS> pk_changes_;
  AccountAuthData base_auth_data_;

  friend struct std::formatter<AuthDataItem>;
  friend class AuthLock;
};

} // namespace hogd

template<> struct std::formatter<hogd::AuthDataItem> {
  static constexpr auto parse(format_parse_context &ctx) { return ctx.begin(); }

  static auto format(const hogd::AuthDataItem &auth_data_item, auto &ctx)
  {
    format_to(ctx.out(), "AuthDataItem{{");
    for (size_t i = 0; i < hogd::MAX_SIGNERS; i++) {
      auto pk = auth_data_item.pk_changes_[i];
      if (pk) {
        format_to(ctx.out(), "{{index:{}, pk:{}}}",
                  i, x23::show(*pk));
      }
    }
    format_to(ctx.out(), "}}");
    return ctx.out();
  }
};
static_assert(
    hogd::is_item<hogd::PointerItem<hogd::AuthDataItem>>);

namespace hogd {

class ReplayCacheIt {
public:
  ReplayCacheIt(BlockHeight expired_height, ReplayCacheData const &cache);

  void assert_is_valid() const
  {
    if constexpr (is_debug_build()) {
      // no duplicate nonces anywhere
      std::unordered_set<ReplayNonce> nonces{};
      {
        auto view = base_replay_cache_.uniquifiers |
                    std::views::transform(&Uniquifier::nonce);
        nonces.insert(view.begin(), view.end());
      }
      {
        auto view = additions_ | std::views::transform(&Uniquifier::nonce);
        nonces.insert(view.begin(), view.end());
      }
      assert(nonces.size() ==
             base_replay_cache_.uniquifiers.size() + additions_.size());

      // no expired uniquifiers - can't check this without acccess to dbtip
    }
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

  // getters
  std::optional<int128_t> to_nnint128() const;
  OwningSpan<const std::byte> to_bytes() const;
  std::optional<AuthDataItem> to_auth_data() const;
  std::optional<ReplayCacheIt> to_replay_cache() const;
  std::optional<NewContractData> to_new_contract() const;
  std::optional<DeployedContractData> to_new_deployment() const;
  bool is_bytes() const { return std::holds_alternative<Bytes>(type_); }
  bool is_deletion() const { return std::holds_alternative<Deletion>(type_); }
  bool is_nnint128() const { return std::holds_alternative<NNInt128>(type_); }
  bool is_exclusive() const { return exclusive_; }
  bool is_permanent_exclusive() const { return permanent_exclusive_; }
  void set_permanent_exclusive()
  {
    permanent_exclusive_ = true;
    exclusive_ = true;
  }
  bool is_create_contract() const
  {
    return std::holds_alternative<NewContractData>(type_);
  }
  bool is_deploy_contract() const
  {
    return std::holds_alternative<DeployedContractData>(type_);
  }

  std::optional<NNInt128> get_raw_nnint128() const;
  std::optional<SHA256Hash> get_raw_deployed_contract_hash() const;
  bool is_auth_data() const
  {
    return std::holds_alternative<PointerItem<AuthDataItem>>(type_);
  }
  bool is_replay_cache() const
  {
    return std::holds_alternative<ReplayCacheIt>(type_);
  }

  template<typename T> auto const *get_inner() const
  {
    return std::get_if<T>(&type_);
  }

  // narrow combines the reservation of a change into this one.
  void narrow(const ItemChange &change);

  auto get_type() const { return type_.index(); }

private:

  std::variant<std::monostate, NNInt128, Bytes, Deletion, DeployedContractData,
               NewContractData, PointerItem<AuthDataItem>, ReplayCacheIt>
      type_;
  bool exclusive_{false};
  bool permanent_exclusive_{false};
  bool change_committed_{false};

  friend struct std::formatter<ItemChange>;
};

// Performance can degrade if an ItemChange becomes too large.
// This is not a hard limit; the current size is 96.  This is a check
// so that we do not accidentally increase the size.
// The biggest type in the ItemChange is NNInt128, which is 64 bytes.
static_assert(sizeof(ItemChange) <= 128);

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
  if (const auto *tmp = std::get_if<PointerItem<AuthDataItem>>(&type_); tmp)
    return *(tmp->get());
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

inline std::optional<NewContractData>
ItemChange::to_new_contract() const
{
  if (const auto *tmp = std::get_if<NewContractData>(&type_); tmp)
    return *tmp;
  return std::nullopt;
}

inline std::optional<DeployedContractData>
ItemChange::to_new_deployment() const
{
  if (const auto *tmp = std::get_if<DeployedContractData>(&type_); tmp)
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
  return HogDBStatus<ItemChange>(std::in_place_t{}, std::move(out));
}

} // namespace hogd

template<> struct std::formatter<hogd::ItemChange> {
  static constexpr auto parse(format_parse_context &ctx) { return ctx.begin(); }
  static auto format(const hogd::ItemChange &item_change, auto &ctx)
  {
    std::visit(
        [&ctx, &item_change](auto &&arg) {
          if constexpr (!std::same_as<std::decay_t<decltype(arg)>,
                                      std::monostate>) {
            format_to(ctx.out(), "Exclusive(Permanent): {}({}), {}",
                      item_change.exclusive_, item_change.permanent_exclusive_,
                      arg);
          }
          else {
            format_to(ctx.out(), "monostate");
          }
        },
        item_change.type_);
    return ctx.out();
  }
};
