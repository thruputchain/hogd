-- | Event helpers (hogd/hog_event/utilities.h).
module Hogd.HogEvent.Utilities
  ( makeEventFromCxsStruct
  , computeEventSignature
  ) where

import qualified Data.ByteString as B
import Data.ByteString.Builder (Builder)
import qualified Data.ByteString.Builder as BB
import qualified Data.ByteString.Lazy as BL
import Hogd.Crypto (sha256)
import Hogd.Xdr (Topic)

-- | @make_event_from_cxs_struct@: the kLogEvent layout, a 32-byte signature
-- as topic 0 followed by the event struct in CXS.  Leading 32-byte fields
-- (addresses) serialize as raw 32-byte chunks and become the remaining
-- topics; the rest is the payload.  This is the SDK's @#[derive(Event)]@
-- wire form.
makeEventFromCxsStruct :: Topic -> Builder -> B.ByteString
makeEventFromCxsStruct signature body = BL.toStrict (BB.toLazyByteString (BB.byteString signature <> body))

-- | The SDK's event-signature topic:
-- @sha256("<major_version>::<crate_name>::<type_name>")@
-- (thruput_contract_util::event_signature).
computeEventSignature :: B.ByteString -> Topic
computeEventSignature = sha256
