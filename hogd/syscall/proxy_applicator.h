#pragma once

#include "hogd/syscall/key_status.h"
#include "hogd/syscall/syscall_status.h"

namespace hogd {

// Missing the contents, which is pending x23c
/**
 * Mediates accesses to one storage location (one StorageObject at a particular
 * AddressAndKey) from one contract invocation.
 *
 * Responsible for ensuring a contract has a consistent local view of the
 * modifications it has made already, and then producing at the end of the
 * contract a (condensed) list of those modifications.
 */
class ProxyApplicator {
public:
  KeyStatus has_key() const;

  SyscallStatus<void> delete_last();

private:
  bool is_deleted_ = false;
};

} // namespace hogd
