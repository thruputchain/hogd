#include <sys/poll.h>
#include <sys/wait.h>
// NOLINTNEXTLINE(modernize-deprecated-headers)
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <format>
#include <gtest/gtest.h>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "auth_claim.hh"
#include "built_contracts/contract_loader.h"
#include "built_contracts/dev_wallet.h"
#include "built_contracts/fibonacci.h"
#include "db_gateway.hh"
#include "db_sched.hh"
#include "event.hh"
#include "exec_result.hh"
#include "hogd/base/json.h"
#include "hogd/base/network.h"
#include "hogd/base/string.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hogd_dev/dev_account_bootstrap.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/init.h"
#include "hogd/mempool/shared.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/version.h"
#include "indexer.hh"
#include "invoke_flag.hh"
#include "rpc_types.hh"
#include "sched_coord.hh"
#include "transaction.hh"
#include "types.hh"
#include "view_gateway.hh"
#include "x23/serialize.h"

#ifndef HOGD_DEV_BINARY
#error "HOGD_DEV_BINARY must be defined by CMake"
#endif

namespace hogd {

namespace {

constexpr auto kStartupTimeout = std::chrono::seconds(30);
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
  Endpoint gateway;
  Endpoint view_gateway;

  struct DevAccount {
    std::uint32_t index;
    std::string address;
    std::string public_key;
    std::uint64_t initial_storage_tokens;
  };
  std::vector<DevAccount> dev_accounts;
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
parse_endpoint(std::string_view endpoint, std::string_view name,
               std::string_view line)
{
  std::string host;
  std::string port_text;
  if (endpoint.starts_with('[')) {
    std::size_t close_bracket = endpoint.find(']');
    if (close_bracket == std::string_view::npos ||
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
    if (colon == std::string_view::npos) {
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

std::string_view
require_string_field(const json::Value &parent, std::string_view key,
                     std::string_view line)
{
  const json::Value *field = parent.find(key);
  if (field == nullptr) {
    throw std::runtime_error(
        std::format("readiness missing field '{}': {}", key, line));
  }
  std::optional<std::string_view> value = field->as_string();
  if (!value.has_value()) {
    throw std::runtime_error(
        std::format("readiness field '{}' is not a string: {}", key, line));
  }
  return *value;
}

std::int64_t
require_integer_field(const json::Value &parent, std::string_view key,
                      std::string_view line)
{
  const json::Value *field = parent.find(key);
  if (field == nullptr) {
    throw std::runtime_error(
        std::format("readiness missing field '{}': {}", key, line));
  }
  std::optional<std::int64_t> value = field->as_integer();
  if (!value.has_value()) {
    throw std::runtime_error(
        std::format("readiness field '{}' is not an integer: {}", key, line));
  }
  return *value;
}

std::vector<Readiness::DevAccount>
parse_dev_accounts(const json::Value &readiness, std::string_view line)
{
  const json::Value *dev_accounts = readiness.find("dev_accounts");
  if (dev_accounts == nullptr) {
    return {};
  }
  if (dev_accounts->type() != json::Value::Type::kArray) {
    throw std::runtime_error(
        std::format("readiness dev_accounts is not an array: {}", line));
  }

  std::vector<Readiness::DevAccount> out;
  for (const json::Value &account : dev_accounts->as_array()) {
    if (account.type() != json::Value::Type::kObject) {
      throw std::runtime_error(
          std::format("readiness dev_accounts entry is not an object: {}",
                      line));
    }
    const std::int64_t index = require_integer_field(account, "index", line);
    const std::int64_t initial_storage_tokens =
        require_integer_field(account, "initial_storage_tokens", line);
    if (index < 0 ||
        std::cmp_greater(index, std::numeric_limits<std::uint32_t>::max()) ||
        initial_storage_tokens < 0) {
      throw std::runtime_error(
          std::format("unexpected dev_accounts integer in readiness: {}",
                      line));
    }
    out.push_back(Readiness::DevAccount{
        .index = static_cast<std::uint32_t>(index),
        .address = std::string(require_string_field(account, "address", line)),
        .public_key =
            std::string(require_string_field(account, "public_key", line)),
        .initial_storage_tokens =
            static_cast<std::uint64_t>(initial_storage_tokens),
    });
  }
  return out;
}

Readiness
parse_readiness(std::string line)
{
  if (!line.empty() && line.back() == '\n') {
    line.pop_back();
  }

  std::expected<json::Value, json::ParseError> parsed = json::parse(line);
  if (!parsed.has_value()) {
    throw std::runtime_error(std::format("readiness JSON parse failed at {}: "
                                         "{}: {}",
                                         parsed.error().offset,
                                         parsed.error().message, line));
  }

  std::string_view status = require_string_field(*parsed, "status", line);
  if (status != "ready") {
    throw std::runtime_error(
        std::format("unexpected readiness status '{}': {}", status, line));
  }

  std::string_view network_id =
      require_string_field(*parsed, "network_id", line);
  if (network_id != "groundhog-local") {
    throw std::runtime_error(std::format("unexpected readiness network_id "
                                         "'{}': {}",
                                         network_id, line));
  }

  const json::Value *endpoints = parsed->find("endpoints");
  if (endpoints == nullptr || endpoints->type() != json::Value::Type::kObject) {
    throw std::runtime_error(
        std::format("readiness missing endpoints object: {}", line));
  }

  return {
      .scheduler =
          parse_endpoint(require_string_field(*endpoints, "scheduler", line),
                         "scheduler", line),
      .read = parse_endpoint(require_string_field(*endpoints, "read", line),
                             "read", line),
      .gateway =
          parse_endpoint(require_string_field(*endpoints, "gateway", line),
                         "gateway", line),
      .view_gateway =
          parse_endpoint(require_string_field(*endpoints, "view_gateway", line),
                         "view_gateway", line),
      .dev_accounts = parse_dev_accounts(*parsed, line),
  };
}

SHA256Hash
make_nonce(std::uint64_t nonce)
{
  SHA256Hash out{};
  std::memcpy(out.data(), &nonce, sizeof(nonce));
  return out;
}

template<std::size_t N>
std::string
hex_string(std::array<std::uint8_t, N> const &bytes)
{
  return std::format("0x{}", encode_hex(std::as_bytes(std::span(bytes))));
}

AuthClaim
source_address_auth_claim()
{
  AuthClaim claim(AuthClaimType::kSourceAddressAuthData);
  claim.source_address_claim().allowed_keys = 0x01;
  claim.source_address_claim().required_weight = 1;
  return claim;
}

SignedTransactionHashBatch
submit_and_commit(sched_coord_client_t &scheduler,
                  SignedTransactionBatch const &txs)
{
  scheduler.schedulercoord_submit_unvetted(t_to_parallel_batch(txs)())();

  SchedulerGetTipResult tip = scheduler.schedcoord_get_tip()();
  if (tip.outcome() != SchedulerGetTipOutcome::SchedulerGetTipOK) {
    ADD_FAILURE() << "schedcoord_get_tip did not return OK";
    return {};
  }
  SchedulerNominationResult nomination =
      scheduler.schedulercoord_nominate(tip.db_tip())();
  if (nomination.outcome() !=
      SchedulerNominationOutcome::SchedulerNominationOK) {
    ADD_FAILURE() << "schedulercoord_nominate did not return OK";
    return {};
  }
  SignedTransactionHashBatch tx_hashes;
  EXPECT_TRUE(parallel_batch_to_t(
      nomination.nominated_block().nomination.transactions_hash, tx_hashes)());
  if (scheduler.schedulercoord_commit_block(
          nomination.nominated_block().block_id)() !=
      SchedulerCommitOutcome::SchedulerCommitOK) {
    ADD_FAILURE() << "schedulercoord_commit_block did not return OK";
    return {};
  }
  return tx_hashes;
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
  static HogdDevProcess start(std::vector<std::string> extra_args = {})
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

      std::vector<std::string> args{HOGD_DEV_BINARY,
                                    "--listen-host",
                                    "127.0.0.1",
                                    "--scheduler-port",
                                    "0",
                                    "--readiness",
                                    "stdout"};
      args.insert(args.end(), extra_args.begin(), extra_args.end());

      std::vector<char *> argv;
      argv.reserve(args.size() + 1);
      for (std::string &arg : args) {
        argv.push_back(arg.data());
      }
      argv.push_back(nullptr);

      execv(HOGD_DEV_BINARY, argv.data());
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

  std::optional<int> wait_for_exit_for_test(std::chrono::milliseconds timeout)
  {
    return wait_for_exit(timeout);
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
  EXPECT_EQ(readiness.gateway.host, "127.0.0.1");
  EXPECT_GT(readiness.gateway.port, 0);
  EXPECT_EQ(readiness.view_gateway.host, "127.0.0.1");
  EXPECT_GT(readiness.view_gateway.port, 0);
  EXPECT_TRUE(readiness.dev_accounts.empty());

  expect_clean_exit(process);
}

TEST(HogdDev, FormatsSchedulerEndpointAsCanonicalHostPort)
{
  EXPECT_EQ(format_advertised_address("127.0.0.1", 12345), "127.0.0.1:12345");
  EXPECT_EQ(format_advertised_address("::1", 12345), "[::1]:12345");
}

TEST(HogdDev, ReadinessParserAcceptsBracketedIpv6Endpoints)
{
  Readiness readiness =
      parse_readiness(R"({"status":"ready","network_id":"groundhog-local",)"
                      R"("endpoints":{"scheduler":"[::1]:12345",)"
                      R"("read":"[::1]:23456",)"
                      R"("gateway":"[::1]:34567",)"
                      R"("view_gateway":"[::1]:45678"}})");

  EXPECT_EQ(readiness.scheduler.host, "::1");
  EXPECT_EQ(readiness.scheduler.port, 12345);
  EXPECT_EQ(readiness.read.host, "::1");
  EXPECT_EQ(readiness.read.port, 23456);
  EXPECT_EQ(readiness.gateway.host, "::1");
  EXPECT_EQ(readiness.gateway.port, 34567);
  EXPECT_EQ(readiness.view_gateway.host, "::1");
  EXPECT_EQ(readiness.view_gateway.port, 45678);
}

TEST(HogdDev, DevAccountDerivationMatchesRustGoldens)
{
  constexpr std::string_view kDefaultSeed = "groundhog-dev-accounts";
  const std::vector<std::string> expected = {
      "0x591ff2bdaaf5a0cbfd31f7b0f88c1ddee39fd9e106ba46303bde718459960633",
      "0xf5495b73debe0193757220005043944a3f03ff20cea7c4559db158eb1389e93f",
      "0xb8c09ac34f6c3ff58ddb549b19751d4e8b52f217fe6138f47322989c55a279a5",
      "0x8c8cb82ec23c847434193b00949c37d50313ef4e73e6dc4904028072a236dfbc",
      "0x45e85a804e73df05e59cdbcabf2252938b3b8b87a274ef60b2eff0b79cdd76dc",
      "0x178f7e68819b387699d2b5acfc8cda87de03ff92db1944f83a114a1946945d37",
      "0x7ea77f21cf1e56066f95d01a5bdd42d34087bd8a02261a0cbf67b2eed93393d0",
      "0xee21e4725d8b38a54e42a0eb83782cdcfcaae89afb665b53c7a941a3835bccf0",
      "0xe4788cde106e2144b3d111af4adf56170a6e25d2636616f891f304af38397174",
      "0x404b21c4c455673bb0c841a55cf5f154aa646a83e0ea9722f9a87c64f1864564",
  };

  for (std::uint32_t i = 0; i < expected.size(); ++i) {
    auto keypair = derive_dev_account_keypair(kDefaultSeed, i);
    EXPECT_EQ(hex_string(keypair.first), expected[i]);
  }
}

TEST(HogdDev, BootstrapArtifactBuildsStableDevAccounts)
{
  DevAccountBootstrapConfig config{.count = 3};

  DevAccountBootstrapArtifact first =
      build_dev_account_bootstrap_artifact(config);
  DevAccountBootstrapArtifact second =
      build_dev_account_bootstrap_artifact(config);

  ASSERT_EQ(first.accounts.size(), 3UL);
  ASSERT_EQ(first.initialization_txs.size(), 3UL);
  ASSERT_EQ(first.genesis_configuration.initial_config.size(), 3UL);
  ASSERT_EQ(second.accounts.size(), first.accounts.size());

  for (std::uint32_t i = 0; i < first.accounts.size(); ++i) {
    EXPECT_EQ(first.accounts[i].index, i);
    EXPECT_EQ(first.accounts[i].storage_tokens, config.storage_tokens);
    EXPECT_EQ(first.accounts[i].address, second.accounts[i].address);
    EXPECT_EQ(first.accounts[i].public_key, second.accounts[i].public_key);
  }
}

TEST(HogdDev, DevAccountReadinessAdvertisesPublicMetadata)
{
  HogdDevProcess process = HogdDevProcess::start({
      "--dev-accounts",
      "3",
      "--dev-account-seed",
      "groundhog-dev-accounts",
      "--dev-account-tokens",
      "1000000000",
  });
  Readiness readiness = process.wait_for_readiness();

  ASSERT_EQ(readiness.dev_accounts.size(), 3UL);
  for (std::uint32_t i = 0; i < readiness.dev_accounts.size(); ++i) {
    auto keypair = derive_dev_account_keypair("groundhog-dev-accounts", i);
    EXPECT_EQ(readiness.dev_accounts[i].index, i);
    EXPECT_EQ(readiness.dev_accounts[i].public_key, hex_string(keypair.first));
    EXPECT_TRUE(readiness.dev_accounts[i].address.starts_with("0x"));
    EXPECT_EQ(readiness.dev_accounts[i].address.size(), 66UL);
    EXPECT_GT(readiness.dev_accounts[i].initial_storage_tokens, 0UL);
    EXPECT_LE(readiness.dev_accounts[i].initial_storage_tokens,
              1'000'000'000UL);
  }

  expect_clean_exit(process);
}

TEST(HogdDev, RejectsDevAccountCountAboveLimit)
{
  HogdDevProcess process = HogdDevProcess::start({
      "--dev-accounts",
      "1001",
  });

  std::optional<int> status = process.wait_for_exit_for_test(kStartupTimeout);
  ASSERT_TRUE(status.has_value());
  const int wait_status = status.value_or(EXIT_SUCCESS);
  EXPECT_FALSE(WIFEXITED(wait_status) &&
               WEXITSTATUS(wait_status) == EXIT_SUCCESS);
  EXPECT_TRUE(process.read_remaining_stdout().empty());
  EXPECT_NE(process.read_remaining_stderr().find("Invalid dev account count"),
            std::string::npos);
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

TEST(HogdDev, GatewayEndpointAnswersIntroAndGetItems)
{
  HogdDevProcess process = HogdDevProcess::start();
  Readiness readiness = process.wait_for_readiness();

  {
    auto client = create_client<DbGateway1>(readiness.gateway.host,
                                            readiness.gateway.port,
                                            /*retry=*/true)();

    EXPECT_EQ(client.dbgateway_intro(version::kHogdProtocol())(),
              DbGatewayIntroOutcome::DbGatewayIntroSuccess);

    // Empty DB: any address returns per-item kRPCNotFound; outer batch
    // result is kRPCNone.
    AddressAndKey query;
    query.address = ContractAddress{};
    query.key.key_type(StorageKeyType::FreeNativeTokenBalance);
    AddressAndKeyBatch batch;
    batch.push_back(query);

    DbGatewayResult result = client.dbgateway_get_items(batch)();
    ASSERT_EQ(result.error(), RPCError::kRPCNone);
    ASSERT_EQ(result.payload().items.size(), 1U);
    EXPECT_EQ(result.payload().items[0].error(), RPCError::kRPCNotFound);
  }

  expect_clean_exit(process);
}

TEST(HogdDev, ViewGatewayEndpointAnswersIntro)
{
  HogdDevProcess process = HogdDevProcess::start();
  Readiness readiness = process.wait_for_readiness();

  {
    auto client = create_client<ViewGateway1>(readiness.view_gateway.host,
                                              readiness.view_gateway.port,
                                              /*retry=*/true)();

    EXPECT_EQ(client.viewgateway_intro(version::kHogdProtocol())(),
              ViewGatewayIntroOutcome::ViewGatewayIntroSuccess);
  }

  expect_clean_exit(process);
}

TEST(HogdDev, ViewGatewayViewsCommittedDevWallet)
{
  HogdDevProcess process = HogdDevProcess::start({
      "--dev-accounts",
      "1",
      "--dev-account-seed",
      "groundhog-dev-accounts",
      "--dev-account-tokens",
      "1000000000",
  });
  Readiness readiness = process.wait_for_readiness();
  ASSERT_EQ(readiness.dev_accounts.size(), 1UL);

  {
    auto client = create_client<ViewGateway1>(readiness.view_gateway.host,
                                              readiness.view_gateway.port,
                                              /*retry=*/true)();

    EXPECT_EQ(client.viewgateway_intro(version::kHogdProtocol())(),
              ViewGatewayIntroOutcome::ViewGatewayIntroSuccess);

    using wallet_meta = ContractTraitMeta<dev_wallet::DevWallet>;
    const ContractAddress wallet_address =
        build_dev_account_bootstrap_artifact({.count = 1}).accounts[0].address;
    EXPECT_EQ(hex_string(wallet_address), readiness.dev_accounts[0].address);

    TransactionViewQuery query{
        .invocation =
            Invocation{
                .invoked_address = wallet_address,
                .entrypoint = wallet_meta::entrypoint<"do_action">(),
                .gas_limit = GasAmount{10'000'000},
                .calldata = Calldata{},
            },
        .parent_address = {},
        .auth_claim = source_address_auth_claim(),
        .forwarded_tokens = {},
        .invoke_flags = std::to_underlying(InvokeFlag::ReadOnly),
    };

    ViewGatewayResult result = client.viewgateway_view(query)();
    ASSERT_EQ(result.outcome(), ViewGatewayOutcome::ViewGatewayOk);
    EXPECT_EQ(result.payload().height, 1);
    EXPECT_EQ(result.payload().output.status_code(),
              InvocationFrameStatusCode::Abort);
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

TEST(HogdDev, DevAccountCanDeployCliShapedTransaction)
{
  HogdDevProcess process = HogdDevProcess::start({
      "--dev-accounts",
      "1",
      "--dev-account-seed",
      "groundhog-dev-accounts",
      "--dev-account-tokens",
      "1000000000",
  });
  Readiness readiness = process.wait_for_readiness();
  ASSERT_EQ(readiness.dev_accounts.size(), 1UL);

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

    auto fibonacci_contract = load_contract("fibonacci.wasm");
    ASSERT_TRUE(fibonacci_contract);

    auto keypair =
        derive_dev_account_keypair("groundhog-dev-accounts", /*index=*/0);
    using wallet_meta = ContractTraitMeta<dev_wallet::DevWallet>;
    const ContractAddress wallet_address =
        build_dev_account_bootstrap_artifact({.count = 1}).accounts[0].address;
    EXPECT_EQ(hex_string(wallet_address), readiness.dev_accounts[0].address);
    const SHA256Hash deploy_nonce = make_nonce(7);
    const SHA256Hash contract_hash = sha256(*fibonacci_contract);

    Transaction tx{
        .invocation =
            Invocation{
                .invoked_address = wallet_address,
                .entrypoint = wallet_meta::entrypoint<"deploy">(),
                .gas_limit = GasAmount{10'000'000},
                .calldata = wallet_meta::make_calldata<"deploy">(deploy_nonce,
                                                                 contract_hash),
            },
        .uniquifier = Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = 7},
        .contracts_to_deploy = {*fibonacci_contract},
        .auth_claim = source_address_auth_claim(),
    };

    Signature signature(CryptographicScheme::Ed25519);
    auto signed_bytes = ed25519_sign(keypair.second, x23::serialize(tx));
    ASSERT_TRUE(signed_bytes.has_value());
    signature.ed25519() = signed_bytes.value_or(Ed25519Signature{});
    SignedTransaction signed_tx{
        .tx = std::move(tx),
        .signatures = {DecoratedSignature{
            .public_key_id = 0,
            .signature = signature,
        }},
    };

    const SignedTransactionHash tx_hash = sha256(signed_tx.tx);
    SignedTransactionHashBatch nominated_hashes =
        submit_and_commit(scheduler, SignedTransactionBatch{signed_tx});
    if (nominated_hashes.size() != 1UL) {
      int status = process.terminate();
      ADD_FAILURE() << "hogd-dev exited with status " << status << "\nstderr:\n"
                    << process.read_remaining_stderr();
      return;
    }
    ASSERT_EQ(nominated_hashes.size(), 1UL);
    ASSERT_EQ(nominated_hashes[0], tx_hash);

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
    EXPECT_EQ(result.info().events.exec_result, ExecResult::ExecResultOK);
    EXPECT_EQ(result.info().events.proposed_tx_hash, tx_hash);
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
