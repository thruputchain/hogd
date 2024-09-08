#include "hogd/syscall/proxy_applicator.h"

#include "hogd/syscall/key_status.h"
#include "hogd/syscall/metered_result.h"

namespace hogd {

MeteredResult<KeyStatus>
ProxyApplicator::has_key() const
{
  if (is_deleted_) {
    return {0_gas, KeyStatus::kKeyAlreadyDeleted};
  }
  // exists case unimplemented, pending x23c
  return {0_gas, KeyStatus::kKeyNExist};
}

MeteredResult<void>
ProxyApplicator::delete_last()
{
  is_deleted_ = true;
  return {0_gas};
}

} // namespace hogd
