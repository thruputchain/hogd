#include "hogd/container/merkle/node.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <format>

#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/container/merkle/hex_array.h"
#include "hogd/container/merkle/sha256.h"
#include "hogd/crypto/crypto.h"
#include "types.hh"

namespace hogd {

namespace {

std::byte
cast_char_to_byte(unsigned char ch)
{
  return std::byte(ch);
}

void
size_to_bytes(size_t s, std::vector<std::byte> &append_to)
{
  for (size_t i = 0; i < sizeof(size_t); i++) {
    append_to.push_back(std::byte((s >> (8 * i)) & 0xFF));
  }
}

void
get_value_string(std::ostringstream &oss,
                 const std::optional<std::string> &value)
{
  oss << std::format("Value: [{}]\n", value ? *value : "<empty>");
}

void
get_path_string(std::ostringstream &oss, const HexArray &path)
{
  oss << "Path: [";
  if (path.empty()) {
    oss << "<empty>";
  }
  else {
    oss << std::format("0x{}", path.str());
  }
  oss << "]" << '\n';
  oss << std::format("Path Size: {}\n", path.size());
}

void
get_child_string(std::ostringstream &oss, size_t child_idx,
                 const std::array<Node *, Node::kMaxNumBranches> children)
{
  oss << std::format("Child {:#x}: ", static_cast<uint8_t>(child_idx));
  if (children[child_idx]) {
    oss << std::format("{} ({})\n", HexArray(children[child_idx]->hash).str(),
                       reinterpret_cast<const void *>(children[child_idx]));
  }
  else {
    oss << "null\n";
  }
}

void
get_value_encoding(std::vector<std::byte> &encoding,
                   const std::optional<std::string> &value)
{
  std::byte value_present_byte = value ? std::byte{0x01} : std::byte{0x00};
  encoding.push_back(value_present_byte);

  size_to_bytes(value ? value->size() : 0, encoding);

  if (value) {
    const std::string &v = *value;
    std::ranges::transform(v, std::back_inserter(encoding), cast_char_to_byte);
  }
}

void
get_path_encoding(std::vector<std::byte> &encoding, const HexArray &path)
{
  size_to_bytes(path.size(), encoding);

  const std::vector<std::byte> path_bytes = path.to_byte_vector();
  encoding.insert(encoding.end(), path_bytes.begin(), path_bytes.end());
}

void
get_child_encoding(std::vector<std::byte> &encoding,
                   const std::array<Node *, Node::kMaxNumBranches> children,
                   size_t num_children)
{
  size_t cur_size = encoding.size();
  encoding.insert(encoding.end(), num_children * Sha256::kSha256Size,
                  std::byte{0x0});
  for (size_t i = 0; i < num_children; i++) {
    Node *child = children[i];
    if (child) {
      memcpy(&encoding[cur_size], &child->hash, Sha256::kSha256Size);
    }
    cur_size += Sha256::kSha256Size;
  }
}

bool
get_value_from_encoding(const std::vector<std::byte> &encodings, size_t &idx,
                        Node::Raw &node)
{
  constexpr size_t kValuePresentSize = sizeof(bool);
  if (idx + kValuePresentSize > encodings.size()) {
    return false;
  }
  bool value_present = static_cast<unsigned char>(encodings[idx]) != 0U;
  idx += kValuePresentSize;

  if (idx + sizeof(size_t) > encodings.size()) {
    return false;
  }
  auto value_size = static_cast<size_t>(encodings[idx]);
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
get_path_from_encoding(const std::vector<std::byte> &encodings, size_t &idx,
                       Node::Raw &node)
{
  if (idx + sizeof(size_t) > encodings.size()) {
    return false;
  }
  auto path_size = static_cast<size_t>(encodings[idx]);
  idx += sizeof(size_t);

  if (path_size > HexArray::kMaxNumHexDigits) {
    return false;
  }

  size_t path_size_in_bytes = (path_size + 1) / 2;
  if (idx + path_size_in_bytes > encodings.size()) {
    return false;
  }
  const std::vector<std::byte> path(encodings.begin() + idx,
                                    encodings.begin() + idx +
                                        path_size_in_bytes);
  node.path = HexArray(path);
  if (path_size % 2 == 1) {
    node.path.remove_from_back(1);
  }
  idx += path_size_in_bytes;

  return true;
}

bool
get_branches_from_encoding(const std::vector<std::byte> &encodings, size_t &idx,
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
get_branch_from_encoding(const std::vector<std::byte> &encodings, size_t &idx,
                         Node::Raw &node)
{
  if (!get_value_from_encoding(encodings, idx, node)) {
    return false;
  }
  if (!get_branches_from_encoding(encodings, idx, node,
                                  /*num_branches=*/Node::kMaxNumBranches)) {
    return false;
  }

  return true;
}

size_t
get_extension_from_encoding(const std::vector<std::byte> &encodings,
                            size_t &idx, Node::Raw &node)
{
  if (!get_path_from_encoding(encodings, idx, node)) {
    return 0U;
  }

  if (!get_branches_from_encoding(encodings, idx, node, /*num_branches=*/1)) {
    return 0U;
  }

  return 1U;
}

size_t
get_leaf_from_encoding(const std::vector<std::byte> &encodings, size_t &idx,
                       Node::Raw &node)
{
  if (!get_path_from_encoding(encodings, idx, node)) {
    return 0U;
  }

  if (!get_value_from_encoding(encodings, idx, node)) {
    return 0U;
  }

  return 1U;
}

} // namespace

std::vector<std::byte> &
Node::get_node_encoding() const
{
  // TODO(jhumphri): Optimize. We know the max lengths of the values we need to
  // hash up front.
  static thread_local std::vector<std::byte> to_hash;
  to_hash.clear();

  to_hash.push_back(static_cast<std::byte>(type));
  switch (type) {
  case Node::Type::kBranch:
    get_branch_encoding(to_hash);
    break;
  case Node::Type::kExtension:
    get_extension_encoding(to_hash);
    break;
  case Node::Type::kLeaf:
    get_leaf_encoding(to_hash);
    break;
  }
  return to_hash;
}

void
Node::get_branch_encoding(std::vector<std::byte> &encoding) const
{
  CHECK_EQ(type, Node::Type::kBranch);

  get_value_encoding(encoding, value);
  get_child_encoding(encoding, child, /*num_children=*/kMaxNumBranches);
}

void
Node::get_extension_encoding(std::vector<std::byte> &encoding) const
{
  CHECK_EQ(type, Node::Type::kExtension);

  get_path_encoding(encoding, path);
  get_child_encoding(encoding, child, /*num_children=*/1);
}

void
Node::get_leaf_encoding(std::vector<std::byte> &encoding) const
{
  CHECK_EQ(type, Node::Type::kLeaf);

  get_path_encoding(encoding, path);
  get_value_encoding(encoding, value);
}

bool
Node::get_node_from_encoding(const std::vector<std::byte> &encodings,
                             size_t &idx, Node::Raw &node)
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
    ret = get_branch_from_encoding(encodings, idx, node);
    break;
  case Node::Type::kExtension:
    ret = (get_extension_from_encoding(encodings, idx, node) != 0U);
    break;
  case Node::Type::kLeaf:
    ret = (get_leaf_from_encoding(encodings, idx, node) != 0U);
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
    node.hash = Sha256::GetSha256(node_encodings);
  }
  return ret;
}

std::string
Node::str() const
{
  std::ostringstream oss;

  oss << std::format("{} ({}, {})\n", get_type_string(type),
                     HexArray(hash).str(),
                     reinterpret_cast<const void *>(this));
  oss << std::format("Parent: {}\n", reinterpret_cast<const void *>(parent));

  switch (type) {
  case Node::Type::kBranch:
    get_branch_string(oss);
    break;
  case Node::Type::kExtension:
    get_extension_string(oss);
    break;
  case Node::Type::kLeaf:
    get_leaf_string(oss);
    break;
  }
  return oss.str();
}

void
Node::get_branch_string(std::ostringstream &oss) const
{
  CHECK_EQ(type, Node::Type::kBranch);

  get_value_string(oss, value);
  for (size_t i = 0; i < child.size(); i++) {
    get_child_string(oss, i, child);
  }
}

void
Node::get_extension_string(std::ostringstream &oss) const
{
  CHECK_EQ(type, Node::Type::kExtension);

  get_path_string(oss, path);
  get_child_string(oss, /*child_idx=*/0, child);
}

void
Node::get_leaf_string(std::ostringstream &oss) const
{
  CHECK_EQ(type, Node::Type::kLeaf);

  get_path_string(oss, path);
  get_value_string(oss, value);
}

} // namespace hogd
