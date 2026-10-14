#include <gtest/gtest.h>

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <ios>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hogd/base/json.h"
#include "hogd/base/string.h"
#include "hogd/base/types.h"
#include "hogd/crypto/mldsa.h"
#include "hogd/crypto/mldsa_signer.h"

namespace hogd {

namespace {

// NOLINTBEGIN(bugprone-unchecked-optional-access)

using namespace std::string_view_literals;

constexpr std::string_view kMsg = "test message";

TEST(MlDsa65, SignAndVerify)
{
  auto key = MlDsa65SigningKey::generate();
  auto sig = key.sign(kMsg);
  ASSERT_TRUE(sig.has_value());
  EXPECT_TRUE(mldsa65_verify(key.public_key(), *sig, kMsg));
  EXPECT_FALSE(mldsa65_verify(key.public_key(), *sig, "wrong message"sv));
  EXPECT_FALSE(mldsa65_verify(key.public_key(), *sig, {}));

  auto other_key = MlDsa65SigningKey::generate();
  EXPECT_FALSE(mldsa65_verify(other_key.public_key(), *sig, kMsg));
}

TEST(MlDsa65, EmptyMessage)
{
  auto key = MlDsa65SigningKey::generate();
  auto sig = key.sign({});
  ASSERT_TRUE(sig.has_value());
  EXPECT_TRUE(mldsa65_verify(key.public_key(), *sig, {}));
  EXPECT_FALSE(mldsa65_verify(key.public_key(), *sig, "\0"sv));
}

TEST(MlDsa65, ContextIsSigned)
{
  constexpr std::string_view kCtx = "context";
  auto key = MlDsa65SigningKey::generate();
  auto sig = key.sign(kMsg, kCtx);
  ASSERT_TRUE(sig.has_value());
  EXPECT_TRUE(mldsa65_verify(key.public_key(), *sig, kMsg, kCtx));
  EXPECT_FALSE(mldsa65_verify(key.public_key(), *sig, kMsg));
  EXPECT_FALSE(mldsa65_verify(key.public_key(), *sig, kMsg, "other context"sv));

  // The context string is not just prepended to the message.
  std::string ctx_and_msg = std::string(kCtx) + std::string(kMsg);
  EXPECT_FALSE(mldsa65_verify(key.public_key(), *sig, ctx_and_msg));

  auto no_ctx_sig = key.sign(kMsg);
  ASSERT_TRUE(no_ctx_sig.has_value());
  EXPECT_FALSE(mldsa65_verify(key.public_key(), *no_ctx_sig, kMsg, kCtx));
}

TEST(MlDsa65, ContextLengthLimit)
{
  std::string longest(kMlDsaMaxContextBytes, 'c');
  std::string too_long(kMlDsaMaxContextBytes + 1, 'c');
  auto key = MlDsa65SigningKey::generate();

  auto sig = key.sign(kMsg, longest);
  ASSERT_TRUE(sig.has_value());
  EXPECT_TRUE(mldsa65_verify(key.public_key(), *sig, kMsg, longest));

  EXPECT_FALSE(key.sign(kMsg, too_long).has_value());
  EXPECT_FALSE(key.sign_with_rnd(kMsg, too_long, MlDsaRnd{}).has_value());
  EXPECT_FALSE(mldsa65_verify(key.public_key(), *sig, kMsg, too_long));
}

// Hedged signing gives a different signature every time, and they all verify:
// signature bytes do not identify a signed message.
TEST(MlDsa65, HedgedSignaturesDiffer)
{
  auto key = MlDsa65SigningKey::generate();
  auto sig1 = key.sign(kMsg);
  auto sig2 = key.sign(kMsg);
  ASSERT_TRUE(sig1.has_value());
  ASSERT_TRUE(sig2.has_value());
  EXPECT_NE(*sig1, *sig2);
  EXPECT_TRUE(mldsa65_verify(key.public_key(), *sig1, kMsg));
  EXPECT_TRUE(mldsa65_verify(key.public_key(), *sig2, kMsg));
}

TEST(MlDsa65, SigningWithFixedRndRepeats)
{
  auto key = MlDsa65SigningKey::generate();
  MlDsaRnd zero{};
  auto sig1 = key.sign_with_rnd(kMsg, {}, zero);
  auto sig2 = key.sign_with_rnd(kMsg, {}, zero);
  ASSERT_TRUE(sig1.has_value());
  ASSERT_TRUE(sig2.has_value());
  EXPECT_EQ(*sig1, *sig2);
  EXPECT_TRUE(mldsa65_verify(key.public_key(), *sig1, kMsg));

  MlDsaRnd one{};
  one[0] = 1;
  auto sig3 = key.sign_with_rnd(kMsg, {}, one);
  ASSERT_TRUE(sig3.has_value());
  EXPECT_NE(*sig1, *sig3);
  EXPECT_TRUE(mldsa65_verify(key.public_key(), *sig3, kMsg));
}

TEST(MlDsa65, KeysFromSeed)
{
  MlDsaSeed seed{};
  seed.fill(0x2a);
  auto key1 = MlDsa65SigningKey::from_seed(seed);
  auto key2 = MlDsa65SigningKey::from_seed(seed);
  EXPECT_EQ(key1.public_key(), key2.public_key());
  EXPECT_EQ(key1.sign_with_rnd(kMsg, {}, MlDsaRnd{}),
            key2.sign_with_rnd(kMsg, {}, MlDsaRnd{}));

  seed[0] ^= 1;
  auto key3 = MlDsa65SigningKey::from_seed(seed);
  EXPECT_NE(key1.public_key(), key3.public_key());
}

TEST(MlDsa65, MovedKeyStillSigns)
{
  auto key = MlDsa65SigningKey::generate();
  auto pk = key.public_key();
  MlDsa65SigningKey moved(std::move(key));
  EXPECT_EQ(moved.public_key(), pk);
  auto sig = moved.sign(kMsg);
  ASSERT_TRUE(sig.has_value());
  EXPECT_TRUE(mldsa65_verify(pk, *sig, kMsg));
}

// Flipping any bit of a signature, whether in the commitment hash, the
// response z or the hint encoding, makes it invalid.
TEST(MlDsa65, RejectsModifiedSignature)
{
  auto key = MlDsa65SigningKey::generate();
  auto sig = key.sign(kMsg);
  ASSERT_TRUE(sig.has_value());
  for (size_t i = 0; i < sig->size(); i += 97) {
    auto modified = *sig;
    modified[i] ^= uint8_t{1} << (i % 8);
    EXPECT_FALSE(mldsa65_verify(key.public_key(), modified, kMsg))
        << "byte " << i;
  }
  auto modified = *sig;
  modified.back() ^= 0x80;
  EXPECT_FALSE(mldsa65_verify(key.public_key(), modified, kMsg));
}

TEST(MlDsa65, RejectsModifiedPublicKey)
{
  auto key = MlDsa65SigningKey::generate();
  auto sig = key.sign(kMsg);
  ASSERT_TRUE(sig.has_value());
  for (size_t i = 0; i < key.public_key().size(); i += 101) {
    auto modified = key.public_key();
    modified[i] ^= uint8_t{1} << (i % 8);
    EXPECT_FALSE(mldsa65_verify(modified, *sig, kMsg)) << "byte " << i;
  }
}

std::string
read_file(const std::string &path)
{
  std::ifstream in(path, std::ios::binary);
  std::ostringstream contents;
  contents << in.rdbuf();
  return std::move(contents).str();
}

// Copies bytes into an array of exactly N bytes, or returns std::nullopt if
// the length differs, as decoding an XDR fixed-length opaque would.
template<size_t N>
std::optional<std::array<uint8_t, N>>
to_array(std::span<const std::byte> bytes)
{
  if (bytes.size() != N) {
    return std::nullopt;
  }
  std::array<uint8_t, N> out;
  std::memcpy(out.data(), bytes.data(), N);
  return out;
}

std::string_view
string_field(const json::Value &value, std::string_view key)
{
  const json::Value *field = value.find(key);
  if (field == nullptr || !field->as_string()) {
    ADD_FAILURE() << "missing string field " << key;
    return {};
  }
  return *field->as_string();
}

bool
has_flag(const json::Value &test, std::string_view flag)
{
  const json::Value *flags = test.find("flags");
  if (flags == nullptr) {
    return false;
  }
  return std::ranges::any_of(flags->as_array(), [flag](const json::Value &f) {
    return f.as_string() == flag;
  });
}

// Wycheproof's ML-DSA-65 verification vectors: valid signatures, including
// ones at the edges of the norm and hint bounds, and invalid ones, including
// malformed hint encodings, out-of-bound responses, over-long contexts, and
// keys and signatures of the wrong length. Every vector must verify or fail
// exactly as labeled.
TEST(MlDsa65, WycheproofVerify)
{
  std::string path =
      std::string(HOGD_WYCHEPROOF_DIR) + "/mldsa_65_verify_test.json";
  std::string text = read_file(path);
  ASSERT_FALSE(text.empty()) << "cannot read " << path;
  auto doc = json::parse(text);
  ASSERT_TRUE(doc.has_value()) << path << ": " << doc.error().message;
  const json::Value *number_of_tests = doc->find("numberOfTests");
  ASSERT_NE(number_of_tests, nullptr);
  const json::Value *groups = doc->find("testGroups");
  ASSERT_NE(groups, nullptr);

  int64_t count = 0;
  for (const json::Value &group : groups->as_array()) {
    auto public_key = to_array<kMlDsa65PublicKeyBytes>(
        decode_hex(string_field(group, "publicKey")));
    const json::Value *tests = group.find("tests");
    ASSERT_NE(tests, nullptr);
    for (const json::Value &test : tests->as_array()) {
      count++;
      const json::Value *tc_id = test.find("tcId");
      ASSERT_NE(tc_id, nullptr);
      SCOPED_TRACE(testing::Message()
                   << "tcId " << tc_id->as_integer().value_or(-1) << ": "
                   << string_field(test, "comment"));
      std::string_view result = string_field(test, "result");
      ASSERT_TRUE(result == "valid" || result == "invalid") << result;

      auto sig = to_array<kMlDsa65SignatureBytes>(
          decode_hex(string_field(test, "sig")));
      if (!public_key || !sig) {
        // A key or signature of the wrong length has no representation, so
        // it is rejected before it can reach mldsa65_verify.
        EXPECT_TRUE(has_flag(test, "IncorrectPublicKeyLength") ||
                    has_flag(test, "IncorrectSignatureLength"));
        EXPECT_EQ(result, "invalid");
        continue;
      }

      auto msg = decode_hex(string_field(test, "msg"));
      std::vector<std::byte> ctx;
      if (test.find("ctx") != nullptr) {
        ctx = decode_hex(string_field(test, "ctx"));
      }
      EXPECT_EQ(mldsa65_verify(*public_key, *sig, msg, ctx), result == "valid");
    }
  }
  EXPECT_EQ(count, number_of_tests->as_integer().value_or(-1));
}

// A SHAKE128 instance (FIPS 202).
class Shake128 {
public:
  Shake128() : ctx_(EVP_MD_CTX_new(), &EVP_MD_CTX_free)
  {
    EXPECT_EQ(EVP_DigestInit_ex(ctx_.get(), EVP_shake128(), nullptr), 1);
  }

  void absorb(std::span<const uint8_t> data)
  {
    EXPECT_EQ(EVP_DigestUpdate(ctx_.get(), data.data(), data.size()), 1);
  }

  // squeeze finalizes the instance and returns its first len output bytes.
  std::vector<uint8_t> squeeze(size_t len)
  {
    std::vector<uint8_t> out(len);
    EXPECT_EQ(EVP_DigestFinalXOF(ctx_.get(), out.data(), out.size()), 1);
    return out;
  }

private:
  // NOLINTNEXTLINE(misc-include-cleaner)
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx_;
};

// CCTV's accumulated ML-DSA test
// (https://github.com/C2SP/CCTV/tree/main/ML-DSA/accumulated): derive key
// pairs from seeds read off one SHAKE128 stream, write each public key and its
// deterministic signature of an empty message to another, and compare a
// digest of the second stream with CCTV's for 100 iterations.
TEST(MlDsa65, AccumulatedKnownAnswers)
{
  constexpr size_t kIterations = 100;
  std::vector<uint8_t> seeds = Shake128().squeeze(kIterations * 32);

  Shake128 accumulator;
  for (size_t i = 0; i < kIterations; i++) {
    MlDsaSeed seed;
    std::memcpy(seed.data(), seeds.data() + (i * seed.size()), seed.size());
    auto key = MlDsa65SigningKey::from_seed(seed);
    accumulator.absorb(key.public_key());
    auto sig = key.sign_with_rnd({}, {}, MlDsaRnd{});
    ASSERT_TRUE(sig.has_value());
    accumulator.absorb(*sig);
    EXPECT_TRUE(mldsa65_verify(key.public_key(), *sig, {}));
  }
  std::vector<uint8_t> digest = accumulator.squeeze(32);
  EXPECT_EQ(encode_hex(std::as_bytes(std::span(digest))),
            "8358a1843220194417cadbc2651295cd8fc65125b5a5c1a239a16dc8b57ca199");
}

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace

} // namespace hogd
