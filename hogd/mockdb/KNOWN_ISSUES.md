# MockDB Known Issues

MockDB is a simplified in-memory database used as a comparison point against
HogDb for correctness testing. It will eventually be removed. The following
issues exist but will not be fixed.

## Partial batch failure leaks locks

When a subtransaction batch partially fails (e.g., first delta succeeds but
second delta's storage cost check fails), `pop_subtransactions` reverts the
subtransaction stack but does not release the `StorageItemDeltaManager` lock
acquired by the first (successful) delta. This blocks concurrent transactions
from using the same key for the rest of the block.

HogDb handles this correctly via `rollback_to_savepoint`.

- **File**: `hogd/db_interface/transaction_state.cc` (pop_subtransactions)
- **Test**: `BugHuntPartialBatchFailureLocksNotReverted` (skipped on MockDB)

## `get_db_tip` returns kRPCUnimplemented

`DbExecutorRpc::get_db_tip` returns `RPCError::kRPCUnimplemented` with a TODO.
HogDb returns actual `DbTip` data. The executor's `DatabaseProxy` maps
`kRPCUnimplemented` to `kSystemUnrecoverable`, causing a fatal error if any
contract calls `get_db_tip` on MockDB.

- **File**: `hogd/db_interface/db_executor_server.cc:~226`

## Balance deltas from different subtransaction depths are not combined

MockDB's `TransactionState::extract_results()` iterates subtransactions
newest-first and uses `emplace` (insert-if-absent). For balance keys, storage
cost adjustments from an earlier subtransaction and manual balance transfers from
a later subtransaction are independent deltas that need to be combined, but
`extract_results` silently drops the earlier one. This means storage costs locked
at one depth are lost if a balance transfer occurs at a deeper depth.

HogDb handles this correctly via `InvokeTX::balance_deltas_`, which tracks
`manual_transfer_amount` and `total_locked_storage_amount` separately and
combines them at commit time.

- **File**: `hogd/db_interface/transaction_state.cc` (extract_results)
- **See also**: `hogd/db_interface/transaction_state.h` line 27 ("THE STORAGE
  COST ACCOUNTING HERE IS BROKEN")
- **Test**: `BalanceTransferAtNewDepthPreservesLockedStorage` (skipped on MockDB)
