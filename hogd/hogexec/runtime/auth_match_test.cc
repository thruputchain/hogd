#include <gtest/gtest.h>

#include "auth.hh"
#include "hogd/hogexec/runtime/auth_entry_matcher.h"

namespace hogd {

class AuthMatchTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    // For simplicity, this test uses every auth context as the same
    // ContractAddress 
    //
    // AAA
    //   BBB
    //   CCC
    //   DDD
    //   BBB
    // BBB
    // AAA
    //   CCC
    //     EEE -- check that this does not match AAAA-CCC earlier

    entry_.metadata.auth_flags = 0;
    entry_.metadata.signature_nonce = 0;

    auto make_node = [](std::string str) -> AuthEntryNode {
      AuthEntryNode node;
      node.context = make_auth_context(str);
      return node;
    };

    entry_.nodes.push_back(make_node("AAA"));
    entry_.nodes.back().children.push_back(make_node("BBB"));
    entry_.nodes.back().children.push_back(make_node("CCC"));
    entry_.nodes.back().children.push_back(make_node("DDD"));
    entry_.nodes.back().children.push_back(make_node("BBB"));
    entry_.nodes.push_back(make_node("BBB"));
    entry_.nodes.push_back(make_node("AAA"));
    entry_.nodes.back().children.push_back(make_node("CCC"));
    entry_.nodes.back().children.back().children.push_back(make_node("EEE"));
  }

  static AuthContext make_auth_context(std::string str)
  {
    AuthContext ctx;
    ctx.address = ContractAddress{};
    for (auto const c : str) {
      ctx.bytes.push_back(c);
    }
    return ctx;
  }

  AuthEntry entry_;
};

TEST_F(AuthMatchTest, MatchSimple)
{

  AuthContextStack stack({make_auth_context("BBB")});

  AuthEntryMatcher matcher(entry_);

  EXPECT_TRUE(matcher.try_match(stack));
}

TEST_F(AuthMatchTest, NoMatchSimple)
{

  AuthContextStack stack({make_auth_context("XYZ")});

  AuthEntryMatcher matcher(entry_);

  EXPECT_FALSE(matcher.try_match(stack));
}

TEST_F(AuthMatchTest, StackedMatch)
{

  AuthContextStack stack({make_auth_context("AAA"), make_auth_context("BBB")});

  AuthEntryMatcher matcher(entry_);

  EXPECT_TRUE(matcher.try_match(stack));
}

TEST_F(AuthMatchTest, GapMatch)
{

  AuthContextStack stack({make_auth_context("AAA"), make_auth_context("EEE")});

  AuthEntryMatcher matcher(entry_);

  EXPECT_TRUE(matcher.try_match(stack));
}

TEST_F(AuthMatchTest, GapMatch2)
{

  AuthContextStack stack({make_auth_context("CCC"), make_auth_context("EEE")});

  AuthEntryMatcher matcher(entry_);

  EXPECT_TRUE(matcher.try_match(stack));
}

TEST_F(AuthMatchTest, MatchConsumed)
{
  AuthEntryMatcher matcher(entry_);

  EXPECT_TRUE(matcher.try_match(
      AuthContextStack({make_auth_context("AAA"), make_auth_context("BBB")})));
  EXPECT_TRUE(matcher.try_match(
      AuthContextStack({make_auth_context("AAA"), make_auth_context("BBB")})));
}

TEST_F(AuthMatchTest, DoubleMatch)
{
  AuthEntryMatcher matcher(entry_);

  EXPECT_TRUE(matcher.try_match(
      AuthContextStack({make_auth_context("AAA"), make_auth_context("BBB")})));

  EXPECT_TRUE(matcher.try_match(
      AuthContextStack({make_auth_context("AAA"), make_auth_context("BBB")})));

  EXPECT_TRUE(matcher.try_match(AuthContextStack({make_auth_context("BBB")})));

  AuthEntryMatcher matcher2(entry_);

  // we match the first match that we find
  EXPECT_TRUE(matcher2.try_match(AuthContextStack({make_auth_context("BBB")})));

  EXPECT_TRUE(matcher2.try_match(
      AuthContextStack({make_auth_context("AAA"), make_auth_context("BBB")})));

  EXPECT_FALSE(matcher2.try_match(
      AuthContextStack({make_auth_context("AAA"), make_auth_context("BBB")})));
}

TEST_F(AuthMatchTest, OnlyLeaves)
{
  AuthEntryMatcher matcher(entry_);

  EXPECT_TRUE(matcher.try_match(
      AuthContextStack({make_auth_context("AAA"), make_auth_context("CCC")})));

  // the second AAA-CCC is not a leaf
  EXPECT_FALSE(matcher.try_match(
      AuthContextStack({make_auth_context("AAA"), make_auth_context("CCC")})));
}

} // namespace hogd
