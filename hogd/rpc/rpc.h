#pragma once

#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <span>
#include <stop_token>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "hogd/base/compiler.h"
#include "hogd/base/cpputil.h"
#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/base/span.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/status.h"
#include "x23/serialize.h"
#include "x23/vec.h"

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
  std::future<OwningSpan<const std::byte>>
  call(std::span<const std::byte> args);

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

  RPCServer(uint16_t port, RPCHandler handler, File &&listener)
    : port_(port), listener_(std::move(listener)), handler_(std::move(handler))
  {}
  ~RPCServer() = default;

  // get_port returns the network port assigned to the server. This is
  // especially useful if it was chosen authomatically.
  uint16_t get_port() const { return port_; }

  // listen accepts connects and handles RPC requests. This should only be
  // called once at a time. It generally blocks forever if there are no errors.
  ErrnoStatus<void> listen();

  // stop kills the listener, causing it to return. It also shuts down all
  // in-flight connection workers.
  void stop();

private:
  const uint16_t port_;
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

// The leading bytes of an RPC message within hogd.
struct RPCCallHeader {
  uint32_t method;
  // TODO(geoff): Other fields; perhaps a service id or hogd version number

  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  constexpr bool validate() const
  {
    // TODO(geoff): validation of hogd version number goes here,
    // along with any other validation logic
    return true;
  }
};

enum RPCReponseError : uint32_t {
  // The RPC call was successfully passed to the server.
  // This does not mean that the RPC-specific internal logic
  // succeeds.  Rather, this merely means that the RPC call
  // is successfully passed to the server and that arguments
  // are properly parsed for the specified method.
  kRpcOk = 0,
  // The RPC byte buffer was not able to be parsed into the
  // arguments for the specified method.
  kRpcInvalidArgument = 1,
  // Unknown method
  kRpcUnknownMethod = 2,

  // TODO(geoff): other errors
};

} // namespace hogd

namespace std {

template<> struct formatter<hogd::RPCReponseError> : formatter<string_view> {
  auto format(hogd::RPCReponseError error, format_context &ctx) const
  {
    string_view name = "Unknown RPCResponseError";
    switch (error) {
    case hogd::RPCReponseError::kRpcOk:
      name = "kRpcOk";
      break;
    case hogd::RPCReponseError::kRpcInvalidArgument:
      name = "kRpcInvalidArgument";
      break;
    case hogd::RPCReponseError::kRpcUnknownMethod:
      name = "kRpcUnknownMethod";
      break;
    }
    return formatter<string_view>::format(name, ctx);
  }
};

} // namespace std

namespace hogd {

struct RPCResponseHeader {
  RPCReponseError error;
};

inline x23::Buf
response_header_to_buf(RPCResponseHeader header)
{
  x23::Buf bytes(sizeof(RPCResponseHeader));
  std::memcpy(bytes.data(), &header, sizeof(RPCResponseHeader));
  return bytes;
}

static_assert(std::is_trivially_copyable_v<RPCCallHeader>,
              "RPCCallHeader must be trivially copyable");
static_assert(std::is_trivially_copyable_v<RPCResponseHeader>,
              "RPCResponseHeader must be trivially copyable");

namespace detail {

template<typename Interface, uint32_t Method, size_t Index = 0>
constexpr bool
is_valid_rpc_method()
{
  if constexpr (Index >= Interface::valid_procnos.size()) {
    return false;
  }
  else if constexpr (Interface::valid_procnos[Index] == Method) {
    return true;
  }
  else {
    return is_valid_rpc_method<Interface, Method, Index + 1>();
  }
}

} // namespace detail

template<typename Interface, uint32_t Method, StringLiteral Name>
concept is_rpc_method =
    detail::is_valid_rpc_method<Interface, Method>() &&
    Interface::template proc_meta<Method>::proc_name == Name.get();

template<typename Interface, uint32_t Method, StringLiteral Name>
requires is_rpc_method<Interface, Method, Name>
auto
rpc_client_call(RPCClient &client, auto &&...args)
{
  using return_type = Interface::template proc_meta<Method>::return_type;

  try {
    x23::Buf buf(sizeof(RPCCallHeader));
    RPCCallHeader header{.method = Method};
    std::memcpy(buf.data(), &header, sizeof(RPCCallHeader));

    (x23::serialize_to(&buf, std::forward<decltype(args)>(args)), ...);

    auto future = client.call(std::as_bytes(std::span(buf)));
    future.wait();
    assert(future.valid());
    auto res = future.get();
    if (res.size() < sizeof(RPCResponseHeader)) {
      error("RPC error: response too small: size = {}", res.size());
      return SystemStatus<return_type>{
          SystemError{SystemErrorCode::kUnrecoverable}};
    }
    RPCResponseHeader response_header;
    std::memcpy(&response_header, res.data(), sizeof(RPCResponseHeader));
    if (response_header.error != RPCReponseError::kRpcOk) {
      error("RPC error: {}", response_header.error);
      return SystemStatus<return_type>{
          SystemError{SystemErrorCode::kAbortTransaction}};
    }

    return_type ret;
    x23::deserialize(&ret, res.subspan(sizeof(RPCResponseHeader)));
    return SystemStatus<return_type>{std::move(ret)};
  } catch (const x23::XdrException &e) {
    error("x23 serialization error: {}", e.what());
    return SystemStatus<return_type>{
        SystemError{SystemErrorCode::kUnrecoverable}};
  }
}

// Template has both the method number and the name, which are checked against
// each other at compile time; this makes it easier to catch errors as rpc
// definitions evolve.
template<typename Interface, uint32_t Method, StringLiteral Name>
requires is_rpc_method<Interface, Method, Name>
OwningSpan<const std::byte>
rpc_server_dispatch_method(auto &server, std::span<const std::byte> args)
{
  using meta = Interface::template proc_meta<Method>;
  typename meta::arg_type a;
  try {
    x23::deserialize(&a, args.subspan(sizeof(RPCCallHeader)));
  } catch (const x23::XdrException &e) {
    error("x23 deserialization error: {}", e.what());
    return vec_as_bytes(response_header_to_buf(
        RPCResponseHeader{RPCReponseError::kRpcInvalidArgument}));
  }

  auto res = meta::dispatch(server, a);
  return vec_as_bytes(x23::serialize(res));
}

} // namespace hogd
