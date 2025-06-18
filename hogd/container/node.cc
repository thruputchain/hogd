#include "hogd/container/node.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
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
  ret.reserve(sizeof(size_t));

  for (size_t i = 0; i < sizeof(size_t); i++) {
    ret.push_back(std::byte((s >> (8 * i)) & 0xFF));
  }
  return ret;
}

void
GetValueString(std::ostringstream &oss, const std::optional<std::string> &value)
{
  oss << std::format("Value: [{}]\n", value ? *value : "<empty>");
}

void
GetPathString(std::ostringstream &oss, const HexArray &path)
{
  oss << std::format("Path: 0x{}\n", path.str());
  oss << std::format("Path Size: {}\n", path.size());
}

void
GetChildString(std::ostringstream &oss, size_t child_idx,
               const std::array<Node *, 16> children)
{
  oss << std::format("Child {}: ", static_cast<uint8_t>(child_idx));
  if (children[child_idx]) {
    oss << std::format("{} ({})\n", HexArray(children[child_idx]->hash).str(),
                       reinterpret_cast<const void *>(children[child_idx]));
  }
  else {
    oss << "null\n";
  }
}

void
GetValueEncoding(std::vector<std::byte> &encoding,
                 const std::optional<std::string> &value)
{
  std::byte value_present_byte = value ? std::byte{0x01} : std::byte{0x00};
  encoding.push_back(value_present_byte);

  std::vector<std::byte> value_size_bytes =
      SizeToBytes(value ? value->size() : 0);
  encoding.insert(encoding.end(), value_size_bytes.begin(),
                  value_size_bytes.end());
  if (value) {
    const std::string &v = *value;
    std::transform(v.begin(), v.end(), std::back_inserter(encoding),
                   CastCharToByte);
  }
}

void
GetPathEncoding(std::vector<std::byte> &encoding, const HexArray &path)
{
  std::vector<std::byte> path_size_bytes = SizeToBytes(path.size());
  encoding.insert(encoding.end(), path_size_bytes.begin(),
                  path_size_bytes.end());

  const std::vector<std::byte> path_bytes = path.ToByteVector();
  encoding.insert(encoding.end(), path_bytes.begin(), path_bytes.end());
}

void
GetChildEncoding(std::vector<std::byte> &encoding,
                 const std::array<Node *, 16> children, size_t num_children)
{
  static constexpr std::array<std::byte, 32> kNullptr = {};

  for (size_t i = 0; i < num_children; i++) {
    Node *child = children[i];
    if (child) {
      std::transform(child->hash.begin(), child->hash.end(),
                     std::back_inserter(encoding), CastCharToByte);
    }
    else {
      encoding.insert(encoding.end(), kNullptr.begin(), kNullptr.end());
    }
  }
}

bool
GetValueFromEncoding(const std::vector<std::byte> &encodings, size_t &idx,
                     Node::Raw &node)
{
  constexpr size_t kValuePresentSize = sizeof(bool);
  if (idx + kValuePresentSize > encodings.size()) {
    return false;
  }
  bool value_present = static_cast<bool>(encodings[idx]);
  idx += kValuePresentSize;

  if (idx + sizeof(size_t) > encodings.size()) {
    return false;
  }
  size_t value_size = static_cast<size_t>(encodings[idx]);
  idx += sizeof(size_t);

  if (idx + value_size > encodings.size()) {
    return false;
  }
  if (value_present) {
    node.value = std::string(reinterpret_cast<const char *>(&encodings[idx]),
                             value_size);
  }
  else {
    node.value = std::nullopt;
  }
  idx += value_size;

  return true;
}

bool
GetPathFromEncoding(const std::vector<std::byte> &encodings, size_t &idx,
                    Node::Raw &node)
{
  if (idx + sizeof(size_t) > encodings.size()) {
    return false;
  }
  size_t path_size = static_cast<size_t>(encodings[idx]);
  idx += sizeof(size_t);
  size_t path_size_in_bytes = (path_size + 1) / 2;

  if (idx + path_size_in_bytes > encodings.size()) {
    return false;
  }
  const std::vector<std::byte> path(encodings.begin() + idx,
                                    encodings.begin() + idx +
                                        path_size_in_bytes);
  node.path = HexArray(path);
  if (path_size % 2 == 1) {
    node.path.RemoveFromBack(1);
  }
  idx += path_size_in_bytes;

  return true;
}

bool
GetBranchesFromEncoding(const std::vector<std::byte> &encodings, size_t &idx,
                        Node::Raw &node, size_t num_branches)
{
  size_t branch_encoding_size = num_branches * sizeof(SHA256Hash);
  if (idx + branch_encoding_size > encodings.size()) {
    return false;
  }
  for (size_t i = 0; i < num_branches; i++) {
    size_t start = idx + (i * sizeof(SHA256Hash));
    memcpy(&node.child_hashes[i], &encodings[start], sizeof(SHA256Hash));
  }
  idx += branch_encoding_size;

  return true;
}

bool
GetBranchFromEncoding(const std::vector<std::byte> &encodings, size_t &idx,
                      Node::Raw &node)
{
  if (!GetValueFromEncoding(encodings, idx, node)) {
    return false;
  }
  if (!GetBranchesFromEncoding(encodings, idx, node, /*num_branches=*/16)) {
    return false;
  }

  return true;
}

size_t
GetExtensionFromEncoding(const std::vector<std::byte> &encodings, size_t &idx,
                         Node::Raw &node)
{
  if (!GetPathFromEncoding(encodings, idx, node)) {
    return false;
  }

  if (!GetBranchesFromEncoding(encodings, idx, node, /*num_branches=*/1)) {
    return false;
  }

  return true;
}

size_t
GetLeafFromEncoding(const std::vector<std::byte> &encodings, size_t &idx,
                    Node::Raw &node)
{
  if (!GetPathFromEncoding(encodings, idx, node)) {
    return false;
  }

  if (!GetValueFromEncoding(encodings, idx, node)) {
    return false;
  }

  return true;
}

} // namespace

void
Node::GetBranchString(std::ostringstream &oss) const
{
  GetValueString(oss, value);
  for (size_t i = 0; i < child.size(); i++) {
    GetChildString(oss, i, child);
  }
}

void
Node::GetExtensionString(std::ostringstream &oss) const
{
  GetPathString(oss, path);
  GetChildString(oss, /*child_idx=*/0, child);
}

void
Node::GetLeafString(std::ostringstream &oss) const
{
  GetPathString(oss, path);
  GetValueString(oss, value);
}

std::string
Node::str() const
{
  std::ostringstream oss;

  oss << std::format("{} ({}, {})\n", GetTypeString(type), HexArray(hash).str(),
                     reinterpret_cast<const void *>(this));
  oss << std::format("Parent: {}\n", reinterpret_cast<const void *>(parent));

  switch (type) {
  case Node::Type::kBranch:
    GetBranchString(oss);
    break;
  case Node::Type::kExtension:
    GetExtensionString(oss);
    break;
  case Node::Type::kLeaf:
    GetLeafString(oss);
    break;
  }
  return oss.str();
}

void
Node::GetBranchEncoding(std::vector<std::byte> &encoding) const
{
  GetValueEncoding(encoding, value);
  GetChildEncoding(encoding, child, /*num_children=*/16);
}

void
Node::GetExtensionEncoding(std::vector<std::byte> &encoding) const
{
  GetPathEncoding(encoding, path);
  GetChildEncoding(encoding, child, /*num_children=*/1);
}

void
Node::GetLeafEncoding(std::vector<std::byte> &encoding) const
{
  GetPathEncoding(encoding, path);
  GetValueEncoding(encoding, value);
}

std::vector<std::byte>
Node::GetNodeEncoding()
{
  // TODO: Optimize. We know the max lengths of the values we need to hash up
  // front.
  std::vector<std::byte> to_hash;
  to_hash.push_back(static_cast<std::byte>(type));
  switch (type) {
  case Node::Type::kBranch:
    GetBranchEncoding(to_hash);
    break;
  case Node::Type::kExtension:
    GetExtensionEncoding(to_hash);
    break;
  case Node::Type::kLeaf:
    GetLeafEncoding(to_hash);
    break;
  }
  return to_hash;
}

bool
Node::GetNodeFromEncoding(const std::vector<std::byte> &encodings, size_t &idx,
                          Node::Raw &node)
{
  size_t original_idx = idx;

  static constexpr size_t kTypeSize = 1;
  if (idx + kTypeSize > encodings.size()) {
    return false;
  }
  node.type = static_cast<Type>(encodings[idx++]);

  bool ret;
  switch (node.type) {
  case Node::Type::kBranch:
    ret = GetBranchFromEncoding(encodings, idx, node);
    break;
  case Node::Type::kExtension:
    ret = GetExtensionFromEncoding(encodings, idx, node);
    break;
  case Node::Type::kLeaf:
    ret = GetLeafFromEncoding(encodings, idx, node);
    break;
  default:
    // Improper node type in the encoding.
    ret = false;
    break;
  }
  if (ret) {
    const std::vector<std::byte> node_encodings(encodings.begin() +
                                                    original_idx,
                                                encodings.begin() + idx);
    node.hash = sha256(node_encodings);
  }
  return ret;
}

} // namespace hogd
