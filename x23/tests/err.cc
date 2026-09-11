#include <cerrno>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

#include <netdb.h>
#include <sys/socket.h>

#include <gtest/gtest.h>

#include "x23/err.h"
#include "x23/serialize_cxs.h"
#include "x23/sockio.h"

using namespace x23;

namespace {

using ConnectError = MultiError<Errno, GaiError>;

void
check_value_error(auto &&result, const std::error_code &code)
{
  try {
    (void)std::forward<decltype(result)>(result).value();
    FAIL() << "value() did not throw";
  } catch (const std::system_error &e) {
    EXPECT_EQ(e.code(), code);
    EXPECT_EQ(&e.code().category(), &code.category());
    EXPECT_NE(std::string(e.what()).find(code.message()), std::string::npos);
  }
}

template<typename T, typename E>
void
check_value_overloads(const E &error)
{
  std::expected<T, E> result = error;
  const std::expected<T, E> const_result = error;
  check_value_error(result, error.error_code());
  check_value_error(const_result, error.error_code());
  check_value_error(std::move(result), error.error_code());
  check_value_error(std::move(const_result), error.error_code());
}

} // namespace

TEST(ExpectedError, ErrnoValueThrowsSystemError)
{
  check_value_overloads<int>(Errno{EACCES});
  check_value_overloads<void>(Errno{EACCES});
}

TEST(ExpectedError, GaiValueThrowsSystemError)
{
  check_value_overloads<int>(GaiError{EAI_NONAME});
  check_value_overloads<void>(GaiError{EAI_NONAME});
}

TEST(ExpectedError, MultiErrorValueThrowsSystemError)
{
  for (const ConnectError error :
       {ConnectError{Errno{ECONNREFUSED}}, ConnectError{GaiError{EAI_AGAIN}}}) {
    check_value_overloads<int>(error);
    check_value_overloads<void>(error);
  }
}

TEST(ExpectedError, SuccessfulValueAccess)
{
  std::expected<int, Errno> result = 42;
  EXPECT_EQ(&result.value(), &*result);
  result.value() = 43;
  EXPECT_EQ(std::as_const(result).value(), 43);
  EXPECT_EQ(std::move(std::as_const(result)).value(), 43);
  EXPECT_EQ(std::move(result).value(), 43);

  std::expected<std::unique_ptr<int>, ConnectError> owned =
      std::make_unique<int>(42);
  auto value = std::move(owned).value();
  ASSERT_TRUE(value);
  EXPECT_EQ(*value, 42);
  EXPECT_FALSE(*owned);

  std::expected<void, ConnectError> done;
  EXPECT_NO_THROW(done.value());
  EXPECT_NO_THROW(std::as_const(done).value());
  EXPECT_NO_THROW(std::move(done).value());
  EXPECT_NO_THROW(std::move(std::as_const(done)).value());
}

TEST(ExpectedError, UnrelatedErrorKeepsStandardException)
{
  std::expected<int, int> result = std::unexpected(17);
  try {
    (void)result.value();
    FAIL() << "value() did not throw";
  } catch (const std::bad_expected_access<int> &e) {
    EXPECT_EQ(e.error(), 17);
  }
}

TEST(MultiError, PreservesAlternatives)
{
  ConnectError error = Errno{EACCES};
  ASSERT_TRUE(std::holds_alternative<Errno>(error));
  EXPECT_EQ(std::get<Errno>(error).raw_code(), EACCES);
  EXPECT_EQ(error.raw_code(), EACCES);
  EXPECT_EQ(error.error_code(), Errno{EACCES}.error_code());

  error = GaiError{EAI_NONAME};
  ASSERT_TRUE(std::holds_alternative<GaiError>(error));
  EXPECT_EQ(std::get<GaiError>(error).raw_code(), EAI_NONAME);
  EXPECT_EQ(error.raw_code(), EAI_NONAME);
  EXPECT_EQ(error.error_code(), GaiError{EAI_NONAME}.error_code());
}

TEST(MultiError, ErrorBaseOperations)
{
  for (const ConnectError error :
       {ConnectError{Errno{EACCES}}, ConnectError{GaiError{EAI_NONAME}}}) {
    auto code = std::visit([](const auto &e) { return e.error_code(); }, error);
    EXPECT_EQ(error.raw_code(), code.value());
    EXPECT_EQ(make_error_code(error), code);
    std::error_code converted = error;
    EXPECT_EQ(converted, code);
    EXPECT_EQ(make_error_condition(error), code.default_error_condition());
    EXPECT_EQ(std::format("{}", error), code.message());
    try {
      error.raise("operation {}", 42);
      FAIL() << "raise() did not throw";
    } catch (const std::system_error &e) {
      EXPECT_EQ(e.code(), code);
      EXPECT_NE(std::string(e.what()).find("operation 42"), std::string::npos);
    }
  }
}

TEST(MultiError, ConvertsExpectedErrors)
{
  auto from_errno = [] -> std::expected<int, ConnectError> {
    return Errno{EACCES};
  }();
  ASSERT_FALSE(from_errno);
  ASSERT_TRUE(std::holds_alternative<Errno>(from_errno.error()));
  EXPECT_EQ(from_errno.error().raw_code(), EACCES);

  auto from_gai = [] -> std::expected<int, ConnectError> {
    return GaiError{EAI_NONAME};
  }();
  ASSERT_FALSE(from_gai);
  ASSERT_TRUE(std::holds_alternative<GaiError>(from_gai.error()));
  EXPECT_EQ(from_gai.error().raw_code(), EAI_NONAME);

  std::expected<int, Errno> single = Errno{ECONNREFUSED};
  std::expected<int, ConnectError> multiple = single;
  ASSERT_FALSE(multiple);
  ASSERT_TRUE(std::holds_alternative<Errno>(multiple.error()));
  EXPECT_EQ(multiple.error().raw_code(), ECONNREFUSED);
}

TEST(MultiError, TcpConnectAddressError)
{
  // Resolution uses AI_NUMERICHOST, so this fails without a DNS query.
  bool prepared = false;
  auto result = tcpconnect("not-an-ip-address", uint16_t{80},
                           [&](int) { prepared = true; });
  ASSERT_FALSE(result);
  EXPECT_FALSE(prepared);
  ASSERT_TRUE(std::holds_alternative<GaiError>(result.error()));
  auto code = GaiError{EAI_NONAME}.error_code();
  EXPECT_EQ(result.error().error_code(), code);
  check_value_error(std::move(result), code);
}

TEST(MultiError, TcpConnectRefused)
{
  auto addr = HostPort{"127.0.0.1", 0}.to_addr().value();
  UniqueFd reserved{socket(AF_INET, SOCK_STREAM, 0)};
  ASSERT_TRUE(reserved);
  ASSERT_EQ(bind(reserved.get(), reinterpret_cast<const sockaddr *>(&addr),
                 sockaddr_len(addr)),
            0);
  auto port = get_port(local_addr(reserved));
  // Darwin drops SYNs to bound, non-listening sockets instead of refusing.
  reserved.reset();
  auto result = tcpconnect("127.0.0.1", port);
  ASSERT_FALSE(result);
  ASSERT_TRUE(std::holds_alternative<Errno>(result.error()));
  auto code = Errno{ECONNREFUSED}.error_code();
  EXPECT_EQ(result.error().error_code(), code);
  check_value_error(std::move(result), code);
}

TEST(MultiError, TcpConnectSuccess)
{
  auto addr = HostPort{"127.0.0.1", 0}.to_addr().value();
  auto listener = stcplisten(addr);
  auto port = get_port(local_addr(listener));
  int prepared_fd = -1;
  auto result =
      tcpconnect("127.0.0.1", port, [&](int fd) { prepared_fd = fd; });
  ASSERT_TRUE(result);
  auto client = std::move(result).value();
  ASSERT_TRUE(client);
  EXPECT_EQ(client.get(), prepared_fd);
  EXPECT_EQ(get_port(remote_addr(client)), port);
}

TEST(ExpectedError, CxsValueThrowsSystemError)
{
  using enum CxsErrorCode;
  for (auto code : {kTruncated, kNonCanonical, kBadDiscriminant,
                    kLengthExceeded, kRecursionLimit, kTrailingData}) {
    CxsError error{code};
    check_value_overloads<int>(error);
    check_value_overloads<void>(error);
    EXPECT_EQ(error, code);
    EXPECT_EQ(error, error.error_code());
    EXPECT_EQ(code, error.error_code());
    EXPECT_NE(error, Errno{error.raw_code()}.error_code());
    EXPECT_EQ(std::string_view(error.category().name()), "cxs");
  }
  EXPECT_EQ(CxsError{CxsErrorCode(999)}.error_code().message(),
            "unknown cxs error");
}
