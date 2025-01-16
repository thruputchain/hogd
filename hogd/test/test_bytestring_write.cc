#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hogexec/runtime/syscall_handler.h"
#include "hogd/hogexec/runtime/test_load_wasm.h"
#include "hogd/test/local_test_harness.h"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/vec.h"

namespace hogd {

class TestBytestringWrite : public ::testing::Test,
                            public test::LocalTestHarness {
protected:
  void SetUp() override
  {
    std::printf("starting setup\n");
    auto [h, c] = test::load_built_rs_contract("test_bytestring.wasm");

    db_core_.get_root_db().write_contract(h, c);
    db_core_.get_root_db().write_deployment(addr_, h);
  }

  ContractAddress addr_ = ContractAddress{};
};

TEST_F(TestBytestringWrite, Nothing)
{}

} // namespace hogd
