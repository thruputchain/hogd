#include <gtest/gtest.h>

#include <filesystem>
#include <span>
#include <string_view>
#include <unordered_map>

#include "event.hh"
#include "hogd/base/log.h"
#include "hogd/base/string.h"
#include "hogd/base/test.h"
#include "hogd/base/xdr_util.h" // IWYU pragma: keep
#include "hogd/crypto/crypto.h"
#include "hogd/event/event_block_context.h"
#include "hogd/init.h"
#include "types.hh"
#include "x23/fileio.h"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

class EventBlockContextTest : public ::testing::Test {
public:
  EventBlockContextTest() = default;

  void SetUp() override { temp_dir_ = TmpDir().directory(); }

  void TearDown() override { std::filesystem::remove_all(temp_dir_); }

  static std::unordered_map<SHA256Hash, TxEventLog>
  parse_event_logs(std::string_view filename)
  {
    std::unordered_map<SHA256Hash, TxEventLog> event_logs;
    auto buf = x23::readfile(filename);

    x23::Vec<TxEventLog, 0xffffffff> serialized_logs;
    x23::deserialize(&serialized_logs, x23::BufView(*buf));

    for (auto &serialized_log : serialized_logs) {
      auto it =
          event_logs.insert({serialized_log.proposed_tx_hash, serialized_log});
      EXPECT_TRUE(it.second);
      if (!it.second) {
        error("duplicate event log for proposed tx hash: {}",
              encode_hex(
                  std::as_bytes(std::span(serialized_log.proposed_tx_hash))));
      }
    }

    return event_logs;
  }

  std::string temp_dir_;
};

TEST_F(EventBlockContextTest, Test)
{
  EventBlockContext context(temp_dir_, 1);
  context.add_event(
      TxEventLog{.proposed_tx_hash = sha256(std::as_bytes(std::span("test"))),
                 .events = {TxEmittedEvent{
                     .contract_address = {"test_address"},
                     .serialized_event = {1, 2, 3},
                 }}});

  context.finish_file();
  auto hash = context.get_root_hash();
  EXPECT_TRUE(std::filesystem::exists(
      EventBlockContext::final_events_filename(temp_dir_, hash)));

  auto event_logs = parse_event_logs(
      EventBlockContext::final_events_filename(temp_dir_, hash));
  EXPECT_EQ(event_logs.size(), 1);
  EXPECT_EQ(event_logs.begin()->first,
            sha256(std::as_bytes(std::span("test"))));
  EXPECT_EQ(event_logs.begin()->second.events.size(), 1);
  EXPECT_EQ(event_logs.begin()->second.events[0].contract_address,
            ContractAddress{"test_address"});
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
