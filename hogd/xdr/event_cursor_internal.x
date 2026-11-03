// clang-format off

%#include "pagination.hh"
%#include "types.hh"

namespace hogd
{
// clang-format on

// Internal schema for the cursor bytes stored in the wire-opaque
// PaginationCursor (see protocol/pagination.x).  This schema is
// hogd-internal: clients never see this type and never decode the
// cursor bytes — they treat the cursor as an opaque blob.  The
// schema lives here, NOT in protocol/, precisely to keep that
// invariant.
//
// Bump `schema_version` any time fields change shape; consumers must
// reject cursors with an unknown version.
enum HistoryCursorMethod {
  CursorEvents = 0,
  CursorTransactions = 1,
  CursorAddressTransactions = 2
};

struct EventCursorPayload {
  unsigned int schema_version;
  HistoryCursorMethod method;
  SHA256Hash query_hash;
  IterationOrder order;
  BlockHeight block_height;
  SHA256Hash tx_hash;
  unsigned int sequence_number;
  BlockHeight max_block_height;
  // Address history binds its walk to the durable outbox origin. Other
  // history methods leave this zero until they adopt the same epoch contract.
  SHA256Hash epoch_hash;
};

} // namespace hogd
