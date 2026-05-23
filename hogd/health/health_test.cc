#include "hogd/health/health.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#include "hogd/base/json.h"
#include "hogd/base/test.h"
#include "gtest/gtest.h"
#include "x23/progname.h"

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
expect_json_string_field(const json::Value &object,
                         std::string_view field,
                         std::string_view expected)
{
  const json::Value *value = object.find(field);
  ASSERT_NE(value, nullptr) << field;
  EXPECT_EQ(value->as_string(), expected) << field;
}

void
expect_json_integer_field(const json::Value &object,
                          std::string_view field,
                          std::int64_t expected)
{
  const json::Value *value = object.find(field);
  ASSERT_NE(value, nullptr) << field;
  EXPECT_EQ(value->as_integer(), expected) << field;
}

void
expect_status_dump(Health::Status status,
                   std::string_view expected_status,
                   const std::filesystem::path &path)
{
  Health::instance().set_status(status)();
  Health::instance().dump_to_file(path)();

  auto parsed = json::parse(read_file(path));
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
  expect_json_string_field(*parsed, "status", expected_status);
}

TEST(HealthTest, InstanceReturnsSingleton)
{
  EXPECT_EQ(&Health::instance(), &Health::instance());
}

TEST(HealthTest, DumpToFileWritesJsonHealthState)
{
  TmpDir tmp_dir;
  const std::filesystem::path path =
      std::filesystem::path(tmp_dir.directory()) / "health";

  Health::instance().set_status(Health::Status::kStarting)();
  Health::instance().set_current_block_height(0)();
  Health::instance().dump_to_file(path)();

  const std::string contents = read_file(path);
  ASSERT_FALSE(contents.empty());

  auto parsed = json::parse(contents);
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message;

  const json::Value *process_name = parsed->find("process_name");
  ASSERT_NE(process_name, nullptr);
  ASSERT_TRUE(process_name->as_string().has_value());
  EXPECT_FALSE(process_name->as_string()->empty());

  expect_json_integer_field(*parsed, "pid", getpid());
  expect_json_string_field(*parsed, "executable_path", x23::getprogpath());

  const json::Value *timestamp = parsed->find("last_updated_timestamp");
  ASSERT_NE(timestamp, nullptr);
  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  ASSERT_TRUE(timestamp->as_string().has_value());
  EXPECT_FALSE(timestamp->as_string()->empty());
  const std::string_view timestamp_value = *timestamp->as_string();
  // NOLINTEND(bugprone-unchecked-optional-access)
  EXPECT_NE(timestamp_value.find('T'), std::string_view::npos);
  ASSERT_FALSE(timestamp_value.empty());
  EXPECT_EQ(timestamp_value.back(), 'Z');

  expect_json_string_field(*parsed, "status", "starting");
  expect_json_integer_field(*parsed, "current_block_height", 0);
}

TEST(HealthTest, SettersUpdateDumpedJsonHealthState)
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

  Health::instance().set_status(Health::Status::kStarting)();
  Health::instance().set_current_block_height(0)();
}

TEST(HealthTest, DumpsAllStatusValues)
{
  TmpDir tmp_dir;
  const std::filesystem::path path =
      std::filesystem::path(tmp_dir.directory()) / "health";

  expect_status_dump(Health::Status::kStarting, "starting", path);
  expect_status_dump(Health::Status::kOk, "ok", path);
  expect_status_dump(Health::Status::kError, "error", path);

  Health::instance().set_status(Health::Status::kStarting)();
}

TEST(HealthTest, DumpToFileOverwritesWithoutLeavingTempFiles)
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

  Health::instance().set_status(Health::Status::kStarting)();
  Health::instance().set_current_block_height(0)();
}

} // namespace
} // namespace hogd
