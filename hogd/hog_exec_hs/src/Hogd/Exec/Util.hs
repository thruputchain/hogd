-- | Small helpers shared across the executor.
module Hogd.Exec.Util
  ( showByteArray
  , hexEncode
  ) where

import qualified Data.ByteString as B
import qualified Data.ByteString.Char8 as BC
import qualified Data.ByteString.Base16 as B16

-- | Lower-case hex, as show_byte_array prints byte arrays.
showByteArray :: B.ByteString -> String
showByteArray = BC.unpack . B16.encode

hexEncode :: B.ByteString -> String
hexEncode = showByteArray
