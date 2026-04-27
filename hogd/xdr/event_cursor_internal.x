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
// Bump `version` any time fields change shape; consumers must
// reject cursors with an unknown version.
struct EventCursorPayload {
  unsigned int schema_version;
  IterationOrder order;
  BlockHeight block_height;
  SHA256Hash tx_hash;
  unsigned int sequence_number;
};

} // namespace hogd
