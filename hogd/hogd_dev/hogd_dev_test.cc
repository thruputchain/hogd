#include <sys/poll.h>
#include <sys/wait.h>
// NOLINTNEXTLINE(modernize-deprecated-headers)
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <gtest/gtest.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "built_contracts/contract_loader.h"
#include "built_contracts/fibonacci.h"
#include "db_sched.hh"
#include "event.hh"
#include "exec_result.hh"
#include "hogd/base/network.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/init.h"
#include "hogd/mempool/shared.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/version.h"
#include "indexer.hh"
#include "sched_coord.hh"
#include "transaction.hh"
#include "types.hh"

#ifndef HOGD_DEV_BINARY
#error "HOGD_DEV_BINARY must be defined by CMake"
#endif

namespace hogd {

namespace {

constexpr auto kStartupTimeout = std::chrono::seconds(10);
constexpr auto kShutdownTimeout = std::chrono::seconds(5);
constexpr auto kIndexPollTimeout = std::chrono::seconds(10);
constexpr std::size_t kMaxReadinessBytes = 16 * 1024;

struct Readiness {
  struct Endpoint {
    std::string host;
    std::uint16_t port;
  };

  Endpoint scheduler;
  Endpoint read;
};

std::string
errno_message(std::string_view operation, int err)
{
  return std::format("{} failed: {}", operation, std::strerror(err));
}

void
close_if_open(int &fd)
{
  if (fd >= 0) {
    close(fd);
    fd = -1;
  }
}

std::string
read_to_end(int fd)
{
  std::string output;
  for (;;) {
    char buffer[4096];
    auto count = read(fd, buffer, sizeof(buffer));
    if (count > 0) {
      output.append(buffer, static_cast<std::size_t>(count));
      continue;
    }
    if (count == 0) {
      return output;
    }
    if (errno == EINTR) {
      continue;
    }
    throw std::runtime_error(errno_message("read", errno));
  }
}

Readiness::Endpoint
parse_endpoint(std::string endpoint, std::string_view name,
               std::string_view line)
{
  std::string host;
  std::string port_text;
  if (endpoint.starts_with('[')) {
    std::size_t close_bracket = endpoint.find(']');
    if (close_bracket == std::string::npos ||
        close_bracket + 1 >= endpoint.size() ||
        endpoint[close_bracket + 1] != ':') {
      throw std::runtime_error(
          std::format("unexpected {} endpoint in readiness: {}", name, line));
    }
    host = endpoint.substr(1, close_bracket - 1);
    port_text = endpoint.substr(close_bracket + 2);
  }
  else {
    std::size_t colon = endpoint.rfind(':');
    if (colon == std::string::npos) {
      throw std::runtime_error(
          std::format("unexpected {} endpoint in readiness: {}", name, line));
    }
    host = endpoint.substr(0, colon);
    port_text = endpoint.substr(colon + 1);
  }

  std::size_t pos = 0;
  std::uint64_t port = std::stoul(port_text, &pos, 10);
  if (pos != port_text.size() || port == 0 || port > 65535) {
    throw std::runtime_error(
        std::format("unexpected {} port in readiness: {}", name, line));
  }

  return {.host = std::move(host), .port = static_cast<std::uint16_t>(port)};
}

Readiness
parse_readiness(std::string line)
{
  if (!line.empty() && line.back() == '\n') {
    line.pop_back();
  }

  const std::string prefix =
      R"({"status":"ready","network_id":"groundhog-local","endpoints":{"scheduler":")";
  const std::string middle = R"(","read":")";
  const std::string suffix = R"("}})";
  if (!line.starts_with(prefix) || !line.ends_with(suffix)) {
    throw std::runtime_error(
        std::format("unexpected readiness line: {}", line));
  }

  std::string endpoints =
      line.substr(prefix.size(), line.size() - prefix.size() - suffix.size());
  std::size_t read_delimiter = endpoints.find(middle);
  if (read_delimiter == std::string::npos) {
    throw std::runtime_error(
        std::format("unexpected readiness line: {}", line));
  }

  std::string scheduler = endpoints.substr(0, read_delimiter);
  std::string read = endpoints.substr(read_delimiter + middle.size());

  return {.scheduler = parse_endpoint(std::move(scheduler), "scheduler", line),
          .read = parse_endpoint(std::move(read), "read", line)};
}

SHA256Hash
make_nonce(std::uint64_t nonce)
{
  SHA256Hash out{};
  std::memcpy(out.data(), &nonce, sizeof(nonce));
  return out;
}

GenesisConfigurationBuilder
make_fibonacci_genesis_builder()
{
  auto fibonacci_contract = load_contract("fibonacci.wasm");
  using fibonacci_meta_t = ContractTraitMeta<fibonacci::Fibonacci>;

  GenesisConfigurationBuilder builder;
  builder.add_initial_config(fibonacci_contract, make_nonce(0),
                             fibonacci_meta_t::entrypoint<"fibonacci">(),
                             fibonacci_meta_t::make_calldata<"fibonacci">(
                                 std::uint32_t{0}),
                             int128_t{1'000'000'000});
  return builder;
}

class HogdDevProcess {
public:
  static HogdDevProcess start()
  {
    int stdout_pipe[2];
    int stderr_pipe[2];
    if (pipe(stdout_pipe) != 0) {
      throw std::runtime_error(errno_message("pipe stdout", errno));
    }
    if (pipe(stderr_pipe) != 0) {
      int saved_errno = errno;
      close(stdout_pipe[0]);
      close(stdout_pipe[1]);
      throw std::runtime_error(errno_message("pipe stderr", saved_errno));
    }

    auto pid = fork();
    if (pid < 0) {
      int saved_errno = errno;
      close(stdout_pipe[0]);
      close(stdout_pipe[1]);
      close(stderr_pipe[0]);
      close(stderr_pipe[1]);
      throw std::runtime_error(errno_message("fork", saved_errno));
    }

    if (pid == 0) {
      close(stdout_pipe[0]);
      close(stderr_pipe[0]);
      if (dup2(stdout_pipe[1], STDOUT_FILENO) < 0 ||
          dup2(stderr_pipe[1], STDERR_FILENO) < 0) {
        _exit(127);
      }
      close(stdout_pipe[1]);
      close(stderr_pipe[1]);
      execl(HOGD_DEV_BINARY, HOGD_DEV_BINARY, "--listen-host", "127.0.0.1",
            "--scheduler-port", "0", "--readiness", "stdout",
            static_cast<char *>(nullptr));
      _exit(127);
    }

    close(stdout_pipe[1]);
    close(stderr_pipe[1]);
    return HogdDevProcess(pid, stdout_pipe[0], stderr_pipe[0]);
  }

  HogdDevProcess(HogdDevProcess const &) = delete;
  HogdDevProcess &operator=(HogdDevProcess const &) = delete;

  HogdDevProcess(HogdDevProcess &&other) noexcept
    : pid_(other.pid_), stdout_fd_(other.stdout_fd_),
      stderr_fd_(other.stderr_fd_),
      stdout_buffer_(std::move(other.stdout_buffer_))
  {
    other.pid_ = -1;
    other.stdout_fd_ = -1;
    other.stderr_fd_ = -1;
  }

  HogdDevProcess &operator=(HogdDevProcess &&) = delete;

  ~HogdDevProcess()
  {
    if (pid_ > 0) {
      // NOLINTNEXTLINE(misc-include-cleaner)
      kill(pid_, SIGKILL);
      int status = 0;
      while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
      }
    }
    close_if_open(stdout_fd_);
    close_if_open(stderr_fd_);
  }

  Readiness wait_for_readiness()
  {
    return parse_readiness(read_stdout_line(kStartupTimeout));
  }

  int terminate()
  {
    if (pid_ <= 0) {
      throw std::runtime_error("hogd-dev process already reaped");
    }
    if (kill(pid_, SIGTERM) != 0 && errno != ESRCH) {
      throw std::runtime_error(errno_message("kill SIGTERM", errno));
    }

    std::optional<int> status = wait_for_exit(kShutdownTimeout);
    if (!status.has_value()) {
      kill(pid_, SIGKILL);
      status = wait_for_exit(kShutdownTimeout);
      if (!status.has_value()) {
        throw std::runtime_error("hogd-dev did not exit after SIGKILL");
      }
    }
    return *status;
  }

  std::string read_remaining_stdout()
  {
    std::string output = std::move(stdout_buffer_);
    stdout_buffer_.clear();
    if (stdout_fd_ >= 0) {
      output += read_to_end(stdout_fd_);
      close_if_open(stdout_fd_);
    }
    return output;
  }

  std::string read_remaining_stderr()
  {
    if (stderr_fd_ < 0) {
      return {};
    }
    std::string output = read_to_end(stderr_fd_);
    close_if_open(stderr_fd_);
    return output;
  }

private:
  HogdDevProcess(int pid, int stdout_fd, int stderr_fd)
    : pid_(pid), stdout_fd_(stdout_fd), stderr_fd_(stderr_fd)
  {}

  std::string read_stdout_line(std::chrono::milliseconds timeout)
  {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
      std::size_t newline = stdout_buffer_.find('\n');
      if (newline != std::string::npos) {
        std::string line = stdout_buffer_.substr(0, newline + 1);
        stdout_buffer_.erase(0, newline + 1);
        return line;
      }
      if (stdout_buffer_.size() > kMaxReadinessBytes) {
        throw std::runtime_error("hogd-dev readiness line exceeded limit");
      }

      auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        throw std::runtime_error("timed out waiting for hogd-dev readiness");
      }
      auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);

      pollfd poll_fd{.fd = stdout_fd_, .events = POLLIN, .revents = 0};
      int ready = poll(&poll_fd, 1, static_cast<int>(remaining.count()));
      if (ready < 0) {
        if (errno == EINTR) {
          continue;
        }
        throw std::runtime_error(errno_message("poll stdout", errno));
      }
      if (ready == 0) {
        throw std::runtime_error("timed out waiting for hogd-dev readiness");
      }

      char buffer[1024];
      auto count = read(stdout_fd_, buffer, sizeof(buffer));
      if (count > 0) {
        stdout_buffer_.append(buffer, static_cast<std::size_t>(count));
        continue;
      }
      if (count == 0) {
        throw std::runtime_error("hogd-dev stdout closed before readiness");
      }
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error(errno_message("read stdout", errno));
    }
  }

  std::optional<int> wait_for_exit(std::chrono::milliseconds timeout)
  {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
      int status = 0;
      auto result = waitpid(pid_, &status, WNOHANG);
      if (result == pid_) {
        pid_ = -1;
        return status;
      }
      if (result < 0) {
        if (errno == EINTR) {
          continue;
        }
        throw std::runtime_error(errno_message("waitpid", errno));
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return std::nullopt;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  int pid_;
  int stdout_fd_;
  int stderr_fd_;
  std::string stdout_buffer_;
};

void
expect_clean_exit(HogdDevProcess &process)
{
  int status = process.terminate();
  std::string stderr_output = process.read_remaining_stderr();
  ASSERT_TRUE(WIFEXITED(status)) << stderr_output;
  EXPECT_EQ(WEXITSTATUS(status), EXIT_SUCCESS) << stderr_output;
  EXPECT_TRUE(process.read_remaining_stdout().empty());
}

TEST(HogdDev, EmitsOneReadinessLineAndStopsOnSigterm)
{
  HogdDevProcess process = HogdDevProcess::start();
  Readiness readiness = process.wait_for_readiness();

  EXPECT_EQ(readiness.scheduler.host, "127.0.0.1");
  EXPECT_GT(readiness.scheduler.port, 0);
  EXPECT_EQ(readiness.read.host, "127.0.0.1");
  EXPECT_GT(readiness.read.port, 0);

  expect_clean_exit(process);
}

TEST(HogdDev, FormatsSchedulerEndpointAsCanonicalHostPort)
{
  EXPECT_EQ(format_advertised_address("127.0.0.1", 12345), "127.0.0.1:12345");
  EXPECT_EQ(format_advertised_address("::1", 12345), "[::1]:12345");
}

TEST(HogdDev, ReadinessParserAcceptsBracketedIpv6Endpoints)
{
  Readiness readiness = parse_readiness(
      R"({"status":"ready","network_id":"groundhog-local","endpoints":{"scheduler":"[::1]:12345","read":"[::1]:23456"}})");

  EXPECT_EQ(readiness.scheduler.host, "::1");
  EXPECT_EQ(readiness.scheduler.port, 12345);
  EXPECT_EQ(readiness.read.host, "::1");
  EXPECT_EQ(readiness.read.port, 23456);
}

TEST(HogdDev, SchedulerTipReportsNoGenesisThenGenesisTip)
{
  HogdDevProcess process = HogdDevProcess::start();
  Readiness readiness = process.wait_for_readiness();

  {
    sched_coord_client_t client =
        create_client<SchedulerCoord1>(readiness.scheduler.host,
                                       readiness.scheduler.port,
                                       /*retry=*/true)();

    SchedulerGetTipResult before = client.schedcoord_get_tip()();
    ASSERT_EQ(before.outcome(),
              SchedulerGetTipOutcome::SchedulerGetTipNoGenesisBlock);

    GenesisConfigurationBuilder builder;
    ASSERT_EQ(client.schedcoord_load_genesis_configuration(
                  builder.build(StorageCostModel::default_cost_model()))(),
              DbSchedResult::DbSchedResultOK);

    SchedulerGetTipResult after = client.schedcoord_get_tip()();
    ASSERT_EQ(after.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    EXPECT_EQ(after.db_tip().height, 0);
    EXPECT_EQ(after.db_tip().id, 0UL);
  }

  expect_clean_exit(process);
}

TEST(HogdDev, ReadEndpointServesIndexerBlockExplorer)
{
  HogdDevProcess process = HogdDevProcess::start();
  Readiness readiness = process.wait_for_readiness();

  {
    auto client = create_client<IndexerBlockExplorer1>(readiness.read.host,
                                                       readiness.read.port,
                                                       /*retry=*/true)();

    EXPECT_EQ(client.indexer_block_explorer_intro(version::kHogdProtocol())(),
              IndexerBlockExplorerIntroOutcome::IntroSuccess);

    SHA256Hash tx_hash{};
    TxInfoResult result = client.get_transaction_info(tx_hash)();
    EXPECT_EQ(result.outcome(), TxInfoOutcome::TxInfoNotFound);
  }

  expect_clean_exit(process);
}

TEST(HogdDev, CommittedTransactionsPopulateReadEndpoint)
{
  HogdDevProcess process = HogdDevProcess::start();
  Readiness readiness = process.wait_for_readiness();

  {
    sched_coord_client_t scheduler =
        create_client<SchedulerCoord1>(readiness.scheduler.host,
                                       readiness.scheduler.port,
                                       /*retry=*/true)();
    auto reader = create_client<IndexerBlockExplorer1>(readiness.read.host,
                                                       readiness.read.port,
                                                       /*retry=*/true)();

    EXPECT_EQ(reader.indexer_block_explorer_intro(version::kHogdProtocol())(),
              IndexerBlockExplorerIntroOutcome::IntroSuccess);

    GenesisConfigurationBuilder builder = make_fibonacci_genesis_builder();
    ASSERT_EQ(scheduler.schedcoord_load_genesis_configuration(
                  builder.build(StorageCostModel::default_cost_model()))(),
              DbSchedResult::DbSchedResultOK);

    std::vector<SignedTransaction> unfurl_txs =
        builder.make_unfurl_genesis_transactions();
    ASSERT_EQ(unfurl_txs.size(), 1UL);
    const SignedTransactionHash tx_hash = sha256(unfurl_txs[0].tx);

    scheduler.schedulercoord_submit_unvetted(
        t_to_parallel_batch(SignedTransactionBatch{unfurl_txs[0]})())();

    SchedulerGetTipResult tip = scheduler.schedcoord_get_tip()();
    ASSERT_EQ(tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    SchedulerNominationResult nomination =
        scheduler.schedulercoord_nominate(tip.db_tip())();
    ASSERT_EQ(nomination.outcome(),
              SchedulerNominationOutcome::SchedulerNominationOK);
    ASSERT_EQ(scheduler.schedulercoord_commit_block(
                  nomination.nominated_block().block_id)(),
              SchedulerCommitOutcome::SchedulerCommitOK);

    const auto deadline = std::chrono::steady_clock::now() + kIndexPollTimeout;
    TxInfoResult result(TxInfoOutcome::TxInfoNotFound);
    while (std::chrono::steady_clock::now() < deadline) {
      result = reader.get_transaction_info(tx_hash)();
      if (result.outcome() == TxInfoOutcome::TxInfoFound) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_EQ(result.outcome(), TxInfoOutcome::TxInfoFound);
    EXPECT_EQ(result.info().height, 1);
    EXPECT_EQ(result.info().events.exec_result, ExecResult::ExecResultOK);
    EXPECT_EQ(result.info().events.proposed_tx_hash, tx_hash);
    ASSERT_FALSE(result.info().events.events.empty());
    EXPECT_TRUE(std::ranges::any_of(
        result.info().events.events, [](TxEmittedEvent const &event) {
          return event.contract_address ==
                 GenesisConfigurationLoader::kPresetNativeTokenAddress;
        }));
  }

  expect_clean_exit(process);
}

} // namespace

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
