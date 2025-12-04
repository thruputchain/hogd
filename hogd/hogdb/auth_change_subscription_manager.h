#pragma once

#include <map>

#include "auth_card.hh"
#include "auth_rpc.hh"
#include "hogd/base/perthread.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "types.hh"
#include "x23/async.h"
#include "x23/corosync.h"
#include "x23/refcnt.h"
#include "x23/sockio.h"

namespace hogd {

class AuthChangeSubscriptionManager {
public:
  void detached_publish_auth_changes(BlockHeight height,
                                     PerThread<KeyChangeBatch> key_changes);
  x23::Async<void> publish_auth_changes(BlockHeight height,
                                        PerThread<KeyChangeBatch> key_changes);
  x23::Async<bool> add_full_auth_change_subscriber(AuthCard card);

  ~AuthChangeSubscriptionManager() { inhibit_.destroy_wait(); }

private:
  using auth_db_client_t =
      decltype(make_rpc_client<AuthDB1>(x23::HostPort{})());

  x23::CoroLock lock_;
  std::map<AuthCard, auth_db_client_t> auth_db_clients_;
  x23::DestroyInhibit inhibit_;
};

} // namespace hogd
