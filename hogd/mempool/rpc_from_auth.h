#pragma once

#include <cstring>
#include <utility>

#include "auth_card.hh"
#include "auth_rpc.hh"
#include "hogd/base/log.h"
#include "hogd/base/rpc_server.h"
#include "hogd/mempool/registration.h"
#include "hogd/mempool/rpc_from_coord.h"
#include "hogd/version.h"
#include "types.hh"
#include "x23/async.h"

namespace hogd {

class MempoolAuthRpc {
public:
  using rpc_service = MempoolAuth1;
  explicit MempoolAuthRpc(MempoolCoordinatorRpc &coord) : coord_(coord) {}

  x23::Async<IntroAuthResult> mempoolauth_intro_auth(AuthCard card)
  {
    info("got {}, expected {}", card.protocol.c_str(),
         version::kHogdProtocol());
    if (strcmp(card.protocol.c_str(), version::kHogdProtocol()) != 0) {
      co_return IntroAuthResult{};
    }

    co_await coord_.register_auth_server(std::move(card));

    IntroAuthResult result;
    Registration::registration().shards_registered(&result.shards());
    co_return result;
  }

  x23::Async<void> mempoolauth_auth_data_changed(BlockHeight height,
                                                 KeyChangeBatch key_changes)
  {
    return coord_.auth_data_changed(height, std::move(key_changes));
  }

private:
  MempoolCoordinatorRpc &coord_;
};

static_assert(is_MempoolAuth1<MempoolAuthRpc *, x23::Async>);

} // namespace hogd
