#include "wasmtime_instance_cache.h"
#include "hogd/rust/wasmtime/bindings.h"

namespace hogd {

ExternalInstancePreType::~ExternalInstancePreType()
{
  if (instance_pre_ptr_ != nullptr) {
    arc_instance_pre_drop_ref(instance_pre_ptr_);
  }
}

ExternalInstancePreType::ExternalInstancePreType(
    ExternalInstancePreType const &other)
  : instance_pre_ptr_(other.instance_pre_ptr_)
{
  if (instance_pre_ptr_ != nullptr) {
    arc_instance_pre_clone_ref(instance_pre_ptr_);
  }
}

ExternalInstancePreType::ExternalInstancePreType(
    ExternalInstancePreType &&other) noexcept
  : instance_pre_ptr_(other.instance_pre_ptr_)
{
  other.instance_pre_ptr_ = nullptr;
}

ExternalInstancePreType &
ExternalInstancePreType::operator=(ExternalInstancePreType &&other) noexcept
{
  if (this != &other) {
    if (instance_pre_ptr_ != nullptr) {
      arc_instance_pre_drop_ref(instance_pre_ptr_);
    }
    instance_pre_ptr_ = other.instance_pre_ptr_;
    other.instance_pre_ptr_ = nullptr;
  }
  return *this;
}

}; // namespace hogd
