#pragma once

#include <optional>
#include <variant>

#include "hogd/storage_proxy/types.h"

#include "hogd/syscall/syscall_status.h"

namespace hogd {

// Copy on write proxy to StorageObjects.
// Needed to support error handling across contract calls
// within a transaction (see designdocs/transaction.md).
// Each transaction InvocationFrame will maintain a map : [AddressAndKey] ->
// [StorageObjectProxy].
//
// Usage pattern:
// - Create new proxy, based on either an existing proxy (in an earlier
// InvocationFrame) or a base object (which is read from storage)
// - Modify the proxy.  Each object has write-access to itself and read-only
// access to earlier layers.
// - Commit or drop the proxy.  Commit means either calling
// commit_self_to_parent(), or (if is_base_object()) moving the object into the
// parent InvocationFrame
//
// A StorageObjectProxy should not be modified while there exists an active
// child proxy.
class StorageObjectProxy {
public:
  StorageObjectProxy(std::optional<StorageObject> const &base_object);
  StorageObjectProxy(StorageObjectProxy *parent_object);

  ~StorageObjectProxy();

  // Implements GetKeyStatus syscall
  KeyStatus get_key_status() const;

  // Implements DeleteLast syscall
  SyscallStatus<void> delete_last();

  bool is_base_object() const;
  void commit_self_to_parent();

private:
  // Link to previous CoW layer in StorageObjectProxy
  std::variant<std::optional<StorageObject>, StorageObjectProxy *>
      previous_object_;

  std::optional<StorageObject> const &get_base_object() const;

  StorageObjectProxy *get_prev_object_proxy(this auto &self)
  {
    return std::get<StorageObjectProxy *>(self.previous_object_);
  }

  mutable bool is_locked_ = false;

  bool is_deleted_ = false;

  void commit_child_proxy(StorageObjectProxy const &child_proxy);
};

} // namespace hogd
