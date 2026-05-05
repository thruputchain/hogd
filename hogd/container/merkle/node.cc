#include "hogd/container/merkle/node.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <format>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "hogd/base/log.h"
#include "hogd/base/perthread.h"
#include "hogd/base/types.h"
#include "hogd/container/merkle/hex_array.h"
#include "hogd/crypto/crypto.h"
#include "types.hh"

namespace hogd {

namespace {

static_assert(sizeof(SHA256Hash) == SHA256Hash{}.size(),
              "Merkle proof wire format requires sizeof(SHA256Hash) == element count");

template<typename Sink>
void
emit_bytes(Sink &sink, std::span<const std::byte> bytes)
{
  if (!bytes.empty()) {
    sink(bytes);
  }
}

template<size_t N, typename Sink>
void
emit_bytes(Sink &sink, const std::array<std::byte, N> &bytes)
{
  emit_bytes(sink, std::span(bytes));
}

std::array<std::byte, sizeof(uint64_t)>
size_to_bytes(uint64_t s)
{
  std::array<std::byte, sizeof(uint64_t)> bytes;
  for (size_t i = 0; i < sizeof(uint64_t); i++) {
    bytes[i] = std::byte((s >> (8 * i)) & 0xFF);
  }
  return bytes;
}

void
get_value_string(std::ostringstream &oss,
                 const std::optional<SHA256Hash> &value_hash)
{
  oss << std::format("Value: [{}]\n",
                     value_hash ? HexArray(*value_hash).str() : "<empty>");
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
                 const std::array<Node *, Node::kMaxNumBranches> &children)
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

template<typename Sink>
void
visit_value_encoding(Sink &sink, const std::optional<SHA256Hash> &value_hash)
{
  std::array<std::byte, 1> value_present = {value_hash ? std::byte{0x01}
                                                       : std::byte{0x00}};
  emit_bytes(sink, value_present);

  if (value_hash) {
    emit_bytes(sink, std::as_bytes(
                         std::span(value_hash->data(), value_hash->size())));
  }
}

template<typename Sink>
void
visit_path_encoding(Sink &sink, const HexArray &path)
{
  emit_bytes(sink, size_to_bytes(path.size()));
  path.visit_encoded_bytes(sink);
}

template<typename Sink>
void
visit_children_encoding(
    Sink &sink, const std::array<Node *, Node::kMaxNumBranches> &children)
{
  for (Node *child : children) {
    if (child) {
      emit_bytes(sink, std::as_bytes(
                           std::span(child->hash.data(), child->hash.size())));
    }
    else {
      std::array<std::byte, SHA256Hash{}.size()> zero_bytes{};
      emit_bytes(sink, zero_bytes);
    }
  }
}

template<typename Sink>
void
visit_child_encoding(Sink &sink, Node *child)
{
  if (child) {
    emit_bytes(sink, std::as_bytes(
                         std::span(child->hash.data(), child->hash.size())));
  }
  else {
    std::array<std::byte, SHA256Hash{}.size()> zero_bytes{};
    emit_bytes(sink, zero_bytes);
  }
}

template<typename Sink>
void
visit_node_encoding(Sink &sink, const Node &node)
{
  std::array<std::byte, 1> type_byte = {static_cast<std::byte>(node.type)};
  emit_bytes(sink, type_byte);

  switch (node.type) {
  case Node::Type::kBranch:
    visit_value_encoding(sink, node.value_hash());
    visit_children_encoding(sink, node.branch_children());
    break;
  case Node::Type::kExtension:
    visit_path_encoding(sink, node.path());
    visit_child_encoding(sink, node.extension_child());
    break;
  case Node::Type::kLeaf:
    visit_path_encoding(sink, node.path());
    visit_value_encoding(sink, node.value_hash());
    break;
  }
}

bool
get_value_from_encoding(const std::vector<std::byte> &encodings, size_t &idx,
                        Node::Raw &node)
{
  static_assert(sizeof(bool) == 1,
                "Merkle proof encoding uses a 1-byte value-present flag; "
                "decoder logic (and other on-wire bool fields) rely on "
                "sizeof(bool) == 1");
  constexpr size_t kValuePresentSize = 1;
  if (idx + kValuePresentSize > encodings.size()) {
    return false;
  }
  const auto flag_byte = static_cast<unsigned char>(encodings[idx]);
  if (flag_byte != 0U && flag_byte != 1U) {
    return false;
  }
  bool value_present = flag_byte != 0U;
  idx += kValuePresentSize;

  if (value_present) {
    node.value_hash = SHA256Hash();
    if (idx + node.value_hash->size() > encodings.size()) {
      return false;
    }
    memcpy(node.value_hash->data(), &encodings[idx], node.value_hash->size());
    idx += node.value_hash->size();
  }
  else {
    node.value_hash = std::nullopt;
  }

  return true;
}

bool
get_path_from_encoding(const std::vector<std::byte> &encodings, size_t &idx,
                       Node::Raw &node)
{
  if (idx + sizeof(uint64_t) > encodings.size()) {
    return false;
  }
  uint64_t path_size = 0;
  for (size_t i = 0; i < sizeof(uint64_t); i++) {
    path_size |= static_cast<uint64_t>(static_cast<uint8_t>(encodings[idx + i]))
                 << (8 * i);
  }
  idx += sizeof(uint64_t);

  if (path_size > HexArray::kMaxNumHexDigits) {
    return false;
  }

  size_t path_size_in_bytes = (path_size + 1) / 2;
  if (idx + path_size_in_bytes > encodings.size()) {
    return false;
  }
  node.path = HexArray(std::span(encodings).subspan(idx, path_size_in_bytes));
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
  size_t branch_encoding_size = num_branches * node.child_hashes[0].size();
  if (idx + branch_encoding_size > encodings.size()) {
    return false;
  }
  for (size_t i = 0; i < num_branches; i++) {
    size_t start = idx + (i * node.child_hashes[i].size());
    memcpy(node.child_hashes[i].data(), &encodings[start], node.child_hashes[i].size());
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
  static PerThread<std::vector<std::byte>> to_hash;
  to_hash->clear();

  auto append_to_vector = [&](std::span<const std::byte> bytes) {
    to_hash->insert(to_hash->end(), bytes.begin(), bytes.end());
  };
  visit_node_encoding(append_to_vector, *this);
  return *to_hash;
}

SHA256Hash
Node::compute_node_hash() const
{
  SHA256Hasher hasher;
  auto update_hash = [&](std::span<const std::byte> bytes) {
    hasher.update(bytes);
  };
  visit_node_encoding(update_hash, *this);
  return hasher.finalize();
}

void
Node::get_branch_encoding(std::vector<std::byte> &encoding) const
{
  CHECK_EQ(type, Node::Type::kBranch);

  auto append_to_vector = [&](std::span<const std::byte> bytes) {
    encoding.insert(encoding.end(), bytes.begin(), bytes.end());
  };
  visit_value_encoding(append_to_vector, value_hash());
  visit_children_encoding(append_to_vector, branch_children());
}

void
Node::get_extension_encoding(std::vector<std::byte> &encoding) const
{
  CHECK_EQ(type, Node::Type::kExtension);

  auto append_to_vector = [&](std::span<const std::byte> bytes) {
    encoding.insert(encoding.end(), bytes.begin(), bytes.end());
  };
  visit_path_encoding(append_to_vector, path());
  visit_child_encoding(append_to_vector, extension_child());
}

void
Node::get_leaf_encoding(std::vector<std::byte> &encoding) const
{
  CHECK_EQ(type, Node::Type::kLeaf);

  auto append_to_vector = [&](std::span<const std::byte> bytes) {
    encoding.insert(encoding.end(), bytes.begin(), bytes.end());
  };
  visit_path_encoding(append_to_vector, path());
  visit_value_encoding(append_to_vector, value_hash());
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
    node.hash =
        sha256(std::span(encodings).subspan(original_idx, idx - original_idx));
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

  get_value_string(oss, value_hash());
  for (size_t i = 0; i < branch_children().size(); i++) {
    get_child_string(oss, i, branch_children());
  }
}

void
Node::get_extension_string(std::ostringstream &oss) const
{
  CHECK_EQ(type, Node::Type::kExtension);

  get_path_string(oss, path());
  std::array<Node *, Node::kMaxNumBranches> child = {extension_child()};
  get_child_string(oss, /*child_idx=*/0, child);
}

void
Node::get_leaf_string(std::ostringstream &oss) const
{
  CHECK_EQ(type, Node::Type::kLeaf);

  get_path_string(oss, path());
  get_value_string(oss, value_hash());
}

} // namespace hogd
