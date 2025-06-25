#include <gtest/gtest.h>

#include <cerrno>
#include <cstddef>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "hogd/base/random_bytes.h"
#include "hogd/base/span.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/init.h"
#include "hogd/rpc/rpc.h"

namespace hogd {

constexpr size_t kArgSize = 256;
constexpr size_t kRPCs = 10000;

class RPCTest : public testing::Test {
protected:
  RPCTest() = default;
  ~RPCTest() override = default;

  void SetUp() override
  {
    // this handler echoes args back in the return value. This is useful for
    // testing because it allows us to check for corruption in the value.
    auto handler =
        [](std::span<const std::byte> args) -> OwningSpan<const std::byte> {
      std::vector buf(args.begin(), args.end());
      std::span<const std::byte> span(buf);
      return OwningSpan(span, [ref = std::move(buf)] mutable {});
    };

    // set up server listener.
    ErrnoStatus<std::shared_ptr<RPCServer>> server =
        RPCServer::create(8080, handler);
    ASSERT_TRUE(server);
    server_ = std::move(*server);
    server_listener_ = std::jthread([this] {
      if (ErrnoStatus<void> ret = server_->listen(); !ret) {
        EXPECT_EQ(ret.error(), EINVAL);
      }
    });

    // set up client connections.
    ErrnoStatus<RPCClient> client = RPCClient::dial("127.0.0.1:8080");
    ASSERT_TRUE(client);
    client_ = std::move(*client);
  }

  void TearDown() override { server_->stop(); }

  RPCClient client_;
  std::shared_ptr<RPCServer> server_;
  std::jthread server_listener_;
  RandomBytes rand_;
};

TEST_F(RPCTest, SynchronousCalls)
{
  std::string args(kArgSize, '0');

  for (size_t i = 0; i < kRPCs; i++) {
    rand_(std::span(args));
    std::future<OwningSpan<const std::byte>> ret =
        client_.call(std::as_bytes(std::span(args)));
    EXPECT_EQ(ret.get(), std::as_bytes(std::span(args)));
  }
}

TEST_F(RPCTest, AsynchronousCalls)
{
  std::vector<std::string> args;
  args.reserve(kRPCs);
  for (size_t i = 0; i < kRPCs; i++) {
    std::string arg(kArgSize, '0');
    rand_(std::span(arg));
    args.emplace_back(std::move(arg));
  }

  // make calls asynchronously.
  std::vector<std::future<OwningSpan<const std::byte>>> futures;
  futures.reserve(kRPCs);
  for (size_t i = 0; i < kRPCs; i++) {
    futures.emplace_back(client_.call(std::as_bytes(std::span(args[i]))));
  }

  // check results.
  for (size_t i = 0; i < kRPCs; i++) {
    EXPECT_EQ(futures[i].get(), std::as_bytes(std::span(args[i])));
  }
}

TEST_F(RPCTest, ManyThreads)
{
  std::vector<std::jthread> threads;
  threads.reserve(kRPCs);
  for (size_t i = 0; i < kRPCs; i++) {
    threads.emplace_back([this] {
      std::string args(kArgSize, '0');
      rand_(std::span(args));
      std::future<OwningSpan<const std::byte>> ret =
          client_.call(std::as_bytes(std::span(args)));
      EXPECT_EQ(ret.get(), std::as_bytes(std::span(args)));
    });
  }
}

} // namespace hogd

int
main(int argc, char *argv[])
{
  int ret = hogd::hogd_init(argc, argv);
  if (ret)
    return ret;
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
