#include <gtest/gtest.h>

#include "contract.hh"
#include "hogd/crypto/crypto.h"
#include "hogd/test_ffi/test_bindings.h"
#include "types.hh"
#include "x23/cleanup.h"
#include "x23/serialize.h"

namespace hogd {

namespace {

void *
make_db()
{
  auto db = create_db();
  EXPECT_EQ(db.ffi_error_code, kTestFFIErrorCodeNone);
  return db.db;
}

void *
make_runtime_context()
{
  auto rc = open_runtime_context();
  EXPECT_EQ(rc.ffi_error_code, kTestFFIErrorCodeNone);
  return rc.context;
}

} // namespace

// Simple tests to make sure that the exported test functions work on at least a
// basic level. This one, for example, catches an error in initializing hogd.
TEST(TestBindingsTest, LazyInitTest)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  free_db(db);
}

TEST(TestBindingsTest, MissingContractOnDeploymentTest)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  ContractAddress deployment_addr{};

  Contract contract{};
  auto hash = sha256(contract);

  // the contract size is for deployment costs, which we ignore in this test.
  auto result =
      setup_deploy_contract(db, deployment_addr.data(), hash.data(), 100);
  ASSERT_EQ(result, kTestFFIErrorCodeDeployNonexistentContract);
}

TEST(TestBindingsTest, MissingMockContractTest)
{
  auto *db = make_db();
  ASSERT_NE(db, nullptr);
  x23::Defer d([db] { free_db(db); });

  auto *rc = make_runtime_context();
  ASSERT_NE(rc, nullptr);
  x23::Defer d2([rc] { close_runtime_context(rc); });

  Contract contract(ContractType::kMockTest);
  contract.mock_contract_id() = 1;

  auto bytes = x23::serialize(contract);

  // the contract size is for deployment costs, which we ignore in this test.
  auto result = setup_create_contract(rc, db, bytes.data(), bytes.size());
  ASSERT_EQ(result, kTestFFIErrorCodeCreateNonexistentMockContract);
}

} // namespace hogd
