// merkle.h - provides a merkle patricia trie

#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cassert>
#include <functional>
#include <initializer_list>
#include <iomanip>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include "hex_array.h"
#include "hogd/base/compiler.h"
#include "hogd/base/os.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/crypto/crypto.h"
#include "types.hh"

namespace hogd {

namespace {

std::byte
CastCharToByte(unsigned char ch)
{
  return std::byte(static_cast<unsigned char>(ch));
}

std::vector<std::byte>
SizeToBytes(size_t s)
{
  std::vector<std::byte> ret;
  for (size_t i = 0; i < sizeof(size_t); i++) {
    ret.push_back(std::byte((s >> (8 * i)) & 0xFF));
  }
  return ret;
}

} // namespace

struct Node {
  enum class Type { kBranch, kExtension, kLeaf };

  Type type;
  SHA256Hash hash;
  Node *parent;

  // TODO: Optimize the variables below with a union or different node types.
  // TODO: Optimize the vectors below--use something other than a vector. We
  // know that path lengths will never exceed 65 bytes.

  // Used by extensions and leaves.
  HexArray path;

  // Used by branches and leaves.
  // TODO: Should we be able to place values in extension nodes, too? For
  // example, the [TwoLeavesAndOneBranch] test would produce a more efficient
  // tree if we could do this.
  std::optional<std::string> value;

  // Used by branches and extensions.
  //
  // For branches, we branch on a hex value and up to all 16 slots may be
  // filled. There are 16 different hex values, so we need 16 different branch
  // slots.
  //
  // For extensions, only the first slot is filled. The other 15 slots are
  // unused.
  std::array<Node *, 16> child;

  std::vector<std::byte> GetNodeEncoding()
  {
    static constexpr std::array<std::byte, 32> kNullptr = {};

    // TODO: Optimize. We know the max lengths of the values we need to hash up
    // front.
    std::vector<std::byte> to_hash;
    to_hash.push_back(static_cast<std::byte>(type));
    switch (type) {
    case Node::Type::kBranch:
    {
      std::byte value_present_byte = value ? std::byte{0x01} : std::byte{0x00};
      to_hash.push_back(value_present_byte);

      std::vector<std::byte> value_size_bytes =
          SizeToBytes(value ? value->size() : 0);
      to_hash.insert(to_hash.end(), value_size_bytes.begin(),
                     value_size_bytes.end());
      if (value) {
        const std::string &v = *value;
        std::transform(v.begin(), v.end(), std::back_inserter(to_hash),
                       CastCharToByte);
      }

      for (Node *child : child) {
        if (child) {
          std::transform(child->hash.begin(), child->hash.end(),
                         std::back_inserter(to_hash), CastCharToByte);
        }
        else {
          to_hash.insert(to_hash.end(), kNullptr.begin(), kNullptr.end());
        }
      }
      break;
    }
    case Node::Type::kExtension:
    {
      std::vector<std::byte> path_size_bytes = SizeToBytes(path.size());
      to_hash.insert(to_hash.end(), path_size_bytes.begin(),
                     path_size_bytes.end());

      const std::vector<std::byte> path_bytes = path.ToByteVector();
      to_hash.insert(to_hash.end(), path_bytes.begin(), path_bytes.end());

      if (child[0]) {
        std::transform(child[0]->hash.begin(), child[0]->hash.end(),
                       std::back_inserter(to_hash), CastCharToByte);
      }
      else {
        to_hash.insert(to_hash.end(), kNullptr.begin(), kNullptr.end());
      }
      break;
    }
    case Node::Type::kLeaf:
    {
      std::vector<std::byte> path_size_bytes = SizeToBytes(path.size());
      to_hash.insert(to_hash.end(), path_size_bytes.begin(),
                     path_size_bytes.end());

      const std::vector<std::byte> path_bytes = path.ToByteVector();
      to_hash.insert(to_hash.end(), path_bytes.begin(), path_bytes.end());

      std::vector<std::byte> value_size_bytes =
          SizeToBytes(value ? value->size() : 0);
      to_hash.insert(to_hash.end(), value_size_bytes.begin(),
                     value_size_bytes.end());

      if (value) {
        const std::string &v = *value;
        std::transform(v.begin(), v.end(), std::back_inserter(to_hash),
                       CastCharToByte);
      }
      break;
    }
    }
    return to_hash;
  }

  SHA256Hash GetNodeHash() { return sha256(GetNodeEncoding()); }

  static bool GetNodeHashFromEncoding(const std::vector<std::byte> &encodings,
                                      size_t &idx, SHA256Hash &hash,
                                      Node::Type &type, size_t &path_size,
                                      std::optional<std::string> &v,
                                      std::array<SHA256Hash, 16> &child_hashes)
  {
    static constexpr size_t kTypeSize = 1;
    if (idx + kTypeSize > encodings.size()) {
      return false;
    }
    type = static_cast<Node::Type>(encodings[idx]);
    size_t encoding_size = 0;
    switch (type) {
    case Node::Type::kBranch:
      // printf("saw branch\n");
      encoding_size = GetBranchEncodingSize(encodings, idx, child_hashes, v);
      break;
    case Node::Type::kExtension:
      // printf("saw extension\n");
      encoding_size =
          GetExtensionEncodingSize(encodings, idx, path_size, child_hashes);
      break;
    case Node::Type::kLeaf:
      // printf("saw leaf\n");
      encoding_size = GetLeafEncodingSize(encodings, idx, path_size, v);
      break;
    default:
      // Improper node type in the encoding.
      return false;
      break;
    }
    if (!encoding_size) {
      return false;
    }
    const std::vector<std::byte> encoding(encodings.begin() + idx,
                                          encodings.begin() + idx +
                                              encoding_size);
    hash = sha256(encoding);
    idx += encoding_size;

    return true;
  }

  static size_t GetBranchEncodingSize(const std::vector<std::byte> &encodings,
                                      size_t idx,
                                      std::array<SHA256Hash, 16> &child_hashes,
                                      std::optional<std::string> &v)
  {
    const size_t orig_idx = idx;
    static constexpr size_t kTypeSize = 1;
    if (idx + kTypeSize > encodings.size()) {
      return 0;
    }
    idx += kTypeSize;

    if (idx + sizeof(size_t) > encodings.size()) {
      return 0;
    }
    constexpr size_t kValuePresentSize = sizeof(bool);
    if (idx + kValuePresentSize > encodings.size()) {
      return 0;
    }
    bool value_present = static_cast<bool>(encodings[idx]);
    idx += kValuePresentSize;

    size_t value_size = static_cast<size_t>(encodings[idx]);
    idx += sizeof(size_t);

    if (idx + value_size > encodings.size()) {
      return 0;
    }
    if (value_present) {
      v = std::string(reinterpret_cast<const char *>(&encodings[idx]),
                      value_size);
    }
    else {
      v = std::nullopt;
    }
    idx += value_size;
    // printf("idx is now %zu, orig_idx %zu\n", idx, orig_idx);

    static constexpr size_t kBranchEncodingSize = (16 * sizeof(SHA256Hash));
    if (idx + kBranchEncodingSize > encodings.size()) {
      return 0;
    }
    for (size_t i = 0; i < 16; i++) {
      size_t start = idx + (i * sizeof(SHA256Hash));
      memcpy(&child_hashes[i], &encodings[start], sizeof(SHA256Hash));
    }
    idx += kBranchEncodingSize;

    return idx - orig_idx;
  }

  static size_t
  GetExtensionEncodingSize(const std::vector<std::byte> &encodings, size_t idx,
                           size_t &path_size,
                           std::array<SHA256Hash, 16> &child_hashes)
  {
    const size_t orig_idx = idx;
    static constexpr size_t kTypeSize = 1;
    if (idx + kTypeSize > encodings.size()) {
      return 0;
    }
    idx += kTypeSize;

    if (idx + sizeof(size_t) > encodings.size()) {
      return 0;
    }
    path_size = static_cast<size_t>(encodings[idx]);
    idx += sizeof(size_t);
    size_t path_size_in_bytes = (path_size + 1) / 2;

    if (idx + path_size_in_bytes > encodings.size()) {
      return 0;
    }
    idx += path_size_in_bytes;

    static constexpr size_t kChildHashSize = sizeof(SHA256Hash);
    if (idx + kChildHashSize > encodings.size()) {
      return 0;
    }
    memcpy(&child_hashes[0], &encodings[idx], sizeof(SHA256Hash));
    idx += kChildHashSize;

    return idx - orig_idx;
  }

  static size_t GetLeafEncodingSize(const std::vector<std::byte> &encodings,
                                    size_t idx, size_t &path_size,
                                    std::optional<std::string> &v)
  {
    const size_t orig_idx = idx;
    static constexpr size_t kTypeSize = 1;
    if (idx + kTypeSize > encodings.size()) {
      return 0;
    }
    idx += kTypeSize;

    if (idx + sizeof(size_t) > encodings.size()) {
      return 0;
    }
    path_size = static_cast<size_t>(encodings[idx]);
    idx += sizeof(size_t);
    size_t path_size_in_bytes = (path_size + 1) / 2;

    if (idx + path_size_in_bytes > encodings.size()) {
      return 0;
    }
    idx += path_size_in_bytes;

    if (idx + sizeof(size_t) > encodings.size()) {
      return 0;
    }
    size_t value_size = static_cast<size_t>(encodings[idx]);
    idx += sizeof(size_t);

    if (idx + value_size > encodings.size()) {
      return 0;
    }
    v = std::string(reinterpret_cast<const char *>(&encodings[idx]),
                    value_size);
    idx += value_size;

    return idx - orig_idx;
  }
};
} // namespace hogd
