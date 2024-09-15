#include "hogd/storage_proxy/storage_object_proxy.h"

#include "hogd/storage_proxy/types.h"
#include "hogd/syscall/syscall_status.h"

#include <cassert>
#include <optional>
#include <utility>
#include <variant>

namespace hogd {

StorageObjectProxy::StorageObjectProxy(
    std::optional<StorageObject> const &base_object)
  : previous_object_(std::in_place_type_t<std::optional<StorageObject>>{},
                     base_object)
{}

StorageObjectProxy::StorageObjectProxy(StorageObjectProxy *parent_object)
  : previous_object_(std::in_place_type_t<StorageObjectProxy *>{},
                     parent_object)
{
  assert(!parent_object->is_locked_);
  parent_object->is_locked_ = true;
}

StorageObjectProxy::~StorageObjectProxy()
{
  auto **base_ptr = std::get_if<StorageObjectProxy *>(&previous_object_);
  if (base_ptr) {
    assert((*base_ptr)->is_locked_);
    (*base_ptr)->is_locked_ = false;
  }
}

bool
StorageObjectProxy::is_base_object() const
{
  return std::holds_alternative<std::optional<StorageObject>>(previous_object_);
}

std::optional<StorageObject> const &
StorageObjectProxy::get_base_object() const
{
  return std::get<std::optional<StorageObject>>(previous_object_);
}

// NOLINTBEGIN(misc-no-recursion)
KeyStatus
StorageObjectProxy::get_key_status() const
{

  if (is_deleted_) {
    return KeyStatus::kKeyAlreadyDeleted;
  }

  if (is_base_object()) {
    return get_base_object().has_value() ? KeyStatus::kKeyExists
                                         : KeyStatus::kKeyNExist;
  }

  return get_prev_object_proxy()->get_key_status();
}
// NOLINTEND(misc-no-recursion)

SyscallStatus<void>
StorageObjectProxy::delete_last()
{
  assert(!is_locked_);
  if (get_key_status() == KeyStatus::kKeyAlreadyDeleted) {
    return SyscallError{SyscallErrorCode::kStorageKeyAlreadyDeleted};
  }

  is_deleted_ = true;
  return {};
}

void
StorageObjectProxy::commit_child_proxy(StorageObjectProxy const &child_proxy)
{
  if (child_proxy.is_deleted_) {
    is_deleted_ = true;
  }
}

void
StorageObjectProxy::commit_self_to_parent()
{
  assert(!is_base_object());

  get_prev_object_proxy()->commit_child_proxy(*this);
}

} // namespace hogd
