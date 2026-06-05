#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#include "hogd/base/json.h"
#include "hogd/base/test.h"
#include "hogd/http/http.h"
#include "hogd/http/http_test.h"
#include "hogd/metrics/health.h"
#include "hogd/metrics/shared.h"
#include "x23/progname.h"
#include "gtest/gtest.h"

#include <unistd.h>

namespace hogd {
namespace {

std::string
read_file(const std::filesystem::path &path)
{
  std::ifstream in(path);
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

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
expect_status_dump(Health::Status status, std::string_view expected_status,
                   const std::filesystem::path &path)
{
  Health::instance().set_status(status)();
  Health::instance().dump_to_file(path)();

  auto parsed = json::parse(read_file(path));
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

TEST_F(HealthTest, DumpToFileWritesJsonHealthState)
{
  TmpDir tmp_dir;
  const std::filesystem::path path =
      std::filesystem::path(tmp_dir.directory()) / "health";

  Health::instance().dump_to_file(path)();

  const std::string contents = read_file(path);
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

TEST_F(HealthTest, DumpToFileCreatesParentDirectories)
{
  TmpDir tmp_dir;
  const std::filesystem::path path =
      std::filesystem::path(tmp_dir.directory()) / "nested" / "health" /
      "status.json";

  ASSERT_FALSE(std::filesystem::exists(path.parent_path()));

  Health::instance().set_status(Health::Status::kOk)();
  Health::instance().set_current_block_height(321)();
  Health::instance().dump_to_file(path)();

  ASSERT_TRUE(std::filesystem::exists(path));
  auto parsed = json::parse(read_file(path));
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
  expect_json_string_field(*parsed, "status", "ok");
  expect_json_integer_field(*parsed, "current_block_height", 321);
}

TEST_F(HealthTest, SettersUpdateDumpedJsonHealthState)
{
  TmpDir tmp_dir;
  const std::filesystem::path path =
      std::filesystem::path(tmp_dir.directory()) / "health";

  Health::instance().set_status(Health::Status::kError)();
  Health::instance().set_current_block_height(123)();
  Health::instance().dump_to_file(path)();

  auto parsed = json::parse(read_file(path));
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message;

  expect_json_string_field(*parsed, "status", "error");
  expect_json_integer_field(*parsed, "current_block_height", 123);
}

TEST_F(HealthTest, DumpsAllStatusValues)
{
  TmpDir tmp_dir;
  const std::filesystem::path path =
      std::filesystem::path(tmp_dir.directory()) / "health";

  expect_status_dump(Health::Status::kStarting, "starting", path);
  expect_status_dump(Health::Status::kOk, "ok", path);
  expect_status_dump(Health::Status::kError, "error", path);
}

TEST_F(HealthTest, DumpToFileOverwritesWithoutLeavingTempFiles)
{
  TmpDir tmp_dir;
  const std::filesystem::path directory = tmp_dir.directory();
  const std::filesystem::path path = directory / "health";

  Health::instance().set_status(Health::Status::kOk)();
  Health::instance().set_current_block_height(456)();
  Health::instance().dump_to_file(path)();

  Health::instance().set_status(Health::Status::kError)();
  Health::instance().set_current_block_height(789)();
  Health::instance().dump_to_file(path)();

  auto parsed = json::parse(read_file(path));
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
  expect_json_string_field(*parsed, "status", "error");
  expect_json_integer_field(*parsed, "current_block_height", 789);

  for (const auto &entry : std::filesystem::directory_iterator(directory)) {
    EXPECT_EQ(entry.path().filename(), path.filename());
  }
}

TEST_F(HealthTest, HealthMetricsRoutesReturnExpectedResponses)
{
  HttpServer server;

  add_health_metrics_routes(server);
  uint16_t port = server.start_server(x23::HostPort{"127.0.0.1", 0});

  std::string health =
      request(port, "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(health.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
  EXPECT_NE(health.find("Content-Type: application/json\r\n"),
            std::string::npos);
  EXPECT_NE(health.find("\"process_name\""), std::string::npos);
  EXPECT_NE(health.find("\"status\""), std::string::npos);

  std::string metrics =
      request(port, "GET /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(metrics.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
  EXPECT_NE(metrics.find("Content-Type: text/plain\r\n"), std::string::npos);
  EXPECT_NE(metrics.find("Content-Length: 0\r\n"), std::string::npos);
  EXPECT_TRUE(metrics.ends_with("\r\n\r\n"));
}

} // namespace
} // namespace hogd
