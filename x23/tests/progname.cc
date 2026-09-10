#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include <gtest/gtest.h>

#include "x23/fileio.h"
#include "x23/progname.h"

using namespace x23;
namespace fs = std::filesystem;

namespace {

class Progname : public ::testing::Test {
protected:
  fs::path dir_;
  std::optional<std::string> saved_path_;
  std::string saved_progpath_;

  void SetUp() override
  {
    if (const char *path = std::getenv("PATH"))
      saved_path_ = path;
    saved_progpath_ = getprogpath();
    // CTest runs in the build directory, on local storage.
    char name[] = "progname-XXXXXX";
    ASSERT_NE(mkdtemp(name), nullptr);
    dir_ = fs::absolute(name);
    ASSERT_TRUE(fs::create_directory(dir_ / "first"));
    ASSERT_TRUE(fs::create_directory(dir_ / "second"));
    auto path = (dir_ / "first").string() + ":" + (dir_ / "second").string();
    ASSERT_EQ(setenv("PATH", path.c_str(), 1), 0);
  }

  void TearDown() override
  {
    if (saved_path_)
      EXPECT_EQ(setenv("PATH", saved_path_->c_str(), 1), 0);
    else
      EXPECT_EQ(unsetenv("PATH"), 0);
    setprogpath(saved_progpath_);
    if (!dir_.empty()) {
      std::error_code ec;
      fs::remove_all(dir_, ec);
      EXPECT_FALSE(ec) << ec.message();
    }
  }
};

} // namespace

TEST_F(Progname, ExplicitPaths)
{
  EXPECT_TRUE(find_exe("").empty());
  EXPECT_EQ(find_exe(dir_ / "missing"), dir_ / "missing");
  EXPECT_EQ(find_exe("subdir/program"), fs::absolute("subdir/program"));
  EXPECT_EQ(find_exe("./program"), fs::absolute("./program"));
}

TEST_F(Progname, SearchOrderAndExecutablePermission)
{
  auto first = dir_ / "first/program";
  auto second = dir_ / "second/program";
  ASSERT_TRUE(writefile(first, std::string_view("#!/bin/sh\n"), true, 0600));
  ASSERT_TRUE(writefile(second, std::string_view("#!/bin/sh\n"), true, 0700));
  EXPECT_EQ(find_exe("program"), second);
  fs::permissions(first, fs::perms::owner_exec, fs::perm_options::add);
  EXPECT_EQ(find_exe("program"), first);
}

TEST_F(Progname, SkipsDirectories)
{
  ASSERT_TRUE(fs::create_directory(dir_ / "first/program"));
  auto second = dir_ / "second/program";
  ASSERT_TRUE(writefile(second, std::string_view("#!/bin/sh\n"), true, 0700));
  EXPECT_EQ(find_exe("program"), second);
}

TEST_F(Progname, FollowsExecutableSymlinks)
{
  auto target = dir_ / "second/program";
  auto link = dir_ / "first/program";
  ASSERT_TRUE(writefile(target, std::string_view("#!/bin/sh\n"), true, 0700));
  fs::create_symlink(target, link);
  EXPECT_EQ(find_exe("program"), link);
  fs::remove(target);
  EXPECT_EQ(find_exe("program"), "program");
}

TEST_F(Progname, EmptyPathComponentSearchesCurrentDirectory)
{
  auto program = dir_ / "program";
  ASSERT_TRUE(writefile(program, std::string_view("#!/bin/sh\n"), true, 0700));
  auto cwd = fs::current_path();
  Defer restore([&] { fs::current_path(cwd); });
  fs::current_path(dir_);
  for (const char *path : {":missing", "missing::missing", "missing:"}) {
    ASSERT_EQ(setenv("PATH", path, 1), 0);
    EXPECT_EQ(find_exe("program"), "./program");
  }
}

TEST_F(Progname, UnsetOrEmptyPathAndMissingProgram)
{
  EXPECT_EQ(find_exe("missing"), "missing");
  ASSERT_EQ(unsetenv("PATH"), 0);
  EXPECT_EQ(find_exe("missing"), "missing");
  ASSERT_EQ(setenv("PATH", "", 1), 0);
  EXPECT_EQ(find_exe("missing"), "missing");
}

TEST_F(Progname, ExplicitAndSearchedProgramNames)
{
  setprogpath("relative/program");
  EXPECT_EQ(getprogname(), "program");
  EXPECT_EQ(getprogpath(), "relative/program");
  auto program = dir_ / "second/program";
  ASSERT_TRUE(writefile(program, std::string_view("#!/bin/sh\n"), true, 0700));
  setprogpath("program");
  EXPECT_EQ(getprogname(), "program");
  EXPECT_EQ(getprogpath(), program.string());
}
