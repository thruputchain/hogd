#include <cstdint>
#include <string>
#include <string_view>

#include "hogd/base/json.h"
#include "hogd/base/test.h"
#include "hogd/base/types.h"
#include "hogd/http/http.h"
#include "hogd/http/http_test.h"
#include "hogd/metrics/health.h"
#include "hogd/metrics/shared.h"
#include "x23/progname.h"
#include "gtest/gtest.h"

#include <unistd.h>

namespace hogd {
namespace {

void
expect_json_string_field(const json::Value &object, std::string_view field,
                         std::string_view expected)
{
  const json::Value *value = object.find(field);
  ASSERT_NE(value, nullptr) << field;
  EXPECT_EQ(value->as_string(), expected) << field;
}

void
expect_json_integer_field(const json::Value &object, std::string_view field,
                          std::int64_t expected)
{
  const json::Value *value = object.find(field);
  ASSERT_NE(value, nullptr) << field;
  EXPECT_EQ(value->as_integer(), expected) << field;
}

void
expect_status_dump(Health::Status status, std::string_view expected_status)
{
  Health::instance().set_status(status)();

  auto parsed = json::parse(Health::instance().dump_to_string()());
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
  expect_json_string_field(*parsed, "status", expected_status);
}

class HealthTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    Health::instance().set_status(Health::Status::kStarting)();
    Health::instance().set_current_block_height(-1)();
  }
};

TEST_F(HealthTest, InstanceReturnsSingleton)
{
  EXPECT_EQ(&Health::instance(), &Health::instance());
}

TEST_F(HealthTest, DumpToStringReturnsJsonHealthState)
{
  const std::string contents = Health::instance().dump_to_string()();
  ASSERT_FALSE(contents.empty());

  auto parsed = json::parse(contents);
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message;

  const json::Value *process_name = parsed->find("process_name");
  ASSERT_NE(process_name, nullptr);
  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  ASSERT_TRUE(process_name->as_string().has_value());
  EXPECT_FALSE(process_name->as_string()->empty());

  expect_json_integer_field(*parsed, "pid", getpid());
  expect_json_string_field(*parsed, "executable_path", x23::getprogpath());

  const json::Value *timestamp = parsed->find("last_updated_timestamp");
  ASSERT_NE(timestamp, nullptr);
  ASSERT_TRUE(timestamp->as_string().has_value());
  EXPECT_FALSE(timestamp->as_string()->empty());
  const std::string_view timestamp_value = *timestamp->as_string();
  // NOLINTEND(bugprone-unchecked-optional-access)
  EXPECT_NE(timestamp_value.find('T'), std::string_view::npos);
  ASSERT_FALSE(timestamp_value.empty());
  EXPECT_EQ(timestamp_value.back(), 'Z');

  expect_json_string_field(*parsed, "status", "starting");
  expect_json_integer_field(*parsed, "current_block_height", -1);
}

TEST_F(HealthTest, SettersUpdateDumpedJsonHealthState)
{
  Health::instance().set_status(Health::Status::kError)();
  Health::instance().set_current_block_height(123)();

  auto parsed = json::parse(Health::instance().dump_to_string()());
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message;

  expect_json_string_field(*parsed, "status", "error");
  expect_json_integer_field(*parsed, "current_block_height", 123);
}

TEST_F(HealthTest, DumpsAllStatusValues)
{
  expect_status_dump(Health::Status::kStarting, "starting");
  expect_status_dump(Health::Status::kOk, "ok");
  expect_status_dump(Health::Status::kError, "error");
}

TEST_F(HealthTest, HealthMetricsRoutesReturnExpectedResponses)
{
  HttpServer server;

  add_health_metrics_routes(server);
  uint16_t port = server.start_server(x23::HostPort{"127.0.0.1", 0});

  std::string http_response =
      request(port, "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(http_response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
  EXPECT_NE(http_response.find("Content-Type: application/json\r\n"),
            std::string::npos);
  EXPECT_NE(http_response.find("\"process_name\""), std::string::npos);
  EXPECT_NE(http_response.find("\"status\""), std::string::npos);
}

} // namespace
} // namespace hogd
