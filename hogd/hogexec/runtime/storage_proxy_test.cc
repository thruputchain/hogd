#include <cstdint>
#include <memory>
#include <stdexcept>
#include <cstring>

#include <gtest/gtest.h>

#include "hogd/base/types.h"
#include "hogd/hogexec/runtime/storage_proxy.h"
#include "types.hh"
#include "storage_object.hh"

namespace hogd {

TEST(StorageProxy, Exists)
{
  StorageProxy proxy(std::nullopt);

  EXPECT_FALSE(proxy.exists());

  std::vector<uint8_t> bytes{0,1,2,3};
  std::vector<uint8_t> bytes2{0,1,2,3};

  EXPECT_TRUE(!!proxy.set_bytestring(std::as_bytes(std::span(bytes))));

  EXPECT_TRUE(proxy.exists());

  proxy.delete_last();

  EXPECT_TRUE(proxy.exists());

  EXPECT_TRUE(!!proxy.set_bytestring(std::as_bytes(std::span(bytes2))));
}

TEST(StorageProxy, SetBytestring)
{
  StorageProxy proxy(std::nullopt);

  auto eq = [] (Bytestring const& bstring, std::vector<uint8_t> const& vec) -> bool {
    return (bstring.size() == vec.size() && std::memcmp(bstring.data(), vec.data(), vec.size()) == 0);
  };

  std::vector<uint8_t> bytes{0,1,2,3};
  std::vector<uint8_t> bytes2{0,1,2,3};

  std::vector<uint8_t> bytes_long(kBytestringMaxLen + 10, 0);

  EXPECT_TRUE(!!proxy.set_bytestring(std::as_bytes(std::span(bytes))));

  auto get_res = proxy.get_bytestring();

  ASSERT_TRUE(!!get_res);

  EXPECT_TRUE(eq(*get_res, bytes));

  EXPECT_TRUE(!!proxy.set_bytestring(std::as_bytes(std::span(bytes2))));

  ASSERT_TRUE(!!(get_res = proxy.get_bytestring()));
  EXPECT_TRUE(eq(*get_res, bytes2));

  EXPECT_FALSE(proxy.set_bytestring(std::as_bytes(std::span(bytes_long))));

  ASSERT_TRUE(!!(get_res = proxy.get_bytestring()));
  EXPECT_TRUE(eq(*get_res, bytes2));

}


}
