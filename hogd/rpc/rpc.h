#pragma once

#include <sched.h>

#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <string_view>
#include <thread>
#include <utility>

#include "hogd/base/compiler.h"
#include "hogd/base/io.h"
#include "hogd/base/span.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"

namespace hogd {

namespace rpc_detail {

class ClientFlow {
public:
  ClientFlow(File &&socket)
    : socket_(std::move(socket)),
      sender_([this](const std::stop_token &stoken) { send_worker(stoken); }),
      receiver_(
          [this](const std::stop_token &stoken) { receive_worker(stoken); })
  {}
  ~ClientFlow();

  // call invokes an RPC on the server. A future allows the caller to wait for
  // a response.
  std::future<OwningSpan<const std::byte>>
  call(std::span<const std::byte> args);

private:
  using Request = std::span<const std::byte>;
  using Completion = std::promise<OwningSpan<const std::byte>>;

  std::mutex lock_;
  std::condition_variable_any request_cv_;
  bool connection_failed_{false};
  std::queue<Request> requests_;
  std::queue<Completion> completions_;

  File socket_;
  std::jthread sender_;
  std::jthread receiver_;

  static void abort_call(Completion &completion);
  static void abort_calls(std::queue<Completion> &&completions);
  void fail();

  void send_worker(const std::stop_token &stoken);
  void receive_worker(const std::stop_token &stoken);
};

} // namespace rpc_detail

inline constexpr int kDefaultFlows = 4;

// RPCClient handles the client side of the RPC protocol.
class RPCClient {
public:
  // dial creates an RPCClient and connects it to the server at addr.
  static ErrnoStatus<RPCClient> dial(std::string_view addr,
                                     int flows = kDefaultFlows);

  RPCClient() = default;
  RPCClient(std::vector<std::unique_ptr<rpc_detail::ClientFlow>> &&flows)
    : flows_(std::move(flows))
  {}

  // Enable move; disable copy.
  RPCClient(RPCClient &&c) noexcept : flows_(std::move(c.flows_)) {}
  RPCClient &operator=(RPCClient &&c) noexcept
  {
    flows_ = std::move(c.flows_);
    return *this;
  }
  RPCClient(const RPCClient &c) = delete;
  RPCClient &operator=(const RPCClient &c) = delete;

  // call invokes an RPC on the server. A future allows the caller to wait for
  // a response.
  std::future<OwningSpan<const std::byte>> call(std::span<const std::byte> args)
  {
    // NOTE: getcpu is vDSO, so it's very fast. We use it to hopefully squeeze
    // out a little more cache affinity while spreading load across flows.
    unsigned int core;
    getcpu(&core, nullptr);
    return flows_[core % flows_.size()]->call(args);
  }

private:
  std::vector<std::unique_ptr<rpc_detail::ClientFlow>> flows_;
};

using RPCHandler =
    std::function<OwningSpan<const std::byte>(std::span<const std::byte>)>;

// RPCServer handles the server side of the RPC protocol.
class RPCServer : public std::enable_shared_from_this<RPCServer> {
public:
  // create is a factory method that creates an RPC server.
  static ErrnoStatus<std::shared_ptr<RPCServer>> create(uint16_t port,
                                                        RPCHandler handler);

  RPCServer(RPCHandler handler, File &&listener)
    : listener_(std::move(listener)), handler_(std::move(handler))
  {}
  ~RPCServer() = default;

  // listen accepts connects and handles RPC requests. This should only be
  // called once at a time. It generally blocks forever if there are no errors.
  ErrnoStatus<void> listen();

  // stop kills the listener, causing it to return. It also shuts down all
  // in-flight connection workers.
  void stop();

private:
  std::mutex lock_;
  File listener_;
  std::vector<File *> workers_;

  alignas(kCacheLineSize) RPCHandler handler_;

  void register_worker(File &f)
  {
    std::scoped_lock guard(lock_);
    workers_.push_back(&f);
  }

  void unregister_worker(File &f)
  {
    std::scoped_lock guard(lock_);
    for (size_t i = 0; i < workers_.size(); ++i) {
      if (workers_[i] != &f)
        continue;

      workers_[i] = workers_.back();
      workers_.pop_back();
      break;
    }
  }

  static void connection_worker(std::shared_ptr<RPCServer> ref, File &&socket,
                                const RPCHandler &handler);
};

} // namespace hogd
