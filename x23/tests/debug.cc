#include <cstdlib>
#include <filesystem>
#include <format>
#include <limits>
#include <map>
#include <string>
#include <string_view>

#include <unistd.h>

#include <gtest/gtest.h>

#include "x23/config.h"
#include "x23/debug.h"
#include "x23/fileio.h"
#include "x23/progname.h"

using namespace x23;

TEST(DebugFormat, PercentSubstitution)
{
  const std::map<char, std::string> replacements{{'f', "file"}, {'p', "123"}};
  EXPECT_EQ(replace_percent("", replacements), "");
  EXPECT_EQ(replace_percent("plain", replacements), "plain");
  EXPECT_EQ(replace_percent("%f.%p.%%.%x.%", replacements), "file.123.%.%x.%");
  EXPECT_EQ(replace_percent("%%%f", replacements), "%file");
  EXPECT_EQ(replace_percent("%f%f", replacements), "filefile");
  EXPECT_EQ(expand_log_path("./%f.%p.%%"),
            std::format("./{}.{}.%", x23::getprogname(), getpid()));
}

TEST(DebugFormat, ArgumentQuoting)
{
  std::string out;
  append_arg(&out, "simple");
  append_arg(&out, "");
  append_arg(&out, "two words");
  append_arg(&out, "a\"b\\c");
  append_arg(&out, "tab\there");
  EXPECT_EQ(out, "simple \"\" \"two words\" \"a\\\"b\\\\c\" \"tab\there\"");
}

TEST(DebugFormat, LinesAndProgramPrefix)
{
  int value = 42;
  EXPECT_EQ(vformat_line("prefix: ", "value {}", std::make_format_args(value)),
            "prefix: value 42\n");
  EXPECT_EQ(vformat_logline("prefix: ", "value {}",
                            std::make_format_args(value)),
            x23::getprogname() + ": prefix: value 42\n");
}

TEST(DebugFormat, StacktraceHonorsIndent)
{
#if HAVE_STD_STACKTRACE || HAVE_BACKTRACE_SYMBOLS
  auto trace = stacktrace(7, "prefix\n");
  ASSERT_TRUE(trace.starts_with("prefix\n"));
  auto line = trace.find('\n') + 1;
  ASSERT_LT(line, trace.size());
  while (line < trace.size()) {
    EXPECT_EQ(trace.substr(line, 7), "       ");
    ASSERT_NE(trace.find('\n', line), std::string::npos);
    line = trace.find('\n', line) + 1;
  }
#else
  EXPECT_EQ(stacktrace(7, "prefix"), "prefix");
#endif
}

TEST(DebugEnvironment, OverridesAndDisable)
{
  const char *var = "X23_TEST_DEBUG_ENV";
  auto specific = std::format("{}_{}", var, x23::getprogname());
  bool saved = ignore_debug_env;
  Defer cleanup([&] {
    ignore_debug_env = saved;
    unsetenv(var);
    unsetenv(specific.c_str());
  });
  ignore_debug_env = false;
  ASSERT_EQ(unsetenv(var), 0);
  ASSERT_EQ(unsetenv(specific.c_str()), 0);
  EXPECT_TRUE(get_debug_env(var).empty());
  ASSERT_EQ(setenv(var, "general", 1), 0);
  EXPECT_EQ(get_debug_env(var), "general");
  ASSERT_EQ(setenv(specific.c_str(), "specific", 1), 0);
  EXPECT_EQ(get_debug_env(var), "specific");
  ASSERT_EQ(setenv(specific.c_str(), "", 1), 0);
  EXPECT_TRUE(get_debug_env(var).empty());
  ASSERT_EQ(setenv(specific.c_str(), "specific", 1), 0);
  ignore_debug_env = true;
  EXPECT_TRUE(get_debug_env(var).empty());
}

TEST(DebugFd, NumericAndInvalidSettings)
{
  EXPECT_EQ(make_debug_fd("0"), 0);
  EXPECT_EQ(make_debug_fd("2"), 2);
  EXPECT_EQ(make_debug_fd(",42"), 42);
  EXPECT_EQ(make_debug_fd("-,42"), 42);
  EXPECT_EQ(make_debug_fd(std::to_string(std::numeric_limits<int>::max())),
            std::numeric_limits<int>::max());
  for (auto invalid : {"", ",", "-1", "2junk", "junk", "2147483648",
                       "999999999999999999999999999999999999"}) {
    SCOPED_TRACE(invalid);
    EXPECT_EQ(make_debug_fd(invalid), -1);
  }
}

TEST(DebugFd, ExpandedFileAppends)
{
  char name[] = "debug-XXXXXX";
  ASSERT_NE(mkdtemp(name), nullptr);
  std::filesystem::path dir(name);
  Defer cleanup([&] { std::filesystem::remove_all(dir); });
  auto path = dir / std::format("{}.{}.%", x23::getprogname(), getpid());
  ASSERT_TRUE(writefile(path, std::string_view("before\n")));
  UniqueFd fd(make_debug_fd("./" + dir.string() + "/%f.%p.%%"));
  ASSERT_TRUE(fd);
  ASSERT_TRUE(fullwrite(fd, std::string_view("after\n")));
  fd.reset();
  auto data = readfile<std::string>(path);
  ASSERT_TRUE(data);
  EXPECT_EQ(*data, "before\nafter\n");
}

TEST(DebugFd, ResourceUsageOnScopeExit)
{
  EXPECT_FALSE(dump_rusage(-1));
  int fds[2];
  ASSERT_EQ(pipe(fds), 0);
  UniqueFd reader(fds[0]), writer(fds[1]);
  {
    auto usage = dump_rusage(writer.get());
    EXPECT_TRUE(usage);
  }
  writer.reset();
  std::string output;
  ASSERT_TRUE(readall(reader, &output));
  EXPECT_TRUE(output.starts_with(x23::getprogname() + " execution time:\n"));
  EXPECT_TRUE(output.contains("real"));
  EXPECT_TRUE(output.contains("user"));
  EXPECT_TRUE(output.contains("sys"));
}
