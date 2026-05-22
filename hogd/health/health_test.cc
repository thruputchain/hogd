#include "hogd/health/health.h"

#include "hogd/base/json.h"
#include "hogd/base/test.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "gtest/gtest.h"

namespace hogd {
namespace {

std::string
read_file(const std::filesystem::path &path)
{
  std::ifstream in(path);
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
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

  Health::instance().dump_to_file(path.c_str());

  const std::string contents = read_file(path);
  ASSERT_FALSE(contents.empty());

  auto parsed = json::parse(contents);
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message;

  const json::Value *timestamp = parsed->find("last_updated_timestamp");
  ASSERT_NE(timestamp, nullptr);
  ASSERT_TRUE(timestamp->as_string().has_value());
  EXPECT_FALSE(timestamp->as_string()->empty());

  const json::Value *status = parsed->find("status");
  ASSERT_NE(status, nullptr);
  EXPECT_EQ(status->as_string(), "ok");

  const json::Value *current_block_height =
      parsed->find("current_block_height");
  ASSERT_NE(current_block_height, nullptr);
  EXPECT_EQ(current_block_height->as_integer(), 0);
}

} // namespace
} // namespace hogd
