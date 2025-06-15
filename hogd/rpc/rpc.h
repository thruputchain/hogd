#pragma once

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

// RPCClient handles the client side of the RPC protocol.
class RPCClient {
public:
  static ErrnoStatus<std::unique_ptr<RPCClient>> dial(std::string_view addr);

  RPCClient(File &&socket)
    : socket_(std::move(socket)),
      sender_([this](const std::stop_token &stoken) { send_worker(stoken); }),
      receiver_(
          [this](const std::stop_token &stoken) { receive_worker(stoken); })
  {}
  ~RPCClient();

  // call invokes an RPC on the server. A future allows the caller to wait for
  // a response.
  std::future<OwningSpan<const std::byte>>
  call(std::span<const std::byte> args);

private:
  using Request = std::span<const std::byte>;
  using Completion = std::promise<OwningSpan<const std::byte>>;

  std::mutex lock_;
  std::condition_variable_any request_cv_;
  std::queue<Request> requests_;
  std::queue<Completion> completions_;

  File socket_;
  std::jthread sender_;
  std::jthread receiver_;

  void send_worker(const std::stop_token &stoken);
  void receive_worker(const std::stop_token &stoken);
};

using RPCHandler =
    std::function<OwningSpan<const std::byte>(std::span<const std::byte>)>;

// RPCServer handles the server side of the RPC protocol.
class alignas(kCacheLineSize) RPCServer {
public:
  RPCServer(uint16_t port, RPCHandler handler)
    : port_(port), handler_(std::move(handler))
  {}
  ~RPCServer() = default;

  // listen accepts connects and handles RPC requests. This should only be
  // called once at a time. It generally blocks forever if there are no errors.
  ErrnoStatus<void> listen();

private:
  uint16_t port_;
  RPCHandler handler_; // extremely read hot so the class is cache aligned.

  void connection_worker(File &&socket);
};

} // namespace hogd
