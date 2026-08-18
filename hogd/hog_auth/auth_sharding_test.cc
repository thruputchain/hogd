#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "address_auth.hh"
#include "auth_card.hh"
#include "auth_rpc.hh"
#include "hogd/hog_auth/auth_server.h"
#include "hogd/hog_auth/auth_server_test_framework.h"
#include "hogd/hog_auth/dbauth_client.h"
#include "hogd/hog_auth/mock_dbauth_rpc.h"
#include "hogd/base/log.h"
#include "hogd/base/types.h"
#include "hogd/db_shard_routing/sharding.h"
#include "hogd/version.h"
#include "types.hh"
#include "x23/x23.h"

namespace hogd {

namespace {

constexpr size_t kShards = 2;

class ShardedAuthFixture {
public:
  explicit ShardedAuthFixture(size_t shard_count)
  {
    std::vector<dbauth_client_t> clients;
    clients.reserve(shard_count);
    for (size_t shard = 0; shard < shard_count; ++shard) {
      auto server = std::make_unique<MockDBAuthServer>();
      const uint16_t port = server->start_server(0);
      clients.push_back(
          make_dbauth_client(std::format("localhost:{}", port))().value());
      servers_.push_back(std::move(server));
    }
    auth_server_ = std::make_unique<AuthServer>(std::move(clients), 16);
  }

  AuthServer &auth_server() { return *auth_server_; }
  MockDBAuthRpc &shard(size_t s) { return *servers_[s]->impl(); }
  size_t shard_count() const { return servers_.size(); }

  // Initializes at `height` without seeding the cache, so lookups reach a db
  // shard. One empty slice per shard, since a block needs all of them.
  void initialize_at(BlockHeight height)
  {
    for (size_t s = 0; s < shard_count(); ++s) {
      while (shard(s).get_current_height() < height) {
        shard(s).increment_height();
      }
    }
    for (size_t s = 0; s < shard_count(); ++s) {
      auth_server().register_auth_data_updates(height, KeyChangeBatch{})();
    }
  }

private:
  std::vector<std::unique_ptr<MockDBAuthServer>> servers_;
  std::unique_ptr<AuthServer> auth_server_;
};

ContractAddress
address_owned_by(size_t owner, size_t shard_count)
{
  for (uint8_t id = 1; id != 0; ++id) {
    ContractAddress address = make_test_address(id);
    if (shard_of_address(address, shard_count) == owner) {
      return address;
    }
  }
  CHECK(false);
  return ContractAddress{};
}

// Built directly rather than through the mock, so the data exists only in the
// slice: a dropped slice shows up as a lookup finding nothing.
KeyChangeBatch
make_slice(ContractAddress address, const AddressAuthData &auth_data)
{
  x23::Ptr<AddressAuthData> auth_data_ptr;
  auth_data_ptr.emplace(auth_data);
  KeyChangeBatch batch;
  batch.push_back(
      KeyChange{.address = address, .auth_data = std::move(auth_data_ptr)});
  return batch;
}

} // namespace

// NOLINTBEGIN(bugprone-unchecked-optional-access)

// A lookup must ask the shard that owns the address. Reading from one fixed
// shard finds nothing for the addresses the others own.
TEST(AuthShardingTest, LookupRoutesToOwningShard)
{
  ShardedAuthFixture fixture(kShards);
  for (size_t owner = 0; owner < kShards; ++owner) {
    fixture.shard(owner).set(address_owned_by(owner, kShards),
                             make_test_auth_data(1));
  }
  fixture.initialize_at(2);

  for (size_t owner = 0; owner < kShards; ++owner) {
    const ContractAddress address = address_owned_by(owner, kShards);
    auto result = fixture.auth_server().lookup_auth_data(2, address)();
    ASSERT_TRUE(result.has_value()) << "shard " << owner;
    ASSERT_NE(result.value(), nullptr) << "shard " << owner;
    EXPECT_TRUE(result.value()->second.has_value())
        << "lookup asked the wrong shard for an address owned by " << owner;
  }
}

// Each shard sends its own slice of a block's key changes. The height must not
// advance until all have reported, and no slice may be lost.
TEST(AuthShardingTest, HeightAdvancesOnlyAfterEveryShardReports)
{
  ShardedAuthFixture fixture(kShards);
  fixture.initialize_at(2);

  const ContractAddress address_0 = address_owned_by(0, kShards);
  const ContractAddress address_1 = address_owned_by(1, kShards);
  const AddressAuthData auth_data_0 = make_test_auth_data(1);
  const AddressAuthData auth_data_1 = make_test_auth_data(2);
  for (size_t s = 0; s < kShards; ++s) {
    fixture.shard(s).increment_height(); // block 3
  }

  KeyChangeBatch slice_0 = make_slice(address_0, auth_data_0);
  fixture.auth_server().register_auth_data_updates(3, std::move(slice_0))();

  std::atomic<bool> lookup_returned = false;
  std::jthread waiter([&] {
    auto blocked = fixture.auth_server().lookup_auth_data(3, address_1)();
    EXPECT_TRUE(blocked.has_value());
    lookup_returned = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_FALSE(lookup_returned) << "height advanced before shard 1 reported";

  KeyChangeBatch slice_1 = make_slice(address_1, auth_data_1);
  fixture.auth_server().register_auth_data_updates(3, std::move(slice_1))();
  waiter.join();

  auto result_0 = fixture.auth_server().lookup_auth_data(3, address_0)();
  ASSERT_TRUE(result_0.has_value());
  ASSERT_NE(result_0.value(), nullptr);
  ASSERT_TRUE(result_0.value()->second.has_value()) << "shard 0's slice lost";
  EXPECT_EQ(result_0.value()->second.value(), auth_data_0);

  auto result_1 = fixture.auth_server().lookup_auth_data(3, address_1)();
  ASSERT_TRUE(result_1.has_value());
  ASSERT_NE(result_1.value(), nullptr);
  ASSERT_TRUE(result_1.value()->second.has_value()) << "shard 1's slice lost";
  EXPECT_EQ(result_1.value()->second.value(), auth_data_1);
}

// A shard forwards key changes only for the addresses it owns, so an
// unsubscribed shard's updates are lost for the life of the process.
TEST(AuthShardingTest, SubscribesToEveryShard)
{
  ShardedAuthFixture fixture(kShards);
  for (size_t s = 0; s < kShards; ++s) {
    while (fixture.shard(s).get_current_height() < 7) {
      fixture.shard(s).increment_height();
    }
    fixture.shard(s).set_intro_height(7);
  }

  AuthCard card{.protocol = version::kHogdProtocol(),
                .upid = {},
                .address_for_db = "",
                .address_for_mempool = ""};
  fixture.auth_server().subscribe_to_auth_data_changes(card)();

  for (size_t s = 0; s < kShards; ++s) {
    EXPECT_EQ(fixture.shard(s).intro_count(), 1U) << "shard " << s;
  }
  auto result =
      fixture.auth_server().lookup_auth_data(7, address_owned_by(0, kShards))();
  EXPECT_TRUE(result.has_value());
}

// NOLINTEND(bugprone-unchecked-optional-access)

} // namespace hogd
