#include <gtest/gtest.h>

#include "exec_coord.hh"
#include "hogd/hogexec/interface/executor_state_manager.h"
#include "hogd/init.h"

namespace hogd {

TEST(ExecutorStateManagerTest, TestRefcounting)
{
  ExecutorStateManager manager(nullptr);

  ASSERT_EQ(manager.open_proposal(1), ExecResult::ExecResultOK);
  ASSERT_EQ(manager.open_proposal(1), ExecResult::ExecResultOK);
  ASSERT_NE(manager.open_validation(1), ExecResult::ExecResultOK);

  manager.remove_block(1);
  ASSERT_NE(manager.open_validation(1), ExecResult::ExecResultOK);
  manager.remove_block(1);
  ASSERT_EQ(manager.open_validation(1), ExecResult::ExecResultOK);
}

} // namespace hogd

int
main(int argc, char *argv[])
{
  auto ret = hogd::hogd_init(argc, argv, false);
  if (ret != 0)
    return ret;
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
