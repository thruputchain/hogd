# TODO


## Indexer: recover transaction bodies on replay

The indexer stores the full `ProposedTransaction` body alongside each
`TxEventLog` in the `tx_index` value
(`block_height || ProposedTransaction || TxEventLog`).  The body is forwarded
out of band by the event server (it is deliberately *not* part of the event
merkle trie).

The devnode recovery path (`hogd/devnode/hog_devnode_node.cc`, the
`consume_events(tip.height, events, {})` call) rebuilds the indexer by replaying
event logs read from the on-disk event log file. That file contains only event
logs, so recovery has **no transaction bodies to supply** and re-indexes those
transactions with an empty body.

We need a durable, recoverable store for transaction bodies — e.g. the planned
per-block flat-file body store — so that recovery can read the bodies back from
flat files and re-index them faithfully instead of dropping them.
