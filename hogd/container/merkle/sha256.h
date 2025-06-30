#pragma once

#include <openssl/sha.h>

namespace hogd {

class Sha256 {
public:
  static constexpr size_t kSha256Size = 32;

  static std::array<unsigned char, kSha256Size>
  GetSha256(const std::vector<std::byte> &data)
  {
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

    std::array<unsigned char, kSha256Size> hash;
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, data.data(), data.size());
    SHA256_Final(hash.data(), &ctx);

    return hash;
  }
};

} // namespace hogd
