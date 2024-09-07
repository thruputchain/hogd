#pragma once

#include "hogd/syscall/key_status.h"
#include "hogd/syscall/metered_result.h"
#include "hogd/syscall/proxy_applicator.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>

namespace hogd {

// waiting on xdr compiler
using Address       = std::array<uint8_t, 32>;
using AddressAndKey = std::array<uint8_t, 64>;
using StorageKey    = std::array<uint8_t, 32>;
using Hash          = std::array<uint8_t, 32>;

class SyscallHandler {
public:
  MeteredResult<uint64_t> get_block_number() const;

  MeteredResult<Hash> get_prev_block_hash() const;

  // storage access methods
  MeteredResult<KeyStatus> has_key(StorageKey const &key);

private:
  SyscallStatus<AddressAndKey> get_addrkey(StorageKey const &key);

  std::map<AddressAndKey, std::unique_ptr<ProxyApplicator>> storage_proxies_;

  MeteredResult<ProxyApplicator *> get_applicator(StorageKey const &key);
};

} // namespace hogd
