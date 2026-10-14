# Indexer address-tx index rework Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the broken, unindexed `TransactionQuery.address` filter with a properly indexed `get_transactions_by_address` RPC (wallet history), by folding a root-invoked-address row into the existing contract index.

**Architecture:** The `contract_index` column family becomes an address-activity index: its key gains a 1-byte record-kind discriminator (`EVENT` vs `ROOT_TX`) placed right after the address, so each `(address, kind)` forms a contiguous block-ordered range walked by the existing `ContractIter`. Ingest writes one `ROOT_TX` row per transaction keyed by `invocation.invoked_address`. The new query prefix-scans `address ‖ ROOT_TX` and returns lightweight `{tx_hash, height}` refs; the client hydrates via the existing `get_transaction_info`.

**Tech Stack:** C++23, RocksDB, GoogleTest, the `x23` XDR framework (`.x` schemas compiled to `.hh` by `x23c` at build time).

## Global Constraints

- **No backwards compatibility / no migration.** There is no deployment; key formats, column-family names, and protocol structs may change freely. No existing indexer DB must be preserved.
- **Schema regen is automatic.** Editing `protocol/indexer.x` and running `cmake --build ./build -j16` reruns `x23c` and regenerates `build/hogd/xdr/indexer.hh`. Never hand-edit generated `.hh` files.
- **The handler contract is compile-enforced.** `static_assert(is_IndexerBlockExplorer1<IndexerBlockExplorerRpc *, x23::Async>)` at `hogd/indexer/rpc_from_block_explorer.h:69` fails to compile until every RPC method declared in the `.x` version block has a matching C++ handler.
- **Address index rows carry no value.** All hydration reads `tx_index`; index rows use empty values (existing convention).
- **Build dir is `./build`** in commands below; adjust if your tree uses another (e.g. `~/build/hogd-debug`). Tests run via `ctest --test-dir ./build`.

---

## File Structure

- `protocol/indexer.x` — remove `TransactionQuery.address`; add `TransactionRef`, `TransactionRefBatch`, `AddressTransactionQuery`, `TransactionRefPage`, and the `get_transactions_by_address = 5` RPC method.
- `hogd/indexer/key_format.h` / `key_format.cc` — add `RecordKind`; grow the contract key by 1 byte; thread `kind` through `make_contract_key`, `make_contract_key_prefix`, `parse_contract_key`.
- `hogd/indexer/key_format_test.cc` — roundtrip tests for the kind byte.
- `hogd/indexer/db.cc` — thread `kind` through `ContractIter` and `create_contract_index_sst_chunk`; emit `ROOT_TX` rows at ingest; drop the address filter in `get_transactions`; add `IndexDB::get_transactions_by_address`.
- `hogd/indexer/db.h` — declare `get_transactions_by_address`.
- `hogd/indexer/rpc_from_block_explorer.h` — add the `get_transactions_by_address` handler.
- `hogd/indexer/indexer_test.cc` — new tests: #2842 regression, event/root separation, pagination.

---

## Task 1: Add the record-kind discriminator to the address index (no behavior change)

Thread a `kind` byte through the key format and every call site, keying all existing event rows as `EVENT`. No new rows, no new query yet — the build stays green and existing event queries behave identically.

**Files:**
- Modify: `hogd/indexer/key_format.h`, `hogd/indexer/key_format.cc`
- Modify: `hogd/indexer/db.cc` (`ContractIter`, `create_contract_index_sst_chunk`, `get_events` iterator construction)
- Test: `hogd/indexer/key_format_test.cc`

**Interfaces:**
- Produces: `IndexerKeyFormat::RecordKind { kEvent = 0, kRootTx = 1 }`.
- Produces: `make_contract_key(key, address, kind, block, tx_hash, seq)`, `make_contract_key_prefix(key, address, kind, block)`, `parse_contract_key(key, address*, kind*, block*, tx_hash*, seq*)`.
- Produces: `ContractIter(db, cf, addr, order, max_block, kind)`.

- [ ] **Step 1: Write the failing test** — append to `hogd/indexer/key_format_test.cc` (inside the existing `namespace hogd { ... }`):

```cpp
TEST(KeyFormatTest, ContractKeyRoundTripsRecordKind)
{
  ContractAddress addr;
  std::ranges::fill(addr, 0x5A);
  SHA256Hash tx_hash;
  std::ranges::fill(tx_hash, 0x7B);
  const BlockHeight block = 4242;
  const uint32_t seq = 9;

  for (IndexerKeyFormat::RecordKind kind :
       {IndexerKeyFormat::kEvent, IndexerKeyFormat::kRootTx}) {
    std::string key;
    IndexerKeyFormat::make_contract_key(key, addr, kind, block, tx_hash, seq);
    EXPECT_EQ(key.size(), IndexerKeyFormat::kContractKeySize);

    ContractAddress addr_out;
    IndexerKeyFormat::RecordKind kind_out;
    BlockHeight block_out;
    SHA256Hash tx_out;
    uint32_t seq_out;
    ASSERT_TRUE(IndexerKeyFormat::parse_contract_key(
        key, &addr_out, &kind_out, &block_out, &tx_out, &seq_out));
    EXPECT_EQ(addr_out, addr);
    EXPECT_EQ(kind_out, kind);
    EXPECT_EQ(block_out, block);
    EXPECT_EQ(tx_out, tx_hash);
    EXPECT_EQ(seq_out, seq);
  }

  // The kind byte sits right after the address, so EVENT and ROOT_TX rows
  // for the same address sort into disjoint contiguous ranges.
  std::string ev_key, rt_key;
  IndexerKeyFormat::make_contract_key(ev_key, addr, IndexerKeyFormat::kEvent,
                                      block, tx_hash, seq);
  IndexerKeyFormat::make_contract_key(rt_key, addr, IndexerKeyFormat::kRootTx,
                                      block, tx_hash, seq);
  EXPECT_LT(ev_key, rt_key);
}
```

- [ ] **Step 2: Run it to confirm it fails to compile** (the `kind` parameter and `RecordKind` don't exist yet)

Run: `cmake --build ./build -j16 --target indexer_test`
Expected: FAIL — `RecordKind` not a member of `IndexerKeyFormat`; `make_contract_key`/`parse_contract_key` arity mismatch.

- [ ] **Step 3: Add `RecordKind` and grow the key in `key_format.h`**

Replace the `kContractKeySize` / `kContractKeyPrefixSize` constants and the three declarations:

```cpp
  // Record kind discriminator, stored as 1 byte immediately after the
  // address so each (address, kind) forms a contiguous key range.
  enum RecordKind : uint8_t {
    kEvent = 0,  // one row per emitted event, keyed by emitter address
    kRootTx = 1, // one row per tx, keyed by root invoked_address (seq = 0)
  };

  // Contract index key format:
  //   contract_address (32) + kind (1) + block_height (8 BE) +
  //   tx_hash (32) + sequence_number (4 BE)
  // Total: 77 bytes
  static constexpr size_t kContractKeySize = 32 + 1 + 8 + 32 + 4;

  // Contract index key prefix (for range queries):
  //   contract_address (32) + kind (1) + block_height (8 BE)
  // Total: 41 bytes
  static constexpr size_t kContractKeyPrefixSize = 32 + 1 + 8;

  static void make_tx_key(std::string &key, SHA256Hash const &tx_hash);

  static void make_contract_key_prefix(std::string &key,
                                       ContractAddress const &address,
                                       RecordKind kind,
                                       BlockHeight block_height);

  static void make_contract_key(std::string &key,
                                ContractAddress const &address,
                                RecordKind kind, BlockHeight block_height,
                                SHA256Hash const &tx_hash,
                                uint32_t sequence_number);

  static bool parse_contract_key(std::string_view key, ContractAddress *address,
                                 RecordKind *kind, BlockHeight *block_height,
                                 SHA256Hash *tx_hash, uint32_t *sequence_number);
```

- [ ] **Step 4: Implement the kind byte in `key_format.cc`**

Replace `make_contract_key_prefix`, `make_contract_key`, and `parse_contract_key`:

```cpp
void
IndexerKeyFormat::make_contract_key_prefix(std::string &key,
                                           ContractAddress const &address,
                                           RecordKind kind,
                                           BlockHeight block_height)
{
  key.clear();
  key.reserve(kContractKeyPrefixSize);
  key.append(reinterpret_cast<const char *>(address.data()), 32);
  key.push_back(static_cast<char>(kind));
  uint64_t height_be = std::byteswap(static_cast<uint64_t>(block_height));
  key.append(reinterpret_cast<const char *>(&height_be), 8);
}

void
IndexerKeyFormat::make_contract_key(std::string &key,
                                    ContractAddress const &address,
                                    RecordKind kind, BlockHeight block_height,
                                    SHA256Hash const &tx_hash,
                                    uint32_t sequence_number)
{
  key.clear();
  key.reserve(kContractKeySize);
  key.append(reinterpret_cast<const char *>(address.data()), 32);
  key.push_back(static_cast<char>(kind));
  uint64_t height_be = std::byteswap(static_cast<uint64_t>(block_height));
  key.append(reinterpret_cast<const char *>(&height_be), 8);
  key.append(reinterpret_cast<const char *>(tx_hash.data()), 32);
  uint32_t seq_be = std::byteswap(sequence_number);
  key.append(reinterpret_cast<const char *>(&seq_be), 4);
}

bool
IndexerKeyFormat::parse_contract_key(std::string_view key,
                                     ContractAddress *address, RecordKind *kind,
                                     BlockHeight *block_height,
                                     SHA256Hash *tx_hash,
                                     uint32_t *sequence_number)
{
  if (key.size() != kContractKeySize) {
    return false;
  }

  size_t offset = 0;
  constexpr size_t kAddressLen = 32;
  constexpr size_t kKindLen = 1;
  constexpr size_t kHeightLen = 8;
  constexpr size_t kHashLen = 32;
  constexpr size_t kSeqLen = 4;

  if (address != nullptr) {
    std::memcpy(address->data(), key.data() + offset, kAddressLen);
  }
  offset += kAddressLen;

  if (kind != nullptr) {
    *kind = static_cast<RecordKind>(static_cast<uint8_t>(key[offset]));
  }
  offset += kKindLen;

  uint64_t height_be;
  std::memcpy(&height_be, key.data() + offset, kHeightLen);
  if (block_height != nullptr) {
    *block_height = std::byteswap(height_be);
  }
  offset += kHeightLen;

  if (tx_hash != nullptr) {
    std::memcpy(tx_hash->data(), key.data() + offset, kHashLen);
  }
  offset += kHashLen;

  uint32_t seq_be;
  std::memcpy(&seq_be, key.data() + offset, kSeqLen);
  if (sequence_number != nullptr) {
    *sequence_number = std::byteswap(seq_be);
  }

  return true;
}
```

- [ ] **Step 5: Thread `kind` through `ContractIter` in `db.cc`**

In the `ContractIter` constructor (around `db.cc:1021`), add a `kind` parameter and precompute the 33-byte `address ‖ kind` boundary prefix:

```cpp
  ContractIter(rocksdb::DB *db, rocksdb::ColumnFamilyHandle *cf,
               ContractAddress addr, IterationOrder order,
               BlockHeight max_block, IndexerKeyFormat::RecordKind kind)
    : contract_(addr), order_(order), max_block_(max_block), kind_(kind),
      iter_(db->NewIterator(rocksdb::ReadOptions(), cf))
  {
    // Boundary prefix: address(32) + kind(1).  refresh_current() stops the
    // walk when a key leaves this range, so an EVENT iterator never bleeds
    // into ROOT_TX rows for the same address (and vice versa).
    kind_prefix_.append(reinterpret_cast<const char *>(contract_.data()), 32);
    kind_prefix_.push_back(static_cast<char>(kind_));
  }
```

In `seek()` (around `db.cc:1043`), pass `kind_` to the prefix builder:

```cpp
    IndexerKeyFormat::make_contract_key_prefix(prefix, contract_, kind_, anchor);
```

In `refresh_current()` (around `db.cc:1112`), replace the address-only boundary check and the `parse_contract_key` call:

```cpp
    rocksdb::Slice key = iter_->key();
    const std::string_view key_sv(key.data(), key.size());
    if (key_sv.size() < IndexerKeyFormat::kContractKeyPrefixSize ||
        !key_sv.starts_with(kind_prefix_)) {
      has_current_ = false;
      return;
    }
    ContractAddress addr_out;
    if (!IndexerKeyFormat::parse_contract_key(key_sv, &addr_out, nullptr,
                                              &current_.block, &current_.tx,
                                              &current_.seq)) {
      has_current_ = false;
      return;
    }
```

Add the two new members alongside the existing ones (near `db.cc:1150`):

```cpp
  IndexerKeyFormat::RecordKind kind_;
  std::string kind_prefix_;
```

- [ ] **Step 6: Key existing event rows as `kEvent` in `create_contract_index_sst_chunk`**

In `create_contract_index_sst_chunk` (around `db.cc:172`), pass the kind:

```cpp
      IndexerKeyFormat::make_contract_key(key_buffer,
                                          emitted_event.contract_address,
                                          IndexerKeyFormat::kEvent,
                                          block_height,
                                          event_log.proposed_tx_hash,
                                          sequence_number);
```

- [ ] **Step 7: Construct `get_events` iterators with `kEvent`**

In `get_events` (around `db.cc:1268`), pass the kind to the `emplace_back`:

```cpp
        iters.emplace_back(db_.get(), contract_cf_handle_, addr, order,
                           max_serve_block, IndexerKeyFormat::kEvent);
```

- [ ] **Step 8: Build and run the full indexer suite**

Run: `cmake --build ./build -j16 --target indexer_test && ctest --test-dir ./build --output-on-failure -R "KeyFormatTest|IndexerTest"`
Expected: PASS — the new roundtrip test passes and all existing event/tx tests are unaffected (event rows just gained a kind byte).

- [ ] **Step 9: Commit**

```bash
git add hogd/indexer/key_format.h hogd/indexer/key_format.cc hogd/indexer/key_format_test.cc hogd/indexer/db.cc
git commit -m "indexer: add record-kind byte to the address index key"
```

---

## Task 2: Drop the broken address filter and remove `TransactionQuery.address`

Delete the eventless-miss filter (#2842) and the schema field it read. After this task, `get_transactions` is purely the whole-chain walk.

**Files:**
- Modify: `protocol/indexer.x` (submodule)
- Modify: `hogd/indexer/db.cc` (`get_transactions`)
- Test: `hogd/indexer/indexer_test.cc`

**Interfaces:**
- Consumes: nothing new.
- Produces: `TransactionQuery` with no `address` field.

- [ ] **Step 1: Write the failing test** — append to `hogd/indexer/indexer_test.cc` (inside `namespace hogd`). This pins the whole-chain query to return every ingested tx regardless of address, and — once the field is gone — will fail to compile if anything still sets `query.address`:

```cpp
TEST_F(IndexerTest, GetTransactionsReturnsAllRegardlessOfAddress)
{
  BlockHeight block_height = 100;
  TxEventLogBatch events = create_event_logs();          // 2 txs, addrs 0xAA/0xBB
  ProposedTransactionBatch bodies = create_tx_bodies(events.size());
  ingest_legacy_events(block_height, events, bodies)();

  TransactionQuery query{};
  query.order = IterationOrder::IterationOrderAscending;
  query.anchor.kind(BlockAnchorKind::AnchorAtBeginning);
  query.limit = 0;

  TransactionPage page = block_explorer_->get_transactions(query)();
  EXPECT_EQ(page.transactions.size(), 2);
}
```

- [ ] **Step 2: Run it to verify it passes today** (the filter is absent from this query)

Run: `cmake --build ./build -j16 --target indexer_test && ctest --test-dir ./build --output-on-failure -R "IndexerTest.GetTransactionsReturnsAllRegardlessOfAddress"`
Expected: PASS (this is a guard we keep green through the edit).

- [ ] **Step 3: Remove the `address` field from `TransactionQuery` in `protocol/indexer.x`**

Replace the struct (lines ~113-132) so it no longer has an address filter:

```
/// `get_transactions` request.  Whole-chain walk in `order` from `anchor`.
/// There is no address filter here — use `get_transactions_by_address` for
/// per-account (root invoked address) transaction history.
struct TransactionQuery {
  IterationOrder order;
  BlockAnchor anchor;

  /// Empty on the initial call.  Non-empty to continue where a previous
  /// response left off; `order` and `anchor` in the request are ignored
  /// when a cursor is supplied — the cursor carries them.
  PaginationCursor cursor;

  /// Clamped server-side to [1, 200]; 0 means server default (50).
  unsigned int limit;
};
```

Also update the `get_transactions` RPC comment (line ~178) from "optionally filtered to a single address" to "whole-chain, in block order."

- [ ] **Step 4: Delete the filter block in `get_transactions` (`db.cc`)**

Remove the entire `if (query.address) { ... }` block (around `db.cc:1531-1542`):

```cpp
    if (query.address) {
      const bool matched =
          std::ranges::any_of(record.events.events,
                              [&](TxEmittedEvent const &ev) {
                                return ev.contract_address == *query.address;
                              });
      if (!matched) {
        continue;
      }
    }
```

- [ ] **Step 5: Build (regenerates `indexer.hh`) and run**

Run: `cmake --build ./build -j16 --target indexer_test && ctest --test-dir ./build --output-on-failure -R "IndexerTest"`
Expected: PASS. If the build errors on a stray `query.address`, that reference must be removed too (repo-wide check already showed only the filter block used it).

- [ ] **Step 6: Commit**

```bash
git add protocol hogd/indexer/db.cc hogd/indexer/indexer_test.cc
git commit -m "indexer: drop broken address filter from get_transactions (#2842)"
```

Note: `git add protocol` stages the submodule pointer bump. Commit and push inside `protocol/` separately per the submodule workflow when ready to share.

---

## Task 3: Add `get_transactions_by_address` (schema + ingest ROOT_TX rows + query + handler)

The feature: index one `ROOT_TX` row per transaction keyed by its root invoked address, and serve it as a paginated refs query. This is the #2842 fix done right.

**Files:**
- Modify: `protocol/indexer.x` (submodule)
- Modify: `hogd/indexer/db.cc` (`create_contract_index_sst_chunk`, `consume_events`, new `get_transactions_by_address`)
- Modify: `hogd/indexer/db.h`
- Modify: `hogd/indexer/rpc_from_block_explorer.h`
- Test: `hogd/indexer/indexer_test.cc`

**Interfaces:**
- Consumes: `IndexerKeyFormat::kRootTx`, `ContractIter(..., kind)` from Task 1.
- Produces: XDR `TransactionRef { SignedTransactionHash tx_hash; BlockHeight height; }`, `AddressTransactionQuery`, `TransactionRefPage { TransactionRefBatch refs; PaginationCursor cursor; BlockHeight latest_block; BlockHeight oldest_block; }`.
- Produces: `IndexerStatus<TransactionRefPage> IndexDB::get_transactions_by_address(AddressTransactionQuery const &) const`.
- Produces: `x23::Async<TransactionRefPage> IndexerBlockExplorerRpc::get_transactions_by_address(AddressTransactionQuery)`.

- [ ] **Step 1: Add the schema types and RPC method to `protocol/indexer.x`**

After the `TransactionPage` struct (around line 157), add:

```
/// Lightweight reference to a transaction: its hash plus the block it
/// executed in.  Returned by `get_transactions_by_address`; hydrate the
/// full body via `get_transaction_info`.
struct TransactionRef {
  SignedTransactionHash tx_hash;
  BlockHeight height;
};

typedef TransactionRef TransactionRefBatch<>;

/// `get_transactions_by_address` request.  Walked in `order` from `anchor`,
/// returning transactions whose ROOT invocation targets `address` (i.e. the
/// account's transaction history).
struct AddressTransactionQuery {
  ContractAddress address;
  IterationOrder order;
  BlockAnchor anchor;
  /// Empty on the initial call.  Non-empty to continue; `order`/`anchor`
  /// are ignored when a cursor is supplied.
  PaginationCursor cursor;
  /// Clamped server-side to [1, 200]; 0 means server default (50).
  unsigned int limit;
};

/// `get_transactions_by_address` response: transaction refs in `order`.
struct TransactionRefPage {
  TransactionRefBatch refs;
  PaginationCursor cursor;
  BlockHeight latest_block;
  BlockHeight oldest_block;
};
```

Add the method to the `IndexerBlockExplorer1` version block (after `get_transactions ... = 4;`, line ~179):

```
    // Paginated transaction refs whose root invoked address == the query
    // address (account transaction history).  Hydrate via get_transaction_info.
    TransactionRefPage get_transactions_by_address(AddressTransactionQuery) = 5;
```

- [ ] **Step 2: Write the failing #2842 regression test** — append to `hogd/indexer/indexer_test.cc`. It ingests an eventless root invocation, then proves the address query surfaces it and hydration works:

```cpp
TEST_F(IndexerTest, AddressQueryFindsEventlessRootInvocation)
{
  BlockHeight block_height = 101;

  ContractAddress root_addr;
  std::ranges::fill(root_addr, 0xCC);

  ProposedTransaction body;
  body.signed_tx.tx.uniquifier.nonce = 42;
  body.signed_tx.tx.invocation.invoked_address = root_addr;

  SHA256Hash tx_hash;
  std::ranges::fill(tx_hash, 0x33);

  TxEventLog event_log;           // no events emitted
  event_log.proposed_tx_hash = tx_hash;
  ASSERT_TRUE(event_log.events.empty());

  ingest_legacy_events(block_height, TxEventLogBatch{event_log},
                                       ProposedTransactionBatch{body})();

  AddressTransactionQuery query{};
  query.address = root_addr;
  query.order = IterationOrder::IterationOrderAscending;
  query.anchor.kind(BlockAnchorKind::AnchorAtBeginning);
  query.limit = 0;

  TransactionRefPage page = block_explorer_->get_transactions_by_address(query)();
  ASSERT_EQ(page.refs.size(), 1);
  EXPECT_EQ(page.refs[0].tx_hash, tx_hash);
  EXPECT_EQ(page.refs[0].height, block_height);

  // The ref hydrates to the full body via the existing by-hash API.
  TxInfoResult info = block_explorer_->get_transaction_info(page.refs[0].tx_hash)();
  ASSERT_EQ(info.outcome(), TxInfoOutcome::TxInfoFound);
  EXPECT_EQ(info.info().tx.signed_tx.tx.invocation.invoked_address, root_addr);
}
```

- [ ] **Step 3: Run to confirm it fails to compile** (`get_transactions_by_address` handler + DB method don't exist)

Run: `cmake --build ./build -j16 --target indexer_test`
Expected: FAIL — `is_IndexerBlockExplorer1` static_assert fails; `get_transactions_by_address` undefined.

- [ ] **Step 4: Emit `ROOT_TX` rows in `create_contract_index_sst_chunk` (`db.cc`)**

Change the signature to accept the bodies span, and rewrite the body. Signature (around `db.cc:146`):

```cpp
create_contract_index_sst_chunk(
    rocksdb::DB * /*db*/, rocksdb::ColumnFamilyHandle *contract_cf_handle,
    BlockHeight block_height, std::span<const TxEventLog> events_chunk,
    std::span<const ProposedTransaction> bodies_chunk,
    const std::string &sst_path, const rocksdb::Options &sst_options,
    const rocksdb::EnvOptions &env_options, size_t chunk_idx)
```

Replace the early-skip and the entry-collection loop:

```cpp
  // Count total events; reserve space for events + one ROOT_TX row per body.
  size_t total_events = 0;
  for (const auto &event_log : events_chunk) {
    total_events += event_log.events.size();
  }

  // Skip only when this chunk would produce no rows at all.
  if (total_events == 0 && bodies_chunk.empty()) {
    return false;
  }

  std::vector<std::pair<std::string, std::string>> entries;
  entries.reserve(total_events + bodies_chunk.size());
  std::string key_buffer;
  const std::string empty_string;

  for (size_t i = 0; i < events_chunk.size(); i++) {
    const TxEventLog &event_log = events_chunk[i];

    uint32_t sequence_number = 0;
    for (const auto &emitted_event : event_log.events) {
      IndexerKeyFormat::make_contract_key(key_buffer,
                                          emitted_event.contract_address,
                                          IndexerKeyFormat::kEvent,
                                          block_height,
                                          event_log.proposed_tx_hash,
                                          sequence_number);
      entries.emplace_back(key_buffer, empty_string);
      sequence_number++;
    }

    // One ROOT_TX row keyed by the tx's root invoked address (seq = 0).
    // Bodies are index-aligned with events; callers that pass no bodies
    // (event-log-only tests) simply get no ROOT_TX rows.
    if (i < bodies_chunk.size()) {
      const ContractAddress &root_addr =
          bodies_chunk[i].signed_tx.tx.invocation.invoked_address;
      IndexerKeyFormat::make_contract_key(key_buffer, root_addr,
                                          IndexerKeyFormat::kRootTx,
                                          block_height,
                                          event_log.proposed_tx_hash, 0);
      entries.emplace_back(key_buffer, empty_string);
    }
  }
```

- [ ] **Step 5: Pass `bodies_chunk` from `consume_events` (`db.cc`)**

In the contract-index chunk loop (around `db.cc:637-670`), compute the aligned bodies subspan (mirroring the tx-index loop) and capture it in the lambda:

```cpp
    const size_t end_idx =
        std::min(start_idx + contract_events_chunk_size, events.size());
    const auto events_chunk = events.subspan(start_idx, end_idx - start_idx);
    const auto bodies_chunk =
        start_idx < bodies.size()
            ? bodies.subspan(start_idx,
                             std::min(end_idx, bodies.size()) - start_idx)
            : std::span<const ProposedTransaction>{};
    const std::string &chunk_sst_path = contract_sst_paths[i];

    auto *db_ptr = db_.get();
    CHECK_PTR(db_ptr);

    auto future = pool.submit([db = db_ptr, contract_cf = contract_cf_handle_,
                               block_height, events_chunk, bodies_chunk,
                               &chunk_sst_path, &contract_sst_options,
                               &env_options, i]() -> IndexerStatus<bool> {
      try {
        return create_contract_index_sst_chunk(
            db, contract_cf, block_height, events_chunk, bodies_chunk,
            chunk_sst_path, contract_sst_options, env_options, i);
      } catch (...) {
        return IndexerError(IndexerErrorCode::kInternal);
      }
    });
```

- [ ] **Step 6: Declare the query in `db.h`**

After the `get_transactions` declaration (around `db.h:123`):

```cpp
  // Paginated account transaction history: refs to transactions whose ROOT
  // invocation targets `query.address`, in block order.  Reads refs directly
  // off the address index (kind = ROOT_TX); no tx_index hydration.  Clients
  // hydrate full bodies via get_transaction_info.
  IndexerStatus<TransactionRefPage>
  get_transactions_by_address(AddressTransactionQuery const &query) const;
```

- [ ] **Step 7: Implement `get_transactions_by_address` in `db.cc`**

Add after `get_transactions` (near `db.cc:1590`). It reuses the anchor/cursor decode from `get_transactions` and a single `ContractIter` at `kRootTx`:

```cpp
IndexerStatus<TransactionRefPage>
IndexDB::get_transactions_by_address(AddressTransactionQuery const &query) const
{
  if (!is_open()) {
    return IndexerError(IndexerErrorCode::kDBNotOpen);
  }

  TransactionRefPage page;
  const BlockHeight max_serve_block = latest_block();
  page.latest_block = max_serve_block;
  page.oldest_block = oldest_block();

  const uint32_t limit = IndexDB::clamp_events_limit(query.limit);

  IterationOrder order;
  BlockHeight anchor_block;
  bool have_cursor = false;
  Position cursor_pos{};

  if (!query.cursor.empty()) {
    auto decoded = cursor::decode(
        std::string_view(reinterpret_cast<const char *>(query.cursor.data()),
                         query.cursor.size()));
    if (!decoded || !is_valid_iteration_order(decoded->order)) {
      return page;
    }
    order = decoded->order;
    cursor_pos.block = decoded->block_height;
    cursor_pos.tx = decoded->tx_hash;
    cursor_pos.seq = decoded->sequence_number;
    anchor_block = cursor_pos.block;
    have_cursor = true;
  }
  else {
    order = query.order;
    if (!is_valid_iteration_order(order)) {
      return page;
    }
    switch (query.anchor.kind()) {
    case BlockAnchorKind::AnchorAtBeginning:
      if (order == IterationOrder::IterationOrderDescending) {
        return page;
      }
      anchor_block = oldest_block();
      break;
    case BlockAnchorKind::AnchorAtEnd:
      if (order == IterationOrder::IterationOrderAscending) {
        return page;
      }
      anchor_block = max_serve_block;
      break;
    case BlockAnchorKind::AnchorAtHeight:
      anchor_block = query.anchor.height();
      if (anchor_block < oldest_block()) {
        if (order == IterationOrder::IterationOrderDescending) {
          return page;
        }
        anchor_block = oldest_block();
      }
      break;
    }
  }

  if (order == IterationOrder::IterationOrderDescending &&
      anchor_block > max_serve_block) {
    anchor_block = max_serve_block;
  }

  ContractIter iter(db_.get(), contract_cf_handle_, query.address, order,
                    max_serve_block, IndexerKeyFormat::kRootTx);
  iter.seek(anchor_block, order);
  if (have_cursor && iter.has_current()) {
    iter.skip_past(order, cursor_pos);
  }

  Position last_emitted{};
  bool any_emitted = false;
  while (page.refs.size() < limit && iter.has_current()) {
    Position const pos = iter.current();
    TransactionRef ref;
    ref.tx_hash = pos.tx;
    ref.height = pos.block;
    page.refs.push_back(ref);
    last_emitted = pos;
    any_emitted = true;
    iter.advance(order);
  }

  // More rows remain past the page — emit a continuation cursor.
  if (any_emitted && iter.has_current()) {
    EventCursorPayload payload;
    payload.order = order;
    payload.block_height = last_emitted.block;
    payload.tx_hash = last_emitted.tx;
    payload.sequence_number = 0;
    std::string encoded = cursor::encode(payload);
    page.cursor.append_range(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t *>(encoded.data()), encoded.size()));
  }

  return page;
}
```

- [ ] **Step 8: Add the RPC handler in `rpc_from_block_explorer.h`**

After the `get_transactions` handler (around line 64):

```cpp
  x23::Async<TransactionRefPage>
  get_transactions_by_address(AddressTransactionQuery query)
  {
    auto status = db_.get_transactions_by_address(query);
    if (!status) {
      co_return TransactionRefPage{};
    }
    co_return std::move(*status);
  }
```

- [ ] **Step 9: Build and run the regression test**

Run: `cmake --build ./build -j16 --target indexer_test && ctest --test-dir ./build --output-on-failure -R "IndexerTest.AddressQueryFindsEventlessRootInvocation"`
Expected: PASS.

- [ ] **Step 10: Add the event/root separation test** — append to `indexer_test.cc`. A tx rooted at `A` that emits an event from `B` must appear in `get_transactions_by_address(A)` and `get_events(B)`, but NOT in `get_transactions_by_address(B)`:

```cpp
TEST_F(IndexerTest, AddressQuerySeparatesRootFromEmitter)
{
  BlockHeight block_height = 200;

  ContractAddress root_a, emitter_b;
  std::ranges::fill(root_a, 0xA1);
  std::ranges::fill(emitter_b, 0xB2);

  ProposedTransaction body;
  body.signed_tx.tx.invocation.invoked_address = root_a;
  SHA256Hash tx_hash;
  std::ranges::fill(tx_hash, 0x44);

  TxEventLog event_log;
  event_log.proposed_tx_hash = tx_hash;
  TxEmittedEvent ev;
  ev.contract_address = emitter_b;   // event emitted by B, not the root A
  ev.num_topics = 0;
  ev.serialized_event.resize(4);
  event_log.events.push_back(ev);

  ingest_legacy_events(block_height, TxEventLogBatch{event_log},
                                       ProposedTransactionBatch{body})();

  auto query_for = [&](ContractAddress const &addr) {
    AddressTransactionQuery q{};
    q.address = addr;
    q.order = IterationOrder::IterationOrderAscending;
    q.anchor.kind(BlockAnchorKind::AnchorAtBeginning);
    q.limit = 0;
    return block_explorer_->get_transactions_by_address(q)();
  };

  // Rooted at A → in A's history.
  TransactionRefPage a_page = query_for(root_a);
  ASSERT_EQ(a_page.refs.size(), 1);
  EXPECT_EQ(a_page.refs[0].tx_hash, tx_hash);

  // B only emitted an event → NOT in B's transaction history.
  TransactionRefPage b_page = query_for(emitter_b);
  EXPECT_TRUE(b_page.refs.empty());

  // But B's event is still found via the event index.
  EventQuery eq{};
  EventFilter filter{};
  filter.contract_addresses.push_back(emitter_b);
  eq.filters.push_back(filter);
  eq.order = IterationOrder::IterationOrderAscending;
  eq.anchor.kind(BlockAnchorKind::AnchorAtBeginning);
  eq.limit = 0;
  EventPage ep = block_explorer_->get_events(eq)();
  EXPECT_EQ(ep.events.size(), 1);
}
```

- [ ] **Step 11: Add the pagination test** — append to `indexer_test.cc`. Ingest three txs rooted at the same address across three blocks; walk them one per page via the cursor:

```cpp
TEST_F(IndexerTest, AddressQueryPaginatesInBlockOrder)
{
  ContractAddress addr;
  std::ranges::fill(addr, 0xD4);

  std::vector<SHA256Hash> hashes;
  for (uint8_t i = 0; i < 3; i++) {
    ProposedTransaction body;
    body.signed_tx.tx.uniquifier.nonce = i;
    body.signed_tx.tx.invocation.invoked_address = addr;
    SHA256Hash h;
    std::ranges::fill(h, static_cast<char>(0x50 + i));
    hashes.push_back(h);
    TxEventLog log;
    log.proposed_tx_hash = h;
    ingest_legacy_events(
        BlockHeight{static_cast<uint64_t>(300 + i)}, TxEventLogBatch{log},
        ProposedTransactionBatch{body})();
  }

  AddressTransactionQuery q{};
  q.address = addr;
  q.order = IterationOrder::IterationOrderAscending;
  q.anchor.kind(BlockAnchorKind::AnchorAtBeginning);
  q.limit = 1;

  std::vector<SHA256Hash> seen;
  for (int guard = 0; guard < 10; guard++) {
    TransactionRefPage page = block_explorer_->get_transactions_by_address(q)();
    for (auto const &r : page.refs) {
      seen.push_back(r.tx_hash);
    }
    if (page.cursor.empty()) {
      break;
    }
    q.cursor = page.cursor;
  }

  ASSERT_EQ(seen.size(), 3);
  EXPECT_EQ(seen[0], hashes[0]);   // block 300
  EXPECT_EQ(seen[1], hashes[1]);   // block 301
  EXPECT_EQ(seen[2], hashes[2]);   // block 302
}
```

- [ ] **Step 12: Build and run all the new tests**

Run: `cmake --build ./build -j16 --target indexer_test && ctest --test-dir ./build --output-on-failure -R "IndexerTest"`
Expected: PASS — all three new address-query tests plus the existing suite.

- [ ] **Step 13: Commit**

```bash
git add protocol hogd/indexer/db.h hogd/indexer/db.cc hogd/indexer/rpc_from_block_explorer.h hogd/indexer/indexer_test.cc
git commit -m "indexer: add get_transactions_by_address backed by ROOT_TX index rows (#2842)"
```

---

## Task 4 (optional, cosmetic): rename `contract_index` → `address_index`

The column family now indexes address activity (events + root transactions), not just contract event emission. This rename is purely for clarity and touches no behavior; skip it if churn is unwelcome. Do it last so nothing functional depends on it.

**Files:**
- Modify: `hogd/indexer/key_format.h` (`kContractIndexColumnFamily` → `kAddressIndexColumnFamily`, value `"address_index"`; update the scheme comment)
- Modify: `hogd/indexer/db.cc` (CF create/open by name; the SST temp filename `contract_index_{}_{}.sst`; local `contract_cf`/`contract_sst_*` names as desired)
- Modify: `hogd/indexer/db.h` (`contract_cf_handle_` member name, if renaming)

- [ ] **Step 1: Rename the constant and CF string in `key_format.h`**

```cpp
  static constexpr const char *kAddressIndexColumnFamily = "address_index";
```

Update the block comment header from "Column family: contract_index" to "Column family: address_index" and note it holds both `kEvent` and `kRootTx` rows.

- [ ] **Step 2: Update all references in `db.cc` / `db.h`**

Replace `kContractIndexColumnFamily` with `kAddressIndexColumnFamily` at the `CreateColumnFamily` call and the open-time name match. Optionally rename `contract_cf_handle_`, `contract_sst_options`, and the `contract_index_{}_{}.sst` temp filename to `address_index_*`. Leave `make_contract_key*` helper names as-is (or rename to `make_address_key*` consistently) — a mechanical, compiler-checked change.

- [ ] **Step 3: Build and run the full suite**

Run: `cmake --build ./build -j16 --target indexer_test && ctest --test-dir ./build --output-on-failure -R "KeyFormatTest|IndexerTest"`
Expected: PASS (no behavior change; fresh DB, no migration).

- [ ] **Step 4: Commit**

```bash
git add hogd/indexer/key_format.h hogd/indexer/key_format.cc hogd/indexer/db.cc hogd/indexer/db.h
git commit -m "indexer: rename contract_index CF to address_index"
```

---

## Self-Review Notes

- **Spec coverage:** RPC surface (by-hash already exists; `get_transactions` de-addressed in Task 2; `get_transactions_by_address` in Task 3) ✓; folded `kind`-discriminated index (Task 1 + Task 3) ✓; refs return shape (Task 3 `TransactionRefPage`) ✓; #2842 resolution (Task 2 removes filter, Task 3 regression test) ✓; migration none (Global Constraints) ✓; CF rename (Task 4) ✓.
- **Non-goals honored:** whole-chain `get_transactions` remains a full scan (untouched except filter removal); no source-address/auth-claim indexing.
- **Type consistency:** `RecordKind {kEvent,kRootTx}`, `make_contract_key(...,kind,...)`, `parse_contract_key(...,kind,...)`, `ContractIter(...,kind)`, `AddressTransactionQuery`, `TransactionRefPage.refs`, `TransactionRef{tx_hash,height}`, and `get_transactions_by_address` are used identically across all tasks.
