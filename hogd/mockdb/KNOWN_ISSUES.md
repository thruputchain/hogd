# MockDB Known Issues

MockDB is a simplified in-memory database used as a comparison point against
HogDB for correctness testing. It will eventually be removed. The following
issues exist but will not be fixed.

## Partial batch failure leaks locks

When a subtransaction batch partially fails (e.g., first delta succeeds but
second delta's storage cost check fails), `pop_subtransactions` reverts the
subtransaction stack but does not release the `StorageItemDeltaManager` lock
acquired by the first (successful) delta. This blocks concurrent transactions
from using the same key for the rest of the block.

HogDB handles this correctly via `rollback_to_savepoint`.

- **File**: `hogd/db_interface/transaction_state.cc` (pop_subtransactions)
- **Test**: `BugHuntPartialBatchFailureLocksNotReverted` (skipped on MockDB)

## `get_db_tip` returns kRPCUnimplemented

`DbExecutorRpc::get_db_tip` returns `RPCError::kRPCUnimplemented` with a TODO.
HogDB returns actual `DbTip` data. The executor's `DatabaseProxy` maps
`kRPCUnimplemented` to `kSystemUnrecoverable`, causing a fatal error if any
contract calls `get_db_tip` on MockDB.

- **File**: `hogd/db_interface/db_executor_server.cc:~226`

## Storage cost accounting does not support transfer-and-use in one transaction

When a contract receives a native token transfer and attempts to use that
balance for storage costs within the same transaction, MockDB does not account
for the incoming transfer when checking the balance. HogDB handles this
correctly.

- **Test**: `TransferAndUseStorageWithinTransaction` (skipped on MockDB)
