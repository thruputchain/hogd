-- | Domain-separated hashes of transaction authentication: a port of
-- @hogd/crypto/domain_separators.h@.
--
-- A domain separator is the CXS encoding of the XDR value
-- @DomainSeparator { ext = ExtensionPoint(0), domain_separator = <UTF-8 name> }@,
-- that is @0x00@ (the extension point's discriminant), the CXS-encoded name
-- length, and the name's bytes. For @"PubkeyAccount"@ that is
-- @00 0d 50 75 62 6b 65 79 41 63 63 6f 75 6e 74@.
module Hogd.Crypto.DomainSeparators
  ( makeDomainSeparator
  , domainSeparatorCxs
  , kPubkeyAuthDomainSeparatorCxs
  , kPublicKeyHashDomainSeparatorCxs
  , kTransactionSignatureDomainSeparatorCxs
  , publicKeyHash
  , makePubkeyAccountPreimage
  , pubkeyAuthAddress
  ) where

import Data.ByteString (ByteString)
import Data.Word (Word64)
import qualified Data.ByteString as B
import Hogd.Crypto (sha256HasherFinalize, sha256HasherNew, sha256HasherUpdates)
import Hogd.Crypto.MlDsa (kMlDsa65PublicKeyBytes)
import Hogd.Xdr.AuthClaim (PubkeyAccountPreimage (..))
import Hogd.Xdr.Preimage (DomainSeparator (..))
import Hogd.Xdr.Types (ContractAddress, ExtensionPoint (..), PublicKey (..), PublicKeyHash)
import X23.Xdr (cxsEncode)

-- | The DomainSeparator for a name (C++ @make_domain_separator@). The name is
-- the UTF-8 bytes of the C++ @u8""@ literal, without its terminating NUL.
makeDomainSeparator :: ByteString -> DomainSeparator
makeDomainSeparator name = DomainSeparator {ext = ExtensionPoint_0, domain_separator = name}

-- | The CXS encoding of the DomainSeparator for a name
-- (C++ @domain_separator_cxs<Name>()@).
domainSeparatorCxs :: ByteString -> ByteString
domainSeparatorCxs name
  | B.length name > 256 = error "domainSeparatorCxs: name longer than DomainSeparator allows"
  | otherwise = cxsEncode (makeDomainSeparator name)

-- | @cxs(DomainSeparator("PubkeyAccount"))@: names a standalone-pubkey account.
kPubkeyAuthDomainSeparatorCxs :: ByteString
kPubkeyAuthDomainSeparatorCxs = domainSeparatorCxs "PubkeyAccount"

-- | @cxs(DomainSeparator("PublicKey"))@: hashes a public key into its
-- PublicKeyHash.
kPublicKeyHashDomainSeparatorCxs :: ByteString
kPublicKeyHashDomainSeparatorCxs = domainSeparatorCxs "PublicKey"

-- | @cxs(DomainSeparator("Transaction"))@: prefixes the transaction signature
-- preimage (@hogd/auth_claim/signature_preimage.h@).
kTransactionSignatureDomainSeparatorCxs :: ByteString
kTransactionSignatureDomainSeparatorCxs = domainSeparatorCxs "Transaction"

-- | The PublicKeyHash naming a key, scheme included:
-- @SHA256(cxs(DomainSeparator("PublicKey")) || cxs(key))@
-- (C++ @public_key_hash@). Nothing if the key cannot be CXS-serialized; the
-- closed Haskell union has no unknown schemes, so that is a key whose bytes
-- are not the scheme's fixed length.
publicKeyHash :: PublicKey -> Maybe PublicKeyHash
publicKeyHash key
  | not (wellSized key) = Nothing
  | otherwise =
      Just $
        sha256HasherFinalize $
          sha256HasherUpdates sha256HasherNew [kPublicKeyHashDomainSeparatorCxs, cxsEncode key]
  where
    wellSized (PublicKey_ed25519 k) = B.length k == 32
    wellSized (PublicKey_mldsa65 k) = B.length k == kMlDsa65PublicKeyBytes

-- | The PubkeyAccountPreimage naming a key of a known scheme
-- (C++ @make_pubkey_account_preimage@, which throws @std::logic_error@ where
-- this calls 'error').
makePubkeyAccountPreimage :: PublicKey -> Word64 -> PubkeyAccountPreimage
makePubkeyAccountPreimage key n = case publicKeyHash key of
  Just h -> PubkeyAccountPreimage {pubkey_hash = h, nonce = n}
  Nothing -> error "public key of an unknown scheme"

-- | The address of a standalone-pubkey account:
-- @SHA256(cxs(DomainSeparator("PubkeyAccount")) || cxs(preimage))@
-- (C++ @pubkey_auth_address@). Nothing if the preimage cannot be
-- CXS-serialized (a pubkey hash that is not 32 bytes long).
pubkeyAuthAddress :: PubkeyAccountPreimage -> Maybe ContractAddress
pubkeyAuthAddress preimage
  | B.length preimage.pubkey_hash /= 32 = Nothing
  | otherwise =
      Just $
        sha256HasherFinalize $
          sha256HasherUpdates sha256HasherNew [kPubkeyAuthDomainSeparatorCxs, cxsEncode preimage]
