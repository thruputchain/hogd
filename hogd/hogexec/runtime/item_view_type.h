#pragma once

namespace hogd {

// The type of an item as viewed by a contract.
// This may be different from the logical type of the item in the database.
// It is required that there is always at least some way to distinguish
// types from each other when reading from the database,
// so as to avoid type confusion.
//
// For example, kNNInt and kNativeTokenBalance are both treated as nnint
// in the database for the purpose of conflict resolution,
// but the types are distinguished by the type of the StorageKey
// (kNativeTokenBalance only exists at specific addresses in the database).
enum class ItemViewType {
  kBytestring,
  kNNInt,
  kRetrocausalQueue,
  kNativeTokenBalance,
};

} // namespace hogd
