#pragma once

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <expected>
#include <format>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
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
#include "x23/format.h"
#include "x23/serialize.h"
#include "x23/vec.h"
#include "x23/x23.h"

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
  RPCClient(std::vector<std::unique_ptr<rpc_detail::ClientFlow>> &&flows,
            std::string &&peer)
    : flows_(std::move(flows)), peer_(std::move(peer))
  {}

  // Enable move; disable copy.
  RPCClient(RPCClient &&c) noexcept = default;
  RPCClient &operator=(RPCClient &&c) noexcept
  {
    flows_ = std::move(c.flows_);
    return *this;
  }
  RPCClient(const RPCClient &c) = delete;
  RPCClient &operator=(const RPCClient &c) = delete;

  // call_async asynchronously invokes an RPC on the server. A future allows
  // the caller to wait for a response.
  std::future<OwningSpan<const std::byte>>
  call_async(std::span<const std::byte> args);

  // call synchronously invokes an RPC on the server.
  OwningSpan<const std::byte> call(std::span<const std::byte> args)
  {
    return call_async(args).get();
  }

  // get_peername returns the peer name of the client.
  std::string_view get_peername() const { return peer_; }

private:
  std::vector<std::unique_ptr<rpc_detail::ClientFlow>> flows_;
  std::string peer_;
};

// args: request, peer
using RPCHandler =
    std::function<OwningSpan<const std::byte>(std::span<const std::byte>,
                                              std::string_view)>;

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
  bool closing_{false};
  File listener_;
  std::vector<File *> workers_;

  alignas(kCacheLineSize) RPCHandler handler_;

  bool register_worker(File &f)
  {
    std::scoped_lock guard(lock_);
    if (closing_)
      return false;
    workers_.push_back(&f);
    return true;
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
  uint32_t xid;
  // TODO(geoff): Other fields; perhaps a service id or hogd version number

  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  constexpr bool validate() const
  {
    // TODO(geoff): validation of hogd version number goes here,
    // along with any other validation logic
    return true;
  }
};

enum class RPCReponseErrorCode : uint32_t {
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

class RPCResponseErrorCategory : public std::error_category {
public:
  const char *name() const noexcept override { return "rpc response"; }
  std::string message(int code) const override;
};

using RPCResponseError =
    EnumError<RPCReponseErrorCode, RPCResponseErrorCategory>;

// Enable comparisons between RPCReponseErrorCode and std::error_code.
inline bool
operator==(const RPCReponseErrorCode &lhs, const std::error_code &rhs)
{
  return RPCResponseError{lhs} == rhs;
}

template<typename T> using RpcStatus = std::expected<T, RPCResponseError>;

} // namespace hogd

namespace std {

template<>
struct formatter<hogd::RPCReponseErrorCode> : formatter<string_view> {
  auto format(hogd::RPCReponseErrorCode error, format_context &ctx) const
  {
    static hogd::RPCResponseErrorCategory category;
    auto name = category.message(static_cast<int>(error));
    return formatter<string_view>::format(name, ctx);
  }
};

} // namespace std

namespace hogd {

struct RPCResponseHeader {
  RPCReponseErrorCode error;
  uint32_t xid;
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

template<typename Interface, StringLiteral Name, size_t Index = 0>
constexpr bool
is_rpc_method()
{
  if constexpr (Index >= Interface::valid_procnos.size()) {
    return false;
  }
  else if constexpr (Interface::template proc_meta<
                         Interface::valid_procnos[Index]>::proc_name ==
                     Name.get()) {
    return true;
  }
  else {
    return is_rpc_method<Interface, Name, Index + 1>();
  }
}

namespace detail {

template<typename Meta>
std::string
make_log_message(bool is_out, std::string_view peername, uint32_t xid,
                 const x23::is_xdr_type auto &...args)
{
  std::string ret =
      std::format("{} {} (xid={:08X}) {}::{} ", peername,
                  is_out ? "<==" : "==>", xid, Meta::version_meta::version_name,
                  Meta::proc_name);

  ret += x23::show(std::forward_as_tuple(args...));
  ret += '\n';
  return ret;
}

inline uint32_t
get_rpc_xid()
{
  static std::atomic<uint16_t> thread_id_counter{0};
  static thread_local const uint32_t current_thread_id =
      uint32_t{thread_id_counter.fetch_add(1, std::memory_order_relaxed)} << 16;
  static thread_local uint16_t local_msg_id = 0;

  return current_thread_id | local_msg_id++;
}

} // namespace detail

template<typename Interface, StringLiteral Name, size_t Index = 0>
requires (is_rpc_method<Interface, Name, Index>())
constexpr uint32_t
get_rpc_method_number()
{
  if constexpr (Interface::template proc_meta<
                    Interface::valid_procnos[Index]>::proc_name == Name.get()) {
    return Interface::valid_procnos[Index];
  }
  else {
    return get_rpc_method_number<Interface, Name, Index + 1>();
  }
}

extern int rpc_logfd;

template<typename Interface, StringLiteral Name, bool DoLogging = true>
requires (is_rpc_method<Interface, Name>())
auto
rpc_client_call(RPCClient &client, x23::is_xdr_type auto &&...args)
{
  constexpr uint32_t kMethod = get_rpc_method_number<Interface, Name>();
  using meta = Interface::template proc_meta<kMethod>;
  using return_type = meta::return_type;

  uint32_t msg_id = detail::get_rpc_xid();

  try {
    x23::Buf buf(sizeof(RPCCallHeader));
    RPCCallHeader header{.method = kMethod, .xid = msg_id};
    std::memcpy(buf.data(), &header, sizeof(RPCCallHeader));

    if constexpr (DoLogging) {
      if (rpc_logfd >= 0) {
        auto msg =
            detail::make_log_message<meta>(true, client.get_peername(), msg_id,
                                           args...);
        write(rpc_logfd, msg.data(), msg.size());
      }
    }

    (x23::serialize_to(&buf, std::forward<decltype(args)>(args)), ...);

    OwningSpan<const std::byte> res =
        client.call(std::as_bytes(std::span(buf)));
    if (res.size() < sizeof(RPCResponseHeader)) {
      error("RPC error: response too small: size = {}", res.size());
      return SystemStatus<return_type>{
          SystemError{SystemErrorCode::kUnrecoverable}};
    }
    RPCResponseHeader response_header;
    std::memcpy(&response_header, res.data(), sizeof(RPCResponseHeader));
    if (response_header.error != RPCReponseErrorCode::kRpcOk) {
      error("RPC error: {}", response_header.error);
      return SystemStatus<return_type>{
          SystemError{SystemErrorCode::kAbortTransaction}};
    }

    return_type ret;
    x23::deserialize(&ret, res.subspan(sizeof(RPCResponseHeader)));
    if constexpr (DoLogging) {
      if (rpc_logfd >= 0) {
        auto msg = detail::make_log_message<meta>(false, client.get_peername(),
                                                  msg_id, ret);
        write(rpc_logfd, msg.data(), msg.size());
      }
    }
    return SystemStatus<return_type>{std::move(ret)};
  } catch (const x23::XdrException &e) {
    error("x23 serialization error: {}", e.what());
    return SystemStatus<return_type>{
        SystemError{SystemErrorCode::kUnrecoverable}};
  }
}

// Returns the method number and the arg span if the header is valid, otherwise
// an error.
inline RpcStatus<std::tuple<RPCCallHeader, std::span<const std::byte>>>
rpc_server_parse_and_strip_header(std::span<const std::byte> args)
{
  RPCCallHeader header;
  if (args.size() < sizeof(header)) {
    return RPCResponseError{RPCReponseErrorCode::kRpcInvalidArgument};
  }
  // TODO(geoff): Maybe we can avoid this memcpy if we can somehow guarantee
  // alignment
  std::memcpy(&header, args.data(), sizeof(header));
  if (!header.validate()) {
    // TODO(geoff): We may want to distinguish between invalid header and
    // invalid method
    return RPCResponseError{RPCReponseErrorCode::kRpcInvalidArgument};
  }
  return std::pair{header, args.subspan(sizeof(header))};
}

// Template has both the method number and the name, which are checked against
// each other at compile time; this makes it easier to catch errors as rpc
// definitions evolve.
// The input arg span must already have the RPCCallHeader stripped off.
template<typename Interface, StringLiteral Name, bool DoLogging = true>
requires (is_rpc_method<Interface, Name>())
OwningSpan<const std::byte>
rpc_server_dispatch_method(auto &server, std::string_view peername,
                           uint32_t xid, std::span<const std::byte> args)
{
  constexpr uint32_t kMethod = get_rpc_method_number<Interface, Name>();
  using meta = Interface::template proc_meta<kMethod>;
  typename meta::arg_type a;
  try {
    x23::deserialize(&a, args);
    if constexpr (DoLogging) {
      if (rpc_logfd >= 0) {
        auto msg = detail::make_log_message<meta>(false, peername, xid, a);
        write(rpc_logfd, msg.data(), msg.size());
      }
    }
  } catch (const x23::XdrException &e) {
    error("x23 deserialization error(xid {}): {}", xid, e.what());
    return vec_as_bytes(response_header_to_buf(
        RPCResponseHeader{.error = RPCReponseErrorCode::kRpcInvalidArgument,
                          .xid = xid}));
  }

  auto res = meta::dispatch(server, a);

  if constexpr (DoLogging) {
    if (rpc_logfd >= 0) {
      auto msg = detail::make_log_message<meta>(true, peername, xid, res);
      write(rpc_logfd, msg.data(), msg.size());
    }
  }

  auto buf_out = response_header_to_buf(
      RPCResponseHeader{.error = RPCReponseErrorCode::kRpcOk, .xid = xid});
  x23::serialize_to(&buf_out, res);
  return vec_as_bytes(buf_out);
}

} // namespace hogd
