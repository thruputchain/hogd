#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
// NOLINTNEXTLINE(modernize-deprecated-headers)
#include <signal.h>
// NOLINTNEXTLINE(modernize-deprecated-headers)
#include <stdlib.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <gtest/gtest.h>
#include <initializer_list>
#include <ios>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "auth_claim.hh"
#include "built_contracts/contract_loader.h"
#include "built_contracts/dev_wallet.h"
#include "built_contracts/itc_erc20.h"
#include "consensus.hh"
#include "contract.hh"
#include "db_gateway.hh"
#include "event.hh"
#include "event_outbox_internal.hh"
#include "exec_result.hh"
#include "header.hh"
#include "hogd/auth_claim/signature_preimage.h"
#include "hogd/base/cleanup.h"
#include "hogd/base/json.h"
#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/base/network_hash.h"
#include "hogd/base/string.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/devnode/bootstrap_accounts.h"
#include "hogd/devnode/dev_wallet_profile.h"
#include "hogd/devnode/manifest_hex.h"
#include "hogd/devnode/runtime_manifest.h"
#include "hogd/genesis/genesis_constants.h"
#include "hogd/hog_db/hog_db_dao.h"
#include "hogd/hog_db/status.h"
#include "hogd/hog_event/event_block_context.h"
#include "hogd/hog_event/indexer_outbox.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_mempool/shared.h"
#include "hogd/init.h"
#include "hogd/ramdb/db.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/version.h"
#include "indexer.hh"
#include "indexer_payload_validation.h"
#include "invoke_flag.hh"
#include "pagination.hh"
#include "proposal.hh"
#include "proposal_hash.h"
#include "rpc_types.hh"
#include "sched_coord.hh"
#include "transaction.hh"
#include "types.hh"
#include "view.hh"
#include "view_gateway.hh"
#include "x23/serialize.h"

#ifndef HOG_DEVNODE_BINARY
#error "HOG_DEVNODE_BINARY must be defined by CMake"
#endif

namespace hogd {

namespace {

constexpr auto kDefaultStartupTimeout = std::chrono::seconds(30);
constexpr auto kDefaultExitTimeout = std::chrono::seconds(10);
constexpr auto kShutdownTimeout = std::chrono::seconds(5);
constexpr auto kIndexPollTimeout = std::chrono::seconds(10);
constexpr std::size_t kMaxReadinessBytes = 16 * 1024;
constexpr std::size_t kTestRamDbBytes = 15UL * 1024 * 1024 * 1024;

std::tuple<uint64_t, uint64_t>
make_erc20_amount(uint128_t amount)
{
  return std::make_tuple(amount >> 64, amount & 0xffffffffffffffffULL);
}

std::shared_ptr<Contract>
load_default_dev_wallet_contract()
{
  return load_dev_wallet_contract(
      resolve_dev_wallet_profile(kDefaultDevWalletProfile));
}

std::chrono::seconds
startup_timeout()
{
  const char *value = std::getenv("HOG_DEVNODE_STARTUP_TIMEOUT_SECS");
  if (value == nullptr || *value == '\0') {
    return kDefaultStartupTimeout;
  }

  errno = 0;
  char *end = nullptr;
  std::uint64_t seconds = std::strtoull(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || seconds == 0 ||
      seconds > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
    throw std::runtime_error("invalid HOG_DEVNODE_STARTUP_TIMEOUT_SECS value");
  }
  return std::chrono::seconds(static_cast<int>(seconds));
}

std::chrono::seconds
exit_timeout()
{
  const char *value = std::getenv("HOG_DEVNODE_EXIT_TIMEOUT_SECS");
  if (value == nullptr || *value == '\0') {
    return kDefaultExitTimeout;
  }

  errno = 0;
  char *end = nullptr;
  std::uint64_t seconds = std::strtoull(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || seconds == 0 ||
      seconds > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
    throw std::runtime_error("invalid HOG_DEVNODE_EXIT_TIMEOUT_SECS value");
  }
  return std::chrono::seconds(static_cast<int>(seconds));
}

struct Readiness {
  struct Endpoint {
    std::string host;
    std::uint16_t port;
  };

  Endpoint scheduler;
  Endpoint indexer;
  Endpoint db_gateway;
  Endpoint view_gateway;
  std::string chain_state;
  std::string network_hash;
  std::filesystem::path manifest_path;
  std::string manifest_hash;
  json::Value manifest;

  struct BootstrapAccount {
    std::uint32_t index;
    std::string role;
    std::string address;
    std::string public_key;
    std::string initial_storage_tokens;
  };
  std::vector<BootstrapAccount> bootstrap_accounts;
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

std::string
read_file(std::filesystem::path const &path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error(std::format("failed to open {}", path.string()));
  }
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

SHA256Hash
hash_string(std::string_view bytes)
{
  SHA256Hasher hasher;
  hasher.update(std::as_bytes(std::span(bytes.data(), bytes.size())));
  return hasher.finalize();
}

int
path_mode(std::filesystem::path const &path)
{
  struct stat stat_buffer{};
  if (::stat(path.c_str(), &stat_buffer) != 0) {
    throw std::runtime_error(errno_message("stat", errno));
  }
  return static_cast<int>(stat_buffer.st_mode & 0777);
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

void
require_exact_object_keys(const json::Value &parent,
                          std::initializer_list<std::string_view> keys,
                          std::string_view name, std::string_view line)
{
  if (parent.type() != json::Value::Type::kObject) {
    throw std::runtime_error(
        std::format("{} is not an object: {}", name, line));
  }
  const auto fields = parent.as_object();
  if (fields.size() != keys.size()) {
    throw std::runtime_error(std::format("{} has {} fields; expected {}: {}",
                                         name, fields.size(), keys.size(),
                                         line));
  }
  for (const auto &[field, _] : fields) {
    bool expected = false;
    for (std::string_view key : keys) {
      if (field == key) {
        expected = true;
        break;
      }
    }
    if (!expected) {
      throw std::runtime_error(
          std::format("{} has unexpected field '{}': {}", name, field, line));
    }
  }
}

std::vector<Readiness::BootstrapAccount>
parse_bootstrap_accounts(const json::Value &manifest, std::string_view line)
{
  const json::Value *bootstrap_accounts = manifest.find("bootstrapAccounts");
  if (bootstrap_accounts == nullptr) {
    return {};
  }
  if (bootstrap_accounts->type() != json::Value::Type::kArray) {
    throw std::runtime_error(
        std::format("manifest bootstrapAccounts is not an array: {}", line));
  }

  std::vector<Readiness::BootstrapAccount> out;
  for (const json::Value &account : bootstrap_accounts->as_array()) {
    if (account.type() != json::Value::Type::kObject) {
      throw std::runtime_error(
          std::format("manifest bootstrapAccounts entry is not an object: {}",
                      line));
    }
    const std::int64_t index = require_integer_field(account, "index", line);
    if (index < 0 ||
        std::cmp_greater(index, std::numeric_limits<std::uint32_t>::max())) {
      throw std::runtime_error(
          std::format("unexpected bootstrapAccounts index in manifest: {}",
                      line));
    }
    out.push_back(Readiness::BootstrapAccount{
        .index = static_cast<std::uint32_t>(index),
        .role = std::string(require_string_field(account, "role", line)),
        .address =
            std::string(require_string_field(account, "walletAddress", line)),
        .public_key =
            std::string(require_string_field(account, "publicKey", line)),
        .initial_storage_tokens = std::string(
            require_string_field(account, "initialStorageTokens", line)),
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
  require_exact_object_keys(*parsed,
                            {"schemaVersion", "status", "manifestPath",
                             "manifestHash"},
                            "readiness pointer", line);

  const std::int64_t schema_version =
      require_integer_field(*parsed, "schemaVersion", line);
  if (schema_version != 2) {
    throw std::runtime_error(
        std::format("unexpected readiness schemaVersion {}: {}", schema_version,
                    line));
  }

  std::string_view status = require_string_field(*parsed, "status", line);
  if (status != "ready") {
    throw std::runtime_error(
        std::format("unexpected readiness status '{}': {}", status, line));
  }

  std::string manifest_hash =
      std::string(require_string_field(*parsed, "manifestHash", line));
  std::filesystem::path manifest_path(
      std::string(require_string_field(*parsed, "manifestPath", line)));
  std::string manifest_bytes = read_file(manifest_path);
  std::string actual_hash = format_hex_hash(hash_string(manifest_bytes));
  if (actual_hash != manifest_hash) {
    throw std::runtime_error(
        std::format("manifest hash mismatch: expected {}, got {}",
                    manifest_hash, actual_hash));
  }

  std::expected<json::Value, json::ParseError> manifest =
      json::parse(manifest_bytes);
  if (!manifest.has_value()) {
    throw std::runtime_error(std::format("manifest JSON parse failed at {}: "
                                         "{}: {}",
                                         manifest.error().offset,
                                         manifest.error().message,
                                         manifest_bytes));
  }

  const std::int64_t manifest_schema_version =
      require_integer_field(*manifest, "schemaVersion", manifest_bytes);
  if (manifest_schema_version != 2) {
    throw std::runtime_error(std::format("unexpected manifest schemaVersion {}",
                                         manifest_schema_version));
  }
  std::string_view kind =
      require_string_field(*manifest, "kind", manifest_bytes);
  if (kind != "hogd-dev-runtime") {
    throw std::runtime_error(
        std::format("unexpected manifest kind '{}'", kind));
  }
  std::string chain_state = std::string(
      require_string_field(*manifest, "chainState", manifest_bytes));
  if (chain_state != "fresh" && chain_state != "recovered") {
    throw std::runtime_error(
        std::format("unexpected manifest chainState '{}'", chain_state));
  }
  std::string_view network_hash =
      require_string_field(*manifest, "networkHash", manifest_bytes);
  if (network_hash.empty()) {
    throw std::runtime_error("manifest networkHash is empty");
  }

  const json::Value *endpoints = manifest->find("endpoints");
  if (endpoints == nullptr || endpoints->type() != json::Value::Type::kObject) {
    throw std::runtime_error(
        std::format("manifest missing endpoints object: {}", manifest_bytes));
  }
  std::vector<Readiness::BootstrapAccount> bootstrap_accounts =
      parse_bootstrap_accounts(*manifest, manifest_bytes);

  return {
      .scheduler = parse_endpoint(require_string_field(*endpoints, "scheduler",
                                                       manifest_bytes),
                                  "scheduler", manifest_bytes),
      .indexer = parse_endpoint(require_string_field(*endpoints, "indexer",
                                                     manifest_bytes),
                                "indexer", manifest_bytes),
      .db_gateway = parse_endpoint(require_string_field(*endpoints, "dbGateway",
                                                        manifest_bytes),
                                   "dbGateway", manifest_bytes),
      .view_gateway =
          parse_endpoint(require_string_field(*endpoints, "viewGateway",
                                              manifest_bytes),
                         "viewGateway", manifest_bytes),
      .chain_state = std::move(chain_state),
      .network_hash = std::string(network_hash),
      .manifest_path = std::move(manifest_path),
      .manifest_hash = std::move(manifest_hash),
      .manifest = std::move(*manifest),
      .bootstrap_accounts = std::move(bootstrap_accounts),
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

// The network a node is serving, as clients learn it: from the networkHash
// field of the runtime manifest the node emits when it becomes ready.
NetworkHash
readiness_network_hash(Readiness const &readiness)
{
  std::optional<NetworkHash> hash = parse_network_hash(readiness.network_hash);
  if (!hash.has_value()) {
    throw std::runtime_error(
        std::format("readiness reported an unparseable network hash '{}'",
                    readiness.network_hash));
  }
  return *hash;
}

SignedTransaction
sign_transaction(Transaction tx, Ed25519SecretKey const &secret_key,
                 NetworkHash const &network_hash)
{
  Signature signature(CryptographicScheme::Ed25519);
  auto signed_bytes =
      ed25519_sign(secret_key,
                   transaction_signature_preimage(network_hash, tx));
  if (!signed_bytes.has_value()) {
    throw std::runtime_error("failed to sign test transaction");
  }
  signature.ed25519() = std::move(*signed_bytes);
  return SignedTransaction{
      .tx = std::move(tx),
      .signatures = {DecoratedSignature{
          .public_key_id = 0,
          .signature = signature,
      }},
  };
}

TransactionHashBatch
propose_and_commit(sched_coord_client_t &scheduler)
{
  SchedulerGetTipResult tip = scheduler.schedcoord_get_tip()();
  if (tip.outcome() != SchedulerGetTipOutcome::SchedulerGetTipOK) {
    ADD_FAILURE() << "schedcoord_get_tip did not return OK";
    return {};
  }
  SchedulerProposalResult proposal =
      scheduler.schedulercoord_propose_block(tip.db_tip().id,
                                             ProposalContext{})();
  if (proposal.outcome() != SchedulerProposalOutcome::SchedulerProposalOK) {
    ADD_FAILURE() << "schedulercoord_propose_block did not return OK";
    return {};
  }
  const SchedulerProposedBlock &proposed = proposal.proposed_block();
  SignedTransactionBatch proposed_txs;
  EXPECT_TRUE(parallel_batch_to_t(proposed.txs, proposed_txs)());
  TransactionHashBatch tx_hashes;
  tx_hashes.reserve(proposed_txs.size());
  for (const SignedTransaction &tx : proposed_txs) {
    tx_hashes.push_back(sha256(tx.tx));
  }
  ConsensusValue value{};
  value.prev_hash = sha256(tip.db_tip().value);
  value.proposal_header = proposed.proposal.header;
  value.proposal_txs_hash =
      proposal_txs_compatibility_hash(proposed.proposal.transactions);
  if (scheduler.schedulercoord_commit_block(proposed.block_id, value)() !=
      SchedulerCommitOutcome::SchedulerCommitOK) {
    ADD_FAILURE() << "schedulercoord_commit_block did not return OK";
    return {};
  }
  return tx_hashes;
}

TransactionHashBatch
submit_and_commit(sched_coord_client_t &scheduler,
                  SignedTransactionBatch const &txs)
{
  scheduler.schedulercoord_submit_unvetted(t_to_parallel_batch(txs)())();
  return propose_and_commit(scheduler);
}

class HogDevnodeProcess {
public:
  static HogDevnodeProcess
  start(std::vector<std::string> extra_args = {},
        const std::vector<std::pair<std::string, std::string>> &extra_env = {},
        bool include_default_args = true)
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

      std::vector<std::string> args{HOG_DEVNODE_BINARY};
      if (include_default_args) {
        args.insert(args.end(),
                    {"--listen-host", "127.0.0.1", "--scheduler-port", "0",
                     "--readiness", "stdout"});
      }
      args.insert(args.end(), extra_args.begin(), extra_args.end());

      for (auto const &[key, value] : extra_env) {
        // NOLINTNEXTLINE(misc-include-cleaner)
        if (setenv(key.c_str(), value.c_str(), /*overwrite=*/1) != 0) {
          _exit(127);
        }
      }

      std::vector<char *> argv;
      argv.reserve(args.size() + 1);
      for (std::string &arg : args) {
        argv.push_back(arg.data());
      }
      argv.push_back(nullptr);

      execv(HOG_DEVNODE_BINARY, argv.data());
      _exit(127);
    }

    close(stdout_pipe[1]);
    close(stderr_pipe[1]);
    return HogDevnodeProcess(pid, stdout_pipe[0], stderr_pipe[0]);
  }

  HogDevnodeProcess(HogDevnodeProcess const &) = delete;
  HogDevnodeProcess &operator=(HogDevnodeProcess const &) = delete;

  HogDevnodeProcess(HogDevnodeProcess &&other) noexcept
    : pid_(other.pid_), stdout_fd_(other.stdout_fd_),
      stderr_fd_(other.stderr_fd_),
      stdout_buffer_(std::move(other.stdout_buffer_))
  {
    other.pid_ = -1;
    other.stdout_fd_ = -1;
    other.stderr_fd_ = -1;
  }

  HogDevnodeProcess &operator=(HogDevnodeProcess &&) = delete;

  ~HogDevnodeProcess()
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
    return parse_readiness(read_stdout_line(startup_timeout()));
  }

  std::optional<int> wait_for_exit_for_test(std::chrono::milliseconds timeout)
  {
    return wait_for_exit(timeout);
  }

  int terminate()
  {
    if (pid_ <= 0) {
      throw std::runtime_error("hog-devnode process already reaped");
    }
    if (kill(pid_, SIGTERM) != 0 && errno != ESRCH) {
      throw std::runtime_error(errno_message("kill SIGTERM", errno));
    }

    std::optional<int> status = wait_for_exit(kShutdownTimeout);
    if (!status.has_value()) {
      kill(pid_, SIGKILL);
      status = wait_for_exit(kShutdownTimeout);
      if (!status.has_value()) {
        throw std::runtime_error("hog-devnode did not exit after SIGKILL");
      }
    }
    return *status;
  }

  // Models a crash: the node gets no shutdown path at all, so whatever it
  // already made durable is exactly what the next start has to recover from.
  // Unlike [terminate()] there is no SIGTERM grace period to fall back on --
  // SIGKILL cannot be caught, so a process that fails to die is a test bug.
  int kill_ungracefully()
  {
    if (pid_ <= 0) {
      throw std::runtime_error("hog-devnode process already reaped");
    }
    // NOLINTNEXTLINE(misc-include-cleaner)
    if (kill(pid_, SIGKILL) != 0 && errno != ESRCH) {
      throw std::runtime_error(errno_message("kill SIGKILL", errno));
    }
    std::optional<int> status = wait_for_exit(kShutdownTimeout);
    if (!status.has_value()) {
      throw std::runtime_error("hog-devnode did not exit after SIGKILL");
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
  HogDevnodeProcess(int pid, int stdout_fd, int stderr_fd)
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
        throw std::runtime_error("hog-devnode readiness line exceeded limit");
      }

      auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        throw std::runtime_error("timed out waiting for hog-devnode readiness");
      }
      auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);

      // NOLINTNEXTLINE(misc-include-cleaner)
      pollfd poll_fd{.fd = stdout_fd_, .events = POLLIN, .revents = 0};
      // NOLINTNEXTLINE(misc-include-cleaner)
      int ready = poll(&poll_fd, 1, static_cast<int>(remaining.count()));
      if (ready < 0) {
        if (errno == EINTR) {
          continue;
        }
        throw std::runtime_error(errno_message("poll stdout", errno));
      }
      if (ready == 0) {
        throw std::runtime_error("timed out waiting for hog-devnode readiness");
      }

      char buffer[1024];
      auto count = read(stdout_fd_, buffer, sizeof(buffer));
      if (count > 0) {
        stdout_buffer_.append(buffer, static_cast<std::size_t>(count));
        continue;
      }
      if (count == 0) {
        throw std::runtime_error(
            std::format("hog-devnode stdout closed before readiness: {}",
                        read_remaining_stderr()));
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
expect_clean_exit(HogDevnodeProcess &process)
{
  int status = process.terminate();
  std::string stderr_output = process.read_remaining_stderr();
  ASSERT_TRUE(WIFEXITED(status)) << stderr_output;
  EXPECT_EQ(WEXITSTATUS(status), EXIT_SUCCESS) << stderr_output;
  EXPECT_TRUE(process.read_remaining_stdout().empty());
}

void
expect_killed_exit(HogDevnodeProcess &process)
{
  int status = process.kill_ungracefully();
  std::string stderr_output = process.read_remaining_stderr();
  ASSERT_TRUE(WIFSIGNALED(status)) << stderr_output;
  // NOLINTNEXTLINE(misc-include-cleaner)
  EXPECT_EQ(WTERMSIG(status), SIGKILL) << stderr_output;
}

std::string
bootstrap_manifest_json(std::uint32_t count, std::string_view seed,
                        std::string_view initial_storage_tokens,
                        std::string_view content_hash,
                        bool include_protocol = false)
{
  using wallet_meta = ContractTraitMeta<dev_wallet::DevWallet>;

  std::string buffer;
  json::Writer(buffer).object([&](json::Writer &w) {
    w.field("schemaVersion", std::int64_t{2});
    w.field("kind", "groundhog-bootstrap");
    w.field("networkHash",
            "00000000000000000000000000000000000000000000000000000000000000aa");
    w.field("profile", "local-dev");
    if (include_protocol) {
      w.field("protocol", version::kHogdProtocol());
    }
    w.object_field("devWallet", [&](json::Writer &w) {
      w.field("profile", "hog-devnode-default");
      w.field("artifactHint", "./dev-wallet.rv32a");
      w.field("contentHash", content_hash);
      w.field("deployEntrypoint",
              entrypoint_hex(wallet_meta::entrypoint<u8"deploy">()));
      w.field("doActionEntrypoint",
              entrypoint_hex(wallet_meta::entrypoint<u8"do_action">()));
    });
    w.object_field("bootstrapFaucet", [&](json::Writer &w) {
      w.field("index", std::int64_t{0});
      w.field("initialStorageTokens",
              std::to_string(kLocalFaucetStorageTokens));
    });
    w.object_field("bootstrapAccounts", [&](json::Writer &w) {
      w.field("count", static_cast<std::int64_t>(count));
      w.field("firstIndex", std::int64_t{1});
      w.field("seed", seed);
      w.field("initialStorageTokens", initial_storage_tokens);
    });
    w.object_field("storageCostModel", [&](json::Writer &w) {
      w.field("profile", "hogd-default");
    });
  });
  return buffer;
}

std::filesystem::path
write_test_bootstrap_manifest(std::string_view contents)
{
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      std::format("hog-devnode-bootstrap-manifest-{}-{}.json", getpid(),
                  std::chrono::steady_clock::now().time_since_epoch().count());
  std::ofstream output(path, std::ios::binary);
  if (!output) {
    throw std::runtime_error(std::format("failed to write {}", path.string()));
  }
  output << contents;
  return path;
}

std::filesystem::path
missing_test_bootstrap_manifest_path()
{
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      std::format("hog-devnode-bootstrap-manifest-missing-{}-{}.json", getpid(),
                  std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::remove(path, ec);
  return path;
}

std::filesystem::path
make_test_data_dir()
{
  return std::filesystem::temp_directory_path() /
         std::format(
             "hog-devnode-data-{}-{}", getpid(),
             std::chrono::steady_clock::now().time_since_epoch().count());
}

HogDbDAO
open_persistent_hog_db(std::filesystem::path const &data_dir)
{
  const std::string db_path = (data_dir / "ramdb").string();
  DBStatus<std::uint64_t> lsn = Database::check(db_path, kTestRamDbBytes);
  if (!lsn.has_value()) {
    lsn.error().raise("failed to check test hog_db");
  }
  DBStatus<std::unique_ptr<Database>> db =
      Database::open_lsn(db_path, *lsn, kTestRamDbBytes);
  if (!db.has_value()) {
    db.error().raise("failed to open test hog_db");
  }
  return HogDbDAO(RAMDB(std::move(*db)));
}

void
initialize_persistent_data_dir(std::filesystem::path const &data_dir,
                               uint32_t dev_accounts = 1)
{
  HogDevnodeProcess process = HogDevnodeProcess::start({
      "--data-dir",
      data_dir.string(),
      "--dev-accounts",
      std::to_string(dev_accounts),
      "--dev-account-seed",
      "groundhog-dev-accounts",
      "--dev-account-tokens",
      "1000000000",
  });
  Readiness readiness = process.wait_for_readiness();
  ASSERT_EQ(readiness.bootstrap_accounts.size(), dev_accounts + 1UL);
  expect_clean_exit(process);
}

TEST(HogDevnode, FreshDataDirOutboxOriginLinksCommittedGenesis)
{
  std::filesystem::path data_dir = make_test_data_dir();
  Defer cleanup_data_dir([&] { std::filesystem::remove_all(data_dir); });
  initialize_persistent_data_dir(data_dir, 0);

  HogDbDAO db = open_persistent_hog_db(data_dir);
  HogDbStatus<DbTip> tip = db.read_dbtip();
  ASSERT_TRUE(tip.has_value()) << tip.error();
  ASSERT_EQ(tip->value.proposal_header.block_height, 1);
  auto store = IndexerOutboxStore::open(
      (data_dir / "events" / "indexer-outbox").string());
  ASSERT_TRUE(store.has_value()) << store.error();
  auto committed = store->load_committed(1)();
  ASSERT_TRUE(committed.has_value()) << committed.error();
  EXPECT_EQ(committed->consensus_value, tip->value);
  auto origin = store->load_current_epoch_origin();
  ASSERT_TRUE(origin.has_value()) << origin.error();
  ASSERT_TRUE(origin->has_value());
  const auto &origin_value = origin.value();
  const auto *current_origin =
      origin_value.has_value() ? &origin_value.value() : nullptr;
  ASSERT_NE(current_origin, nullptr);
  EXPECT_EQ(current_origin->origin.first_height, 1);
  EXPECT_EQ(current_origin->origin.first_prev_hash,
            committed->consensus_value.prev_hash);
}

IndexerOutboxCommittedPayload
make_empty_outbox_append(BlockHeight height, ConsensusValueHash prev_hash)
{
  ProposalTxs proposal_txs;
  TxEventLogBatch event_logs;
  ConsensusValue value{};
  value.prev_hash = std::move(prev_hash);
  value.proposal_header.block_height = height;
  value.proposal_header.events_hash =
      compute_indexer_outbox_event_root(event_logs, {});
  auto txs_root = compute_indexer_proposal_txs_root(proposal_txs)();
  CHECK(txs_root.has_value());
  value.proposal_header.txs_hash = *txs_root;
  value.proposal_txs_hash = proposal_txs_compatibility_hash(proposal_txs);
  return {.format_version = kIndexerOutboxFormatVersion,
          .consensus_value = std::move(value),
          .event_logs = std::move(event_logs),
          .tx_bodies = {}};
}

IndexerOutboxCommittedPayload
append_next_test_outbox_record(std::filesystem::path const &data_dir)
{
  auto store = IndexerOutboxStore::open(
      (data_dir / "events" / "indexer-outbox").string());
  CHECK(store.has_value());
  auto latest = store->latest_committed_record_raw();
  CHECK(latest.has_value());
  CHECK(latest->has_value());
  const auto &latest_value = latest.value();
  const auto *latest_record =
      latest_value.has_value() ? &latest_value.value() : nullptr;
  CHECK(latest_record != nullptr);
  auto previous = store->load_committed(latest_record->block_height)();
  CHECK(previous.has_value());
  auto next = make_empty_outbox_append(latest_record->block_height + 1,
                                       sha256(previous->consensus_value));
  CHECK(store->publish_committed(next)().has_value());
  return next;
}

void
advance_persistent_data_dir(std::filesystem::path const &data_dir)
{
  HogDevnodeProcess process = HogDevnodeProcess::start({
      "--data-dir",
      data_dir.string(),
      "--dev-accounts",
      "1",
      "--dev-account-seed",
      "groundhog-dev-accounts",
      "--dev-account-tokens",
      "1000000000",
  });
  Readiness readiness = process.wait_for_readiness();
  ASSERT_EQ(readiness.chain_state, "recovered");
  ASSERT_EQ(readiness.bootstrap_accounts.size(), 2UL);
  EXPECT_EQ(readiness.bootstrap_accounts[0].role, "faucet");
  EXPECT_EQ(readiness.bootstrap_accounts[1].role, "local_user");

  sched_coord_client_t scheduler =
      create_client<SchedulerCoord1>(readiness.scheduler.host,
                                     readiness.scheduler.port,
                                     /*retry=*/true)()
          .value();
  EXPECT_TRUE(submit_and_commit(scheduler, {}).empty());
  SchedulerGetTipResult tip = scheduler.schedcoord_get_tip()();
  ASSERT_EQ(tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  ASSERT_EQ(tip.db_tip().value.proposal_header.block_height, 2);

  auto reader = create_client<IndexerBlockExplorer1>(readiness.indexer.host,
                                                     readiness.indexer.port,
                                                     /*retry=*/true)()
                    .value();
  AddressTransactionQuery query{};
  query.order = IterationOrder::IterationOrderAscending;
  query.anchor.kind(BlockAnchorKind::AnchorAtBeginning);
  const auto deadline = std::chrono::steady_clock::now() + kIndexPollTimeout;
  TransactionRefPage page;
  while (std::chrono::steady_clock::now() < deadline) {
    page = reader.get_transactions_by_address(query)();
    if (page.latest_block == 2) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_EQ(page.latest_block, 2);
  expect_clean_exit(process);
}

void
write_test_data_dir_metadata(
    std::filesystem::path const &data_dir, LocalBootstrapConfig const &config,
    std::int64_t schema_version = 2,
    std::uint64_t faucet_tokens = kLocalFaucetStorageTokens)
{
  std::string buffer;
  json::Writer(buffer).object([&](json::Writer &w) {
    w.field("schemaVersion", schema_version);
    w.field("kind", "hog-devnode-data");
    w.object_field("bootstrapConfig", [&](json::Writer &w) {
      w.field("devWalletProfile", config.dev_wallet_profile);
      w.field("faucetTokens", std::to_string(faucet_tokens));
      w.field("devAccounts",
              static_cast<std::int64_t>(config.user_account_count));
      w.field("devAccountSeed", config.seed);
      w.field("devAccountTokens", std::to_string(config.user_storage_tokens));
    });
  });

  std::filesystem::create_directories(data_dir);
  std::ofstream metadata(data_dir / "hog-devnode-data.json",
                         std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(metadata);
  metadata << buffer;
}

void
expect_startup_failure(std::vector<std::string> args,
                       std::string_view expected_stderr)
{
  HogDevnodeProcess process = HogDevnodeProcess::start(std::move(args));
  std::optional<int> status = process.wait_for_exit_for_test(startup_timeout());
  if (!status.has_value()) {
    ADD_FAILURE() << "hog-devnode did not exit";
    return;
  }
  const int exit_status = *status;
  std::string stderr_output = process.read_remaining_stderr();
  ASSERT_TRUE(WIFEXITED(exit_status)) << stderr_output;
  EXPECT_EQ(WEXITSTATUS(exit_status), EXIT_FAILURE);
  EXPECT_TRUE(process.read_remaining_stdout().empty());
  EXPECT_NE(stderr_output.find(expected_stderr), std::string::npos)
      << stderr_output;
}

TEST(HogDevnode, EmitsOneReadinessLineAndStopsOnSigterm)
{
  HogDevnodeProcess process = HogDevnodeProcess::start();
  Readiness readiness = process.wait_for_readiness();

  EXPECT_EQ(readiness.network_hash,
            "0000000000000000000000000000000000000000000000000000000000000000");
  EXPECT_EQ(readiness.scheduler.host, "127.0.0.1");
  EXPECT_GT(readiness.scheduler.port, 0);
  EXPECT_EQ(readiness.indexer.host, "127.0.0.1");
  EXPECT_GT(readiness.indexer.port, 0);
  EXPECT_EQ(readiness.db_gateway.host, "127.0.0.1");
  EXPECT_GT(readiness.db_gateway.port, 0);
  EXPECT_EQ(readiness.view_gateway.host, "127.0.0.1");
  EXPECT_GT(readiness.view_gateway.port, 0);
  ASSERT_EQ(readiness.bootstrap_accounts.size(), 1UL);
  EXPECT_EQ(readiness.bootstrap_accounts[0].index, 0U);
  EXPECT_EQ(readiness.bootstrap_accounts[0].role, "faucet");
  EXPECT_EQ(readiness.bootstrap_accounts[0].initial_storage_tokens,
            "10000000000000");
  EXPECT_TRUE(readiness.manifest_path.is_absolute());
  EXPECT_TRUE(std::filesystem::exists(readiness.manifest_path));
  EXPECT_EQ(path_mode(readiness.manifest_path), 0600);
  EXPECT_EQ(path_mode(readiness.manifest_path.parent_path()), 0700);
  EXPECT_FALSE(std::filesystem::exists(
      readiness.manifest_path.parent_path() /
      std::format("{}{}", readiness.manifest_path.filename().string(),
                  ".tmp")));
  EXPECT_EQ(readiness.manifest_hash,
            hex_string(hash_string(read_file(readiness.manifest_path))));
  EXPECT_EQ(require_string_field(readiness.manifest, "protocol", ""),
            version::kHogdProtocol());
  const json::Value *producer = readiness.manifest.find("producer");
  ASSERT_NE(producer, nullptr);
  EXPECT_EQ(require_string_field(*producer, "binary", ""), "hog-devnode");
  const std::string_view producer_version =
      require_string_field(*producer, "version", "");
  EXPECT_EQ(producer_version, version::kHogdVersion);
  EXPECT_NE(producer_version, "0.0.1");
  std::string_view git_hash = version::kHogdGitHash;
  if (!git_hash.empty() && git_hash != "unknown") {
    EXPECT_EQ(require_string_field(*producer, "commitHash", ""), git_hash);
  }

  const json::Value *bootstrap = readiness.manifest.find("bootstrap");
  ASSERT_NE(bootstrap, nullptr);
  const json::Value *dev_wallet = bootstrap->find("devWallet");
  ASSERT_NE(dev_wallet, nullptr);
  using wallet_meta = ContractTraitMeta<dev_wallet::DevWallet>;
  auto wallet_contract = load_default_dev_wallet_contract();
  std::string wallet_hash = hex_string(sha256(*wallet_contract));
  EXPECT_EQ(require_string_field(*dev_wallet, "contentHash", ""), wallet_hash);
  EXPECT_EQ(require_string_field(*dev_wallet, "deployedCodeHash", ""),
            wallet_hash);
  EXPECT_EQ(require_string_field(*dev_wallet, "artifactHint", ""),
            "contracts/dev_wallet.rv32a");
  EXPECT_EQ(require_string_field(*dev_wallet, "deployEntrypoint", ""),
            entrypoint_hex(wallet_meta::entrypoint<u8"deploy">()));
  EXPECT_EQ(require_string_field(*dev_wallet, "doActionEntrypoint", ""),
            entrypoint_hex(wallet_meta::entrypoint<u8"do_action">()));
  EXPECT_EQ(require_string_field(*dev_wallet, "createWalletEntrypoint", ""),
            entrypoint_hex(wallet_meta::entrypoint<u8"create_wallet">()));

  const json::Value *cost_model = bootstrap->find("storageCostModel");
  ASSERT_NE(cost_model, nullptr);
  EXPECT_EQ(require_string_field(*cost_model, "baseDataItemCost", ""), "12000");
  EXPECT_EQ(require_string_field(*cost_model, "bytestringCostPerByte", ""),
            "10");
  EXPECT_EQ(require_string_field(*cost_model, "baseContractDeploymentCost", ""),
            "12000");
  EXPECT_EQ(require_string_field(*cost_model, "contractDeploymentCostPerByte",
                                 ""),
            "10");
  EXPECT_EQ(require_string_field(*cost_model, "authPerKeyCost", ""), "1111");
  EXPECT_EQ(require_string_field(*cost_model, "rcqCostPerEntry", ""), "8");
  EXPECT_EQ(require_string_field(*cost_model, "replayCacheCostPerEntry", ""),
            "7");
  EXPECT_EQ(require_string_field(*cost_model, "balanceCost", ""), "1234");
  EXPECT_TRUE(require_string_field(*cost_model, "hash", "").starts_with("0x"));

  expect_clean_exit(process);
}

TEST(HogDevnode, HelpPrintsBeforeInitialization)
{
  HogDevnodeProcess process = HogDevnodeProcess::start({"--help"});

  std::optional<int> status = process.wait_for_exit_for_test(exit_timeout());
  if (!status.has_value()) {
    FAIL() << "hog-devnode did not exit";
    return;
  }
  const int exit_status = status.value();
  ASSERT_TRUE(WIFEXITED(exit_status));
  EXPECT_EQ(WEXITSTATUS(exit_status), EXIT_SUCCESS);

  std::string stdout_output = process.read_remaining_stdout();
  std::string stderr_output = process.read_remaining_stderr();
  EXPECT_TRUE(stdout_output.contains("Usage:"));
  EXPECT_TRUE(stdout_output.contains("--listen-host <ip>"));
  EXPECT_TRUE(stdout_output.contains("--data-dir <dir>"));
  EXPECT_TRUE(stdout_output.contains("--contract-artifacts-dir <dir>"));
  EXPECT_TRUE(stdout_output.contains("--dev-wallet-profile <name>"));
  EXPECT_TRUE(stdout_output.contains("--version"));
  EXPECT_TRUE(stdout_output.contains("HOGD_LOG_FORMAT=json"));
  EXPECT_TRUE(stdout_output.contains("readiness line"));
  EXPECT_TRUE(stdout_output.contains("--ramdb-bytes <bytes>"));
  EXPECT_TRUE(stderr_output.empty()) << stderr_output;
}

TEST(HogDevnode, VersionPrintsBeforeInitialization)
{
  HogDevnodeProcess process =
      HogDevnodeProcess::start({"--version"}, {},
                               /*include_default_args=*/false);

  std::optional<int> status = process.wait_for_exit_for_test(exit_timeout());
  if (!status.has_value()) {
    FAIL() << "hog-devnode did not exit";
    return;
  }
  const int exit_status = status.value();
  ASSERT_TRUE(WIFEXITED(exit_status));
  EXPECT_EQ(WEXITSTATUS(exit_status), EXIT_SUCCESS);

  std::string stdout_output = process.read_remaining_stdout();
  std::string stderr_output = process.read_remaining_stderr();
  EXPECT_EQ(stdout_output,
            std::format("hog-devnode {}\n", version::kHogdVersion));
  EXPECT_TRUE(stderr_output.empty()) << stderr_output;
}

TEST(HogDevnode, InvalidNumericArgumentPrintsFriendlyUsageBeforeInitialization)
{
  HogDevnodeProcess process =
      HogDevnodeProcess::start({"--scheduler-port", "not-a-port"});

  std::optional<int> status = process.wait_for_exit_for_test(exit_timeout());
  if (!status.has_value()) {
    FAIL() << "hog-devnode did not exit";
    return;
  }
  const int exit_status = status.value();
  ASSERT_TRUE(WIFEXITED(exit_status));
  EXPECT_EQ(WEXITSTATUS(exit_status), EXIT_FAILURE);

  std::string stdout_output = process.read_remaining_stdout();
  std::string stderr_output = process.read_remaining_stderr();
  EXPECT_TRUE(stdout_output.empty()) << stdout_output;
  EXPECT_TRUE(stderr_output.contains("error:")) << stderr_output;
  EXPECT_TRUE(stderr_output.contains("Invalid")) << stderr_output;
  EXPECT_TRUE(stderr_output.contains("Usage:")) << stderr_output;
}

TEST(HogDevnode, InvalidRamdbBytesPrintsFriendlyUsageBeforeInitialization)
{
  expect_startup_failure({"--ramdb-bytes", "not-a-size"},
                         "Invalid --ramdb-bytes: not-a-size");
}

TEST(HogDevnode, ZeroRamdbBytesPrintsFriendlyUsageBeforeInitialization)
{
  expect_startup_failure({"--ramdb-bytes", "0"},
                         "--ramdb-bytes must be positive");
}

TEST(HogDevnode, RamdbReservationFailurePrintsDiagnostic)
{
  // 1 EiB cannot be reserved on any host; the failure must name the size and
  // point at the flag instead of the bare "Cannot allocate memory" it used
  // to print.
  expect_startup_failure({"--ramdb-bytes", "1152921504606846976"},
                         "ramdb failed to reserve 1073741824.0 GiB");
}

TEST(HogDevnode, RamdbReservationFailureReferencesFlag)
{
  expect_startup_failure({"--ramdb-bytes", "1152921504606846976"},
                         "Reduce capacity with --ramdb-bytes.");
}

TEST(HogDevnode, ReducedRamdbBytesReachesReadiness)
{
  // 2 GiB instead of the 15 GiB default; mirrors the low-memory (e.g. WSL)
  // configuration the flag exists for.
  HogDevnodeProcess process =
      HogDevnodeProcess::start({"--ramdb-bytes", "2147483648"});
  process.wait_for_readiness();
  expect_clean_exit(process);
}

TEST(HogDevnode,
     InvalidReadinessArgumentPrintsFriendlyUsageBeforeInitialization)
{
  HogDevnodeProcess process =
      HogDevnodeProcess::start({"--readiness", "stderr"});

  std::optional<int> status = process.wait_for_exit_for_test(exit_timeout());
  if (!status.has_value()) {
    FAIL() << "hog-devnode did not exit";
    return;
  }
  const int exit_status = status.value();
  ASSERT_TRUE(WIFEXITED(exit_status));
  EXPECT_EQ(WEXITSTATUS(exit_status), EXIT_FAILURE);

  std::string stdout_output = process.read_remaining_stdout();
  std::string stderr_output = process.read_remaining_stderr();
  EXPECT_TRUE(stdout_output.empty()) << stdout_output;
  EXPECT_TRUE(stderr_output.contains("error:")) << stderr_output;
  EXPECT_TRUE(stderr_output.contains("Invalid readiness sink: stderr"))
      << stderr_output;
  EXPECT_TRUE(stderr_output.contains("Usage:")) << stderr_output;
}

TEST(HogDevnode, InvalidDevWalletProfilePrintsFriendlyUsageBeforeInitialization)
{
  HogDevnodeProcess process =
      HogDevnodeProcess::start({"--dev-wallet-profile", "not-a-profile"});

  std::optional<int> status = process.wait_for_exit_for_test(exit_timeout());
  if (!status.has_value()) {
    FAIL() << "hog-devnode did not exit";
    return;
  }
  const int exit_status = status.value();
  ASSERT_TRUE(WIFEXITED(exit_status));
  EXPECT_EQ(WEXITSTATUS(exit_status), EXIT_FAILURE);

  std::string stdout_output = process.read_remaining_stdout();
  std::string stderr_output = process.read_remaining_stderr();
  EXPECT_TRUE(stdout_output.empty()) << stdout_output;
  EXPECT_TRUE(stderr_output.contains("unsupported dev wallet profile"))
      << stderr_output;
  EXPECT_TRUE(stderr_output.contains("Usage:")) << stderr_output;
}

TEST(HogDevnode, ContractArtifactDirectoryPrependsAndFallsBackToDefaults)
{
  std::filesystem::path missing_dir =
      std::filesystem::temp_directory_path() /
      std::format("hog-devnode-missing-contract-artifacts-{}-{}", getpid(),
                  std::chrono::steady_clock::now().time_since_epoch().count());
  HogDevnodeProcess process = HogDevnodeProcess::start(
      {"--contract-artifacts-dir", missing_dir.string(), "--dev-wallet-profile",
       "hog-devnode-default"});
  Readiness readiness = process.wait_for_readiness();

  const json::Value *bootstrap = readiness.manifest.find("bootstrap");
  ASSERT_NE(bootstrap, nullptr);
  const json::Value *dev_wallet = bootstrap->find("devWallet");
  ASSERT_NE(dev_wallet, nullptr);
  EXPECT_EQ(require_string_field(*dev_wallet, "artifactHint", ""),
            "contracts/dev_wallet.rv32a");

  expect_clean_exit(process);
}

TEST(HogDevnode, RejectsWildcardListenHostBeforeInitialization)
{
  for (std::string_view host : {"0.0.0.0", "::", "[::]", "0:0:0:0:0:0:0:0",
                                "[0:0:0:0:0:0:0:0]", "::0"}) {
    SCOPED_TRACE(host);
    HogDevnodeProcess process =
        HogDevnodeProcess::start({"--listen-host", std::string(host)});

    std::optional<int> status = process.wait_for_exit_for_test(exit_timeout());
    if (!status.has_value()) {
      FAIL() << "hog-devnode did not exit";
      return;
    }
    const int exit_status = status.value();
    ASSERT_TRUE(WIFEXITED(exit_status));
    EXPECT_EQ(WEXITSTATUS(exit_status), EXIT_FAILURE);

    std::string stdout_output = process.read_remaining_stdout();
    std::string stderr_output = process.read_remaining_stderr();
    EXPECT_TRUE(stdout_output.empty()) << stdout_output;
    EXPECT_TRUE(
        stderr_output.contains("wildcard addresses cannot be advertised"))
        << stderr_output;
    EXPECT_TRUE(stderr_output.contains("Usage:")) << stderr_output;
  }
}

TEST(HogDevnode, InvalidLogLevelPrintsFriendlyUsageBeforeInitialization)
{
  HogDevnodeProcess process =
      HogDevnodeProcess::start({"--log-level", "chatty"});

  std::optional<int> status = process.wait_for_exit_for_test(exit_timeout());
  if (!status.has_value()) {
    FAIL() << "hog-devnode did not exit";
    return;
  }
  const int exit_status = status.value();
  ASSERT_TRUE(WIFEXITED(exit_status));
  EXPECT_EQ(WEXITSTATUS(exit_status), EXIT_FAILURE);

  std::string stdout_output = process.read_remaining_stdout();
  std::string stderr_output = process.read_remaining_stderr();
  EXPECT_TRUE(stdout_output.empty()) << stdout_output;
  EXPECT_TRUE(stderr_output.contains("error:")) << stderr_output;
  EXPECT_TRUE(stderr_output.contains("Invalid log level: chatty"))
      << stderr_output;
  EXPECT_TRUE(stderr_output.contains("Usage:")) << stderr_output;
}

TEST(HogDevnode, DevAccountCountAboveMaximumPrintsFriendlyUsage)
{
  HogDevnodeProcess process =
      HogDevnodeProcess::start({"--dev-accounts", "2000"});

  std::optional<int> status = process.wait_for_exit_for_test(exit_timeout());
  if (!status.has_value()) {
    FAIL() << "hog-devnode did not exit";
    return;
  }
  const int exit_status = status.value();
  ASSERT_TRUE(WIFEXITED(exit_status));
  EXPECT_EQ(WEXITSTATUS(exit_status), EXIT_FAILURE);

  std::string stdout_output = process.read_remaining_stdout();
  std::string stderr_output = process.read_remaining_stderr();
  EXPECT_TRUE(stdout_output.empty()) << stdout_output;
  EXPECT_TRUE(stderr_output.contains("error:")) << stderr_output;
  EXPECT_TRUE(stderr_output.contains("exceeds maximum")) << stderr_output;
  EXPECT_TRUE(stderr_output.contains("Usage:")) << stderr_output;
}

TEST(HogDevnode, BootstrapManifestConflictsWithShorthandPrintsFriendlyUsage)
{
  HogDevnodeProcess process = HogDevnodeProcess::start(
      {"--bootstrap-manifest", "/nonexistent.json", "--dev-accounts", "1"});

  std::optional<int> status = process.wait_for_exit_for_test(exit_timeout());
  if (!status.has_value()) {
    FAIL() << "hog-devnode did not exit";
    return;
  }
  const int exit_status = status.value();
  ASSERT_TRUE(WIFEXITED(exit_status));
  EXPECT_EQ(WEXITSTATUS(exit_status), EXIT_FAILURE);

  std::string stdout_output = process.read_remaining_stdout();
  std::string stderr_output = process.read_remaining_stderr();
  EXPECT_TRUE(stdout_output.empty()) << stdout_output;
  EXPECT_TRUE(stderr_output.contains("error:")) << stderr_output;
  EXPECT_TRUE(
      stderr_output.contains("--bootstrap-manifest conflicts with dev-account"))
      << stderr_output;
  EXPECT_TRUE(stderr_output.contains("Usage:")) << stderr_output;
}

TEST(HogDevnode, JsonLogFormatKeepsReadinessOnStdoutAndLogsJsonToStderr)
{
  // Disable LeakSanitizer so its print_suppressions output (which is
  // non-JSON) does not pollute stderr when running under ASAN.
  HogDevnodeProcess process =
      HogDevnodeProcess::start({}, {{"HOGD_LOG_FORMAT", "json"},
                                    {"LSAN_OPTIONS", "detect_leaks=0"}});
  Readiness readiness = process.wait_for_readiness();

  EXPECT_EQ(readiness.scheduler.host, "127.0.0.1");
  int status = process.terminate();
  std::string stderr_output = process.read_remaining_stderr();
  ASSERT_TRUE(WIFEXITED(status)) << stderr_output;
  EXPECT_EQ(WEXITSTATUS(status), EXIT_SUCCESS) << stderr_output;
  EXPECT_TRUE(process.read_remaining_stdout().empty());
  ASSERT_FALSE(stderr_output.empty());

  bool saw_log_record = false;
  bool saw_hog_devnode_target = false;
  std::size_t start = 0;
  while (start < stderr_output.size()) {
    std::size_t end = stderr_output.find('\n', start);
    std::string_view line =
        end == std::string::npos
            ? std::string_view(stderr_output).substr(start)
            : std::string_view(stderr_output).substr(start, end - start);
    start = end == std::string::npos ? stderr_output.size() : end + 1;
    if (line.empty()) {
      continue;
    }

    auto parsed = json::parse(line);
    ASSERT_TRUE(parsed.has_value()) << line;
    const json::Value *level = parsed->find("level");
    const json::Value *timestamp_ms = parsed->find("timestamp_ms");
    const json::Value *target = parsed->find("target");
    const json::Value *message = parsed->find("message");
    const json::Value *fields = parsed->find("fields");
    ASSERT_NE(level, nullptr) << line;
    ASSERT_NE(timestamp_ms, nullptr) << line;
    ASSERT_NE(target, nullptr) << line;
    ASSERT_NE(message, nullptr) << line;
    ASSERT_NE(fields, nullptr) << line;
    EXPECT_TRUE(level->as_string().has_value()) << line;
    std::optional<std::int64_t> timestamp = timestamp_ms->as_integer();
    if (!timestamp.has_value()) {
      FAIL() << line;
      return;
    }
    EXPECT_GT(timestamp.value(), 0);
    EXPECT_TRUE(target->as_string().has_value()) << line;
    EXPECT_TRUE(message->as_string().has_value()) << line;
    ASSERT_EQ(fields->type(), json::Value::Type::kObject) << line;
    const json::Value *file = fields->find("file");
    const json::Value *line_number = fields->find("line");
    ASSERT_NE(file, nullptr) << line;
    ASSERT_NE(line_number, nullptr) << line;
    EXPECT_TRUE(file->as_string().has_value()) << line;
    EXPECT_TRUE(line_number->as_integer().has_value()) << line;
    if (target->as_string() == "devnode") {
      saw_hog_devnode_target = true;
    }
    saw_log_record = true;
  }
  EXPECT_TRUE(saw_log_record);
  EXPECT_TRUE(saw_hog_devnode_target);
}

TEST(HogDevnode, ConstructorFailureAfterExecutorStartExitsBeforeReadiness)
{
  HogDevnodeProcess process = HogDevnodeProcess::start(
      {}, {{"HOGD_LOG_FORMAT", "json"},
           {"HOG_DEVNODE_TEST_THROW_AFTER_EXECUTOR_START", "1"}});

  std::optional<int> status = process.wait_for_exit_for_test(exit_timeout());
  if (!status.has_value()) {
    FAIL() << "hog-devnode did not exit after injected constructor failure";
    return;
  }
  const int exit_status = status.value();
  ASSERT_TRUE(WIFEXITED(exit_status));
  EXPECT_EQ(WEXITSTATUS(exit_status), EXIT_FAILURE);

  std::string stdout_output = process.read_remaining_stdout();
  std::string stderr_output = process.read_remaining_stderr();
  EXPECT_TRUE(stdout_output.empty()) << stdout_output;
  EXPECT_TRUE(stderr_output.contains("hog-devnode failed before readiness"))
      << stderr_output;
  EXPECT_TRUE(
      stderr_output.contains("injected hog-devnode constructor failure"))
      << stderr_output;
}

TEST(HogDevnode, FormatsSchedulerEndpointAsCanonicalHostPort)
{
  EXPECT_EQ(format_advertised_address("127.0.0.1", 12345), "127.0.0.1:12345");
  EXPECT_EQ(format_advertised_address("::1", 12345), "[::1]:12345");
}

TEST(HogDevnode, ReadinessParserAcceptsBracketedIpv6Endpoints)
{
  std::filesystem::path manifest_path =
      std::filesystem::temp_directory_path() /
      std::format("hog-devnode-test-manifest-{}.json", getpid());
  Defer cleanup_manifest([&] { std::filesystem::remove(manifest_path); });
  std::string manifest =
      R"({"schemaVersion":2,"kind":"hogd-dev-runtime","chainState":"fresh",)"
      R"("networkHash":"0000000000000000000000000000000000000000000000000000000000000000","protocol":"hogp/test",)"
      R"("producer":{"binary":"hog-devnode","version":"test"},)"
      R"("endpoints":{"scheduler":"[::1]:12345","indexer":"[::1]:23456",)"
      R"("dbGateway":"[::1]:34567","viewGateway":"[::1]:45678"},)"
      R"("bootstrapAccountDerivation":{"scheme":"groundhog-bootstrap-account-v1","seed":"test"},)"
      R"("bootstrapAccounts":[],"bootstrap":{"devWallet":{)"
      R"("contentHash":"0x0000000000000000000000000000000000000000000000000000000000000000",)"
      R"("deployedCodeHash":"0x0000000000000000000000000000000000000000000000000000000000000000",)"
      R"("artifactHint":"contracts/dev_wallet.rv32a",)"
      R"("deployEntrypoint":"0x0000000000000000","doActionEntrypoint":"0x0000000000000000"},)"
      R"("storageCostModel":{"hash":"0x0000000000000000000000000000000000000000000000000000000000000000",)"
      R"("baseDataItemCost":"12000","bytestringCostPerByte":"10",)"
      R"("baseContractDeploymentCost":"12000","contractDeploymentCostPerByte":"10",)"
      R"("authPerKeyCost":"1111","rcqCostPerEntry":"8",)"
      R"("replayCacheCostPerEntry":"7","balanceCost":"1234"}}})";
  {
    std::ofstream output(manifest_path, std::ios::binary);
    output << manifest;
  }
  Readiness readiness = parse_readiness(
      std::format(R"({{"schemaVersion":2,"status":"ready",)"
                  R"("manifestPath":"{}","manifestHash":"{}"}})",
                  manifest_path.string(), hex_string(hash_string(manifest))));

  EXPECT_EQ(readiness.scheduler.host, "::1");
  EXPECT_EQ(readiness.scheduler.port, 12345);
  EXPECT_EQ(readiness.indexer.host, "::1");
  EXPECT_EQ(readiness.indexer.port, 23456);
  EXPECT_EQ(readiness.db_gateway.host, "::1");
  EXPECT_EQ(readiness.db_gateway.port, 34567);
  EXPECT_EQ(readiness.view_gateway.host, "::1");
  EXPECT_EQ(readiness.view_gateway.port, 45678);
}

TEST(HogDevnode, BootstrapAccountDerivationMatchesRustGoldens)
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
    auto keypair = derive_bootstrap_account_keypair(kDefaultSeed, i);
    EXPECT_EQ(hex_string(keypair.first), expected[i]);
  }
}

TEST(HogDevnode, BootstrapArtifactBuildsStableAccounts)
{
  LocalBootstrapConfig config{.user_account_count = 3};

  BootstrapAccountArtifact first =
      build_local_bootstrap_account_artifact(config);
  BootstrapAccountArtifact second =
      build_local_bootstrap_account_artifact(config);

  ASSERT_EQ(first.accounts.size(), 4UL);
  ASSERT_EQ(first.initialization_txs.size(), 1UL);
  ASSERT_EQ(second.accounts.size(), first.accounts.size());

  for (std::uint32_t i = 0; i < first.accounts.size(); ++i) {
    EXPECT_EQ(first.accounts[i].index, i);
    EXPECT_EQ(first.accounts[i].role, i == 0
                                          ? BootstrapAccountRole::kFaucet
                                          : BootstrapAccountRole::kLocalUser);
    const std::uint64_t expected_tokens =
        i == 0 ? kLocalFaucetStorageTokens : config.user_storage_tokens;
    EXPECT_EQ(first.accounts[i].initial_storage_tokens, expected_tokens);
    EXPECT_EQ(first.accounts[i].storage_tokens, expected_tokens);
    EXPECT_EQ(first.accounts[i].address, second.accounts[i].address);
    EXPECT_EQ(first.accounts[i].public_key, second.accounts[i].public_key);
  }
}

TEST(HogDevnode, LocalUserDefaultRemainsOneHundredHog)
{
  LocalBootstrapConfig config{.user_account_count = 1};
  EXPECT_EQ(config.user_storage_tokens, 1'000'000'000ULL);

  BootstrapAccountArtifact artifact =
      build_local_bootstrap_account_artifact(config);
  ASSERT_EQ(artifact.accounts.size(), 2UL);
  EXPECT_EQ(artifact.accounts[0].initial_storage_tokens,
            kLocalFaucetStorageTokens);
  EXPECT_EQ(artifact.accounts[1].initial_storage_tokens, 1'000'000'000ULL);
}

TEST(HogDevnode, LocalBootstrapRejectsAllocationAboveTokenSupply)
{
  constexpr std::uint64_t kTokenSupply =
      kTotalHogSupply * std::uint64_t{kHogDivisibility};
  LocalBootstrapConfig config{
      .user_account_count = 1,
      .user_storage_tokens = kTokenSupply,
  };

  EXPECT_THROW(build_local_bootstrap_account_artifact(config),
               std::runtime_error);
}

TEST(HogDevnode, BootstrapManifestAdvertisesAccountMetadata)
{
  HogDevnodeProcess process = HogDevnodeProcess::start({
      "--dev-accounts",
      "3",
      "--dev-account-seed",
      "groundhog-dev-accounts",
      "--dev-account-tokens",
      "1000000000",
  });
  Readiness readiness = process.wait_for_readiness();

  ASSERT_EQ(readiness.bootstrap_accounts.size(), 4UL);
  const json::Value *derivation =
      readiness.manifest.find("bootstrapAccountDerivation");
  ASSERT_NE(derivation, nullptr);
  EXPECT_EQ(require_string_field(*derivation, "scheme", ""),
            "groundhog-bootstrap-account-v1");
  EXPECT_EQ(require_string_field(*derivation, "seed", ""),
            "groundhog-dev-accounts");
  for (std::uint32_t i = 0; i < readiness.bootstrap_accounts.size(); ++i) {
    auto keypair =
        derive_bootstrap_account_keypair("groundhog-dev-accounts", i);
    EXPECT_EQ(readiness.bootstrap_accounts[i].index, i);
    EXPECT_EQ(readiness.bootstrap_accounts[i].role,
              i == 0 ? "faucet" : "local_user");
    EXPECT_EQ(readiness.bootstrap_accounts[i].public_key,
              hex_string(keypair.first));
    EXPECT_TRUE(readiness.bootstrap_accounts[i].address.starts_with("0x"));
    EXPECT_EQ(readiness.bootstrap_accounts[i].address.size(), 66UL);
    EXPECT_EQ(readiness.bootstrap_accounts[i].initial_storage_tokens,
              i == 0 ? "10000000000000" : "1000000000");
  }

  expect_clean_exit(process);
}

TEST(HogDevnode, BootstrapManifestInputWithoutProtocolAdvertisesAccountMetadata)
{
  auto wallet_contract = load_default_dev_wallet_contract();
  const std::string manifest =
      bootstrap_manifest_json(2, "manifest-seed", "1000000000",
                              hex_string(sha256(*wallet_contract)));
  std::filesystem::path manifest_path = write_test_bootstrap_manifest(manifest);
  Defer cleanup_manifest([&] { std::filesystem::remove(manifest_path); });

  HogDevnodeProcess process = HogDevnodeProcess::start({
      "--bootstrap-manifest",
      manifest_path.string(),
  });
  Readiness readiness = process.wait_for_readiness();

  EXPECT_EQ(readiness.network_hash,
            "00000000000000000000000000000000000000000000000000000000000000aa");
  ASSERT_EQ(readiness.bootstrap_accounts.size(), 3UL);
  const json::Value *derivation =
      readiness.manifest.find("bootstrapAccountDerivation");
  ASSERT_NE(derivation, nullptr);
  EXPECT_EQ(require_string_field(*derivation, "seed", ""), "manifest-seed");
  for (std::uint32_t i = 0; i < readiness.bootstrap_accounts.size(); ++i) {
    auto keypair = derive_bootstrap_account_keypair("manifest-seed", i);
    EXPECT_EQ(readiness.bootstrap_accounts[i].index, i);
    EXPECT_EQ(readiness.bootstrap_accounts[i].role,
              i == 0 ? "faucet" : "local_user");
    EXPECT_EQ(readiness.bootstrap_accounts[i].public_key,
              hex_string(keypair.first));
    EXPECT_EQ(readiness.bootstrap_accounts[i].initial_storage_tokens,
              i == 0 ? "10000000000000" : "1000000000");
  }

  expect_clean_exit(process);
}

TEST(HogDevnode, BootstrapManifestConflictsWithDevAccountShorthands)
{
  auto wallet_contract = load_default_dev_wallet_contract();
  const std::string manifest =
      bootstrap_manifest_json(1, "manifest-seed", "42",
                              hex_string(sha256(*wallet_contract)));
  std::filesystem::path manifest_path = write_test_bootstrap_manifest(manifest);
  Defer cleanup_manifest([&] { std::filesystem::remove(manifest_path); });

  HogDevnodeProcess process = HogDevnodeProcess::start({
      "--bootstrap-manifest",
      manifest_path.string(),
      "--dev-accounts",
      "1",
  });

  std::optional<int> status = process.wait_for_exit_for_test(startup_timeout());
  ASSERT_TRUE(status.has_value());
  const int wait_status = status.value_or(EXIT_SUCCESS);
  EXPECT_FALSE(WIFEXITED(wait_status) &&
               WEXITSTATUS(wait_status) == EXIT_SUCCESS);
  EXPECT_TRUE(process.read_remaining_stdout().empty());
  EXPECT_NE(process.read_remaining_stderr().find(
                "--bootstrap-manifest conflicts with dev-account"),
            std::string::npos);
}

TEST(HogDevnode, BootstrapManifestConflictDetectedBeforeManifestRead)
{
  std::filesystem::path missing_path = missing_test_bootstrap_manifest_path();
  HogDevnodeProcess process = HogDevnodeProcess::start({
      "--bootstrap-manifest",
      missing_path.string(),
      "--dev-accounts",
      "1",
  });

  std::optional<int> status = process.wait_for_exit_for_test(startup_timeout());
  ASSERT_TRUE(status.has_value());
  const int wait_status = status.value_or(EXIT_SUCCESS);
  EXPECT_FALSE(WIFEXITED(wait_status) &&
               WEXITSTATUS(wait_status) == EXIT_SUCCESS);
  EXPECT_TRUE(process.read_remaining_stdout().empty());
  EXPECT_NE(process.read_remaining_stderr().find(
                "--bootstrap-manifest conflicts with dev-account"),
            std::string::npos);
}

TEST(HogDevnode, BootstrapManifestConflictDetectedBeforeShorthandParsing)
{
  std::filesystem::path missing_path = missing_test_bootstrap_manifest_path();
  HogDevnodeProcess process = HogDevnodeProcess::start({
      "--bootstrap-manifest",
      missing_path.string(),
      "--dev-accounts",
      "not-a-count",
  });

  std::optional<int> status = process.wait_for_exit_for_test(startup_timeout());
  ASSERT_TRUE(status.has_value());
  const int wait_status = status.value_or(EXIT_SUCCESS);
  EXPECT_FALSE(WIFEXITED(wait_status) &&
               WEXITSTATUS(wait_status) == EXIT_SUCCESS);
  EXPECT_TRUE(process.read_remaining_stdout().empty());
  EXPECT_NE(process.read_remaining_stderr().find(
                "--bootstrap-manifest conflicts with dev-account"),
            std::string::npos);
}

TEST(HogDevnode, BootstrapManifestRejectsOversizedFile)
{
  std::filesystem::path manifest_path =
      write_test_bootstrap_manifest(std::string((64 * 1024) + 1, ' '));
  Defer cleanup_manifest([&] { std::filesystem::remove(manifest_path); });

  HogDevnodeProcess process = HogDevnodeProcess::start({
      "--bootstrap-manifest",
      manifest_path.string(),
  });

  std::optional<int> status = process.wait_for_exit_for_test(startup_timeout());
  ASSERT_TRUE(status.has_value());
  const int wait_status = status.value_or(EXIT_SUCCESS);
  EXPECT_FALSE(WIFEXITED(wait_status) &&
               WEXITSTATUS(wait_status) == EXIT_SUCCESS);
  EXPECT_TRUE(process.read_remaining_stdout().empty());
  EXPECT_NE(process.read_remaining_stderr().find("bootstrap manifest exceeds"),
            std::string::npos);
}

TEST(HogDevnode, BootstrapManifestRejectsMismatchedDevWalletHash)
{
  const std::string manifest = bootstrap_manifest_json(
      1, "manifest-seed", "42",
      "0x9999999999999999999999999999999999999999999999999999999999999999");
  std::filesystem::path manifest_path = write_test_bootstrap_manifest(manifest);
  Defer cleanup_manifest([&] { std::filesystem::remove(manifest_path); });

  HogDevnodeProcess process = HogDevnodeProcess::start({
      "--bootstrap-manifest",
      manifest_path.string(),
  });

  std::optional<int> status = process.wait_for_exit_for_test(startup_timeout());
  ASSERT_TRUE(status.has_value());
  const int wait_status = status.value_or(EXIT_SUCCESS);
  EXPECT_FALSE(WIFEXITED(wait_status) &&
               WEXITSTATUS(wait_status) == EXIT_SUCCESS);
  EXPECT_TRUE(process.read_remaining_stdout().empty());
  EXPECT_NE(process.read_remaining_stderr().find("devWallet.contentHash"),
            std::string::npos);
}

TEST(HogDevnode, BootstrapManifestRejectsEmptyBootstrapAccountsSeed)
{
  auto wallet_contract = load_default_dev_wallet_contract();
  const std::string manifest =
      bootstrap_manifest_json(1, "", "42",
                              hex_string(sha256(*wallet_contract)));
  std::filesystem::path manifest_path = write_test_bootstrap_manifest(manifest);
  Defer cleanup_manifest([&] { std::filesystem::remove(manifest_path); });

  HogDevnodeProcess process = HogDevnodeProcess::start({
      "--bootstrap-manifest",
      manifest_path.string(),
  });

  std::optional<int> status = process.wait_for_exit_for_test(startup_timeout());
  ASSERT_TRUE(status.has_value());
  const int wait_status = status.value_or(EXIT_SUCCESS);
  EXPECT_FALSE(WIFEXITED(wait_status) &&
               WEXITSTATUS(wait_status) == EXIT_SUCCESS);
  EXPECT_TRUE(process.read_remaining_stdout().empty());
  EXPECT_NE(process.read_remaining_stderr().find(
                "bootstrapAccounts.seed must not be empty"),
            std::string::npos);
}

TEST(HogDevnode, BootstrapManifestRejectsWhitespaceInInitialStorageTokens)
{
  auto wallet_contract = load_default_dev_wallet_contract();
  const std::string manifest =
      bootstrap_manifest_json(1, "manifest-seed", " 1000",
                              hex_string(sha256(*wallet_contract)));
  std::filesystem::path manifest_path = write_test_bootstrap_manifest(manifest);
  Defer cleanup_manifest([&] { std::filesystem::remove(manifest_path); });

  HogDevnodeProcess process = HogDevnodeProcess::start({
      "--bootstrap-manifest",
      manifest_path.string(),
  });

  std::optional<int> status = process.wait_for_exit_for_test(startup_timeout());
  ASSERT_TRUE(status.has_value());
  const int wait_status = status.value_or(EXIT_SUCCESS);
  EXPECT_FALSE(WIFEXITED(wait_status) &&
               WEXITSTATUS(wait_status) == EXIT_SUCCESS);
  EXPECT_TRUE(process.read_remaining_stdout().empty());
  EXPECT_NE(process.read_remaining_stderr().find(
                "Invalid bootstrap manifest initialStorageTokens"),
            std::string::npos);
}

TEST(HogDevnode, RejectsDevAccountCountAboveLimit)
{
  HogDevnodeProcess process = HogDevnodeProcess::start({
      "--dev-accounts",
      "1001",
  });

  std::optional<int> status = process.wait_for_exit_for_test(startup_timeout());
  ASSERT_TRUE(status.has_value());
  const int wait_status = status.value_or(EXIT_SUCCESS);
  EXPECT_FALSE(WIFEXITED(wait_status) &&
               WEXITSTATUS(wait_status) == EXIT_SUCCESS);
  EXPECT_TRUE(process.read_remaining_stdout().empty());
  EXPECT_NE(process.read_remaining_stderr().find("Invalid dev account count"),
            std::string::npos);
}

TEST(HogDevnode, SchedulerTipReportsFaucetInitializationAfterReadiness)
{
  HogDevnodeProcess process = HogDevnodeProcess::start();
  Readiness readiness = process.wait_for_readiness();

  {
    sched_coord_client_t client =
        create_client<SchedulerCoord1>(readiness.scheduler.host,
                                       readiness.scheduler.port,
                                       /*retry=*/true)()
            .value();

    SchedulerGetTipResult tip = client.schedcoord_get_tip()();
    ASSERT_EQ(tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    EXPECT_EQ(tip.db_tip().value.proposal_header.block_height, 1);
    EXPECT_NE(tip.db_tip().id, 0UL);
  }

  expect_clean_exit(process);
}

TEST(HogDevnode, GatewayEndpointAnswersIntroAndGetItems)
{
  HogDevnodeProcess process = HogDevnodeProcess::start();
  Readiness readiness = process.wait_for_readiness();

  {
    auto client = create_client<DbGateway1>(readiness.db_gateway.host,
                                            readiness.db_gateway.port,
                                            /*retry=*/true)()
                      .value();

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

TEST(HogDevnode, ViewGatewayEndpointAnswersIntro)
{
  HogDevnodeProcess process = HogDevnodeProcess::start();
  Readiness readiness = process.wait_for_readiness();

  {
    auto client = create_client<ViewGateway1>(readiness.view_gateway.host,
                                              readiness.view_gateway.port,
                                              /*retry=*/true)()
                      .value();

    EXPECT_EQ(client.viewgateway_intro(version::kHogdProtocol())(),
              ViewGatewayIntroOutcome::ViewGatewayIntroSuccess);
  }

  expect_clean_exit(process);
}

TEST(HogDevnode, ViewGatewayViewsCommittedBootstrapWallet)
{
  HogDevnodeProcess process = HogDevnodeProcess::start({
      "--dev-accounts",
      "1",
      "--dev-account-seed",
      "groundhog-dev-accounts",
      "--dev-account-tokens",
      "1000000000",
  });
  Readiness readiness = process.wait_for_readiness();
  ASSERT_EQ(readiness.bootstrap_accounts.size(), 2UL);

  {
    auto client = create_client<ViewGateway1>(readiness.view_gateway.host,
                                              readiness.view_gateway.port,
                                              /*retry=*/true)()
                      .value();

    EXPECT_EQ(client.viewgateway_intro(version::kHogdProtocol())(),
              ViewGatewayIntroOutcome::ViewGatewayIntroSuccess);

    using wallet_meta = ContractTraitMeta<dev_wallet::DevWallet>;
    const ContractAddress wallet_address =
        build_local_bootstrap_account_artifact({.user_account_count = 1})
            .accounts[1]
            .address;
    EXPECT_EQ(hex_string(wallet_address),
              readiness.bootstrap_accounts[1].address);

    TransactionViewQuery query{
        .invocation =
            Invocation{
                .invoked_address = wallet_address,
                .entrypoint = wallet_meta::entrypoint<u8"do_action">(),
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

TEST(HogDevnode, IndexerEndpointServesBlockExplorer)
{
  HogDevnodeProcess process = HogDevnodeProcess::start();
  Readiness readiness = process.wait_for_readiness();

  {
    auto client = create_client<IndexerBlockExplorer1>(readiness.indexer.host,
                                                       readiness.indexer.port,
                                                       /*retry=*/true)()
                      .value();

    EXPECT_EQ(client.indexer_block_explorer_intro(version::kHogdProtocol())(),
              IndexerBlockExplorerIntroOutcome::IntroSuccess);

    SHA256Hash tx_hash{};
    TxInfoResult result = client.get_transaction_info(tx_hash)();
    EXPECT_EQ(result.outcome(), TxInfoOutcome::TxInfoNotFound);
  }

  expect_clean_exit(process);
}

TEST(HogDevnode, CommittedTransactionsPopulateIndexerEndpoint)
{
  HogDevnodeProcess process = HogDevnodeProcess::start({
      "--dev-accounts",
      "1",
      "--dev-account-seed",
      "groundhog-dev-accounts",
      "--dev-account-tokens",
      "1000000000",
  });
  Readiness readiness = process.wait_for_readiness();
  ASSERT_EQ(readiness.bootstrap_accounts.size(), 2UL);

  {
    sched_coord_client_t scheduler =
        create_client<SchedulerCoord1>(readiness.scheduler.host,
                                       readiness.scheduler.port,
                                       /*retry=*/true)()
            .value();
    auto reader = create_client<IndexerBlockExplorer1>(readiness.indexer.host,
                                                       readiness.indexer.port,
                                                       /*retry=*/true)()
                      .value();

    EXPECT_EQ(reader.indexer_block_explorer_intro(version::kHogdProtocol())(),
              IndexerBlockExplorerIntroOutcome::IntroSuccess);

    AddressTransactionQuery startup_query{};
    startup_query.order = IterationOrder::IterationOrderAscending;
    startup_query.anchor.kind(BlockAnchorKind::AnchorAtBeginning);
    EXPECT_EQ(reader.get_transactions_by_address(startup_query)().latest_block,
              1);

    auto fibonacci_contract = load_rv32_contract("fibonacci.rv32a");
    ASSERT_TRUE(fibonacci_contract);
    auto keypair =
        derive_bootstrap_account_keypair("groundhog-dev-accounts", /*index=*/1);
    using wallet_meta = ContractTraitMeta<dev_wallet::DevWallet>;
    const ContractAddress wallet_address =
        build_local_bootstrap_account_artifact({.user_account_count = 1})
            .accounts[1]
            .address;

    Transaction tx{
        .invocation =
            Invocation{
                .invoked_address = wallet_address,
                .entrypoint = wallet_meta::entrypoint<u8"deploy">(),
                .gas_limit = GasAmount{10'000'000},
                .calldata = wallet_meta::make_calldata<u8"deploy">(
                    make_nonce(11), sha256(*fibonacci_contract)),
            },
        .uniquifier = Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = 11},
        .contracts_to_deploy = {*fibonacci_contract},
        .auth_claim = source_address_auth_claim(),
    };
    Signature signature(CryptographicScheme::Ed25519);
    auto signed_bytes = ed25519_sign(
        keypair.second,
        transaction_signature_preimage(readiness_network_hash(readiness), tx));
    ASSERT_TRUE(signed_bytes.has_value());
    signature.ed25519() = signed_bytes.value_or(Ed25519Signature{});
    SignedTransaction signed_tx{
        .tx = std::move(tx),
        .signatures = {DecoratedSignature{
            .public_key_id = 0,
            .signature = signature,
        }},
    };
    const TransactionHash tx_hash = sha256(signed_tx.tx);
    ASSERT_EQ(
        submit_and_commit(scheduler, SignedTransactionBatch{signed_tx}).size(),
        1UL);

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
    EXPECT_EQ(result.info().height, 2);
    EXPECT_EQ(result.info().events.exec_result, ExecResult::ExecResultOK);
    EXPECT_EQ(result.info().events.tx_hash, tx_hash);
  }

  expect_clean_exit(process);
}

TEST(HogDevnode, MultiHeightIndexerGapReplaysThroughPull)
{
  std::filesystem::path data_dir = make_test_data_dir();
  Defer cleanup_data_dir([&] { std::filesystem::remove_all(data_dir); });

  TransactionHash tx_hash{};
  SignedTransaction expected_signed_tx;
  std::optional<ProposedTransaction> expected_indexed_tx;
  std::optional<TxEventLog> expected_event_log;
  const ContractAddress wallet_address =
      build_local_bootstrap_account_artifact({.user_account_count = 1})
          .accounts[1]
          .address;

  auto expect_recovered_index = [&](auto &reader, BlockHeight latest_height) {
    const auto deadline = std::chrono::steady_clock::now() + kIndexPollTimeout;
    TransactionRefPage page;
    do {
      AddressTransactionQuery query{};
      query.address = wallet_address;
      query.order = IterationOrder::IterationOrderAscending;
      query.anchor.kind(BlockAnchorKind::AnchorAtBeginning);
      page = reader.get_transactions_by_address(query)();
      if (page.latest_block == latest_height) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);

    TxInfoResult result = reader.get_transaction_info(tx_hash)();
    ASSERT_EQ(result.outcome(), TxInfoOutcome::TxInfoFound);
    EXPECT_EQ(result.info().height, 2);
    EXPECT_EQ(result.info().tx.signed_tx, expected_signed_tx);
    ASSERT_TRUE(expected_indexed_tx.has_value());
    ASSERT_TRUE(expected_event_log.has_value());
    EXPECT_EQ(result.info().tx, *expected_indexed_tx);
    EXPECT_EQ(result.info().events, *expected_event_log);
    EXPECT_EQ(page.latest_block, latest_height);
    ASSERT_EQ(page.refs.size(), 1UL);
    EXPECT_EQ(page.refs[0].tx_hash, tx_hash);
    EXPECT_EQ(page.refs[0].height, 2);
  };

  {
    HogDevnodeProcess process = HogDevnodeProcess::start(
        {
            "--data-dir",
            data_dir.string(),
            "--dev-accounts",
            "1",
            "--dev-account-seed",
            "groundhog-dev-accounts",
            "--dev-account-tokens",
            "1000000000",
        },
        {{"HOG_DEVNODE_TEST_WITHHOLD_INDEXER_PULL", "1"}});
    Readiness readiness = process.wait_for_readiness();
    ASSERT_EQ(readiness.bootstrap_accounts.size(), 2UL);
    EXPECT_EQ(readiness.chain_state, "fresh");
    EXPECT_TRUE(readiness.manifest_path.string().starts_with(
        (data_dir / "runtime").string()));
    EXPECT_EQ(path_mode(readiness.manifest_path.parent_path()), 0700);

    sched_coord_client_t scheduler =
        create_client<SchedulerCoord1>(readiness.scheduler.host,
                                       readiness.scheduler.port,
                                       /*retry=*/true)()
            .value();
    auto reader = create_client<IndexerBlockExplorer1>(readiness.indexer.host,
                                                       readiness.indexer.port,
                                                       /*retry=*/true)()
                      .value();

    auto fibonacci_contract = load_rv32_contract("fibonacci.rv32a");
    ASSERT_TRUE(fibonacci_contract);
    auto keypair =
        derive_bootstrap_account_keypair("groundhog-dev-accounts", /*index=*/1);
    using wallet_meta = ContractTraitMeta<dev_wallet::DevWallet>;

    Transaction tx{
        .invocation =
            Invocation{
                .invoked_address = wallet_address,
                .entrypoint = wallet_meta::entrypoint<u8"deploy">(),
                .gas_limit = GasAmount{10'000'000},
                .calldata = wallet_meta::make_calldata<u8"deploy">(
                    make_nonce(17), sha256(*fibonacci_contract)),
            },
        .uniquifier = Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = 17},
        .contracts_to_deploy = {*fibonacci_contract},
        .auth_claim = source_address_auth_claim(),
    };
    Signature signature(CryptographicScheme::Ed25519);
    auto signed_bytes = ed25519_sign(
        keypair.second,
        transaction_signature_preimage(readiness_network_hash(readiness), tx));
    ASSERT_TRUE(signed_bytes.has_value());
    signature.ed25519() = signed_bytes.value_or(Ed25519Signature{});
    SignedTransaction signed_tx{
        .tx = std::move(tx),
        .signatures = {DecoratedSignature{
            .public_key_id = 0,
            .signature = signature,
        }},
    };
    expected_signed_tx = signed_tx;
    tx_hash = sha256(signed_tx.tx);
    TransactionHashBatch committed =
        submit_and_commit(scheduler, SignedTransactionBatch{signed_tx});
    if (committed.size() != 1) {
      const int status = process.terminate();
      ADD_FAILURE() << "expected one committed transaction, process status "
                    << status << ":\n"
                    << process.read_remaining_stderr();
      return;
    }

    EXPECT_TRUE(submit_and_commit(scheduler, {}).empty());
    SchedulerGetTipResult tip = scheduler.schedcoord_get_tip()();
    ASSERT_EQ(tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    EXPECT_EQ(tip.db_tip().value.proposal_header.block_height, 3);

    expect_clean_exit(process);
  }

  {
    auto store =
        IndexerOutboxStore::open_or_create_in((data_dir / "events").string());
    ASSERT_TRUE(store.has_value()) << store.error();
    auto payload = store->load_committed(2)();
    ASSERT_TRUE(payload.has_value()) << payload.error();
    ASSERT_EQ(payload->tx_bodies.size(), 1UL);
    ASSERT_EQ(payload->event_logs.size(), 1UL);
    expected_indexed_tx = payload->tx_bodies[0];
    expected_event_log = payload->event_logs[0];
    EXPECT_EQ(expected_event_log->tx_hash, tx_hash);
  }

  {
    HogDevnodeProcess process = HogDevnodeProcess::start({
        "--data-dir",
        data_dir.string(),
        "--dev-accounts",
        "1",
        "--dev-account-seed",
        "groundhog-dev-accounts",
        "--dev-account-tokens",
        "1000000000",
    });
    Readiness readiness = process.wait_for_readiness();
    ASSERT_EQ(readiness.bootstrap_accounts.size(), 2UL);
    EXPECT_EQ(readiness.chain_state, "recovered");

    sched_coord_client_t scheduler =
        create_client<SchedulerCoord1>(readiness.scheduler.host,
                                       readiness.scheduler.port,
                                       /*retry=*/true)()
            .value();
    SchedulerGetTipResult tip = scheduler.schedcoord_get_tip()();
    ASSERT_EQ(tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    EXPECT_EQ(tip.db_tip().value.proposal_header.block_height, 3);

    auto reader = create_client<IndexerBlockExplorer1>(readiness.indexer.host,
                                                       readiness.indexer.port,
                                                       /*retry=*/true)()
                      .value();
    expect_recovered_index(reader, 3);

    EXPECT_TRUE(submit_and_commit(scheduler, {}).empty());
    tip = scheduler.schedcoord_get_tip()();
    ASSERT_EQ(tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    EXPECT_EQ(tip.db_tip().value.proposal_header.block_height, 4);
    expect_recovered_index(reader, 4);
    expect_clean_exit(process);
  }

  {
    HogDevnodeProcess process = HogDevnodeProcess::start({
        "--data-dir",
        data_dir.string(),
        "--dev-accounts",
        "1",
        "--dev-account-seed",
        "groundhog-dev-accounts",
        "--dev-account-tokens",
        "1000000000",
    });
    Readiness readiness = process.wait_for_readiness();
    ASSERT_EQ(readiness.bootstrap_accounts.size(), 2UL);
    EXPECT_EQ(readiness.chain_state, "recovered");

    sched_coord_client_t scheduler =
        create_client<SchedulerCoord1>(readiness.scheduler.host,
                                       readiness.scheduler.port,
                                       /*retry=*/true)()
            .value();
    SchedulerGetTipResult tip = scheduler.schedcoord_get_tip()();
    ASSERT_EQ(tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    EXPECT_EQ(tip.db_tip().value.proposal_header.block_height, 4);

    auto reader = create_client<IndexerBlockExplorer1>(readiness.indexer.host,
                                                       readiness.indexer.port,
                                                       /*retry=*/true)()
                      .value();
    expect_recovered_index(reader, 4);
    expect_clean_exit(process);
  }
}

TEST(HogDevnode, DataDirRejectsLegacyNetworkHashAfterFaucetInitialization)
{
  std::filesystem::path data_dir = make_test_data_dir();
  Defer cleanup_data_dir([&] { std::filesystem::remove_all(data_dir); });

  auto wallet_contract = load_default_dev_wallet_contract();
  const std::string manifest =
      bootstrap_manifest_json(0, "groundhog-dev-accounts", "1000000000",
                              hex_string(sha256(*wallet_contract)));
  std::filesystem::path manifest_path = write_test_bootstrap_manifest(manifest);
  Defer cleanup_manifest([&] { std::filesystem::remove(manifest_path); });

  auto start_and_check = [&] {
    HogDevnodeProcess process = HogDevnodeProcess::start({
        "--data-dir",
        data_dir.string(),
        "--bootstrap-manifest",
        manifest_path.string(),
    });
    Readiness readiness = process.wait_for_readiness();
    EXPECT_EQ(readiness.chain_state, "fresh");
    EXPECT_EQ(
        readiness.network_hash,
        "00000000000000000000000000000000000000000000000000000000000000aa");

    sched_coord_client_t scheduler =
        create_client<SchedulerCoord1>(readiness.scheduler.host,
                                       readiness.scheduler.port,
                                       /*retry=*/true)()
            .value();
    SchedulerGetTipResult tip = scheduler.schedcoord_get_tip()();
    ASSERT_EQ(tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    EXPECT_EQ(tip.db_tip().value.proposal_header.block_height, 1);
    EXPECT_EQ(format_network_hash(
                  tip.db_tip().value.proposal_header.network_hash),
              readiness.network_hash);

    expect_clean_exit(process);
  };

  start_and_check();
  {
    HogDbDAO db = open_persistent_hog_db(data_dir);
    HogDbStatus<DbTip> tip = db.read_dbtip();
    ASSERT_TRUE(tip.has_value()) << tip.error();
    tip->value.proposal_header.network_hash = {};
    ASSERT_TRUE(db.write_dbtip(*tip).has_value());
    ASSERT_TRUE(db.commit().has_value());
  }
  expect_startup_failure(
      {
          "--data-dir",
          data_dir.string(),
          "--bootstrap-manifest",
          manifest_path.string(),
      },
      "does not match requested network hash");
}

TEST(HogDevnode, DataDirRejectsRestartWithDifferentNetworkHash)
{
  std::filesystem::path data_dir = make_test_data_dir();
  Defer cleanup_data_dir([&] { std::filesystem::remove_all(data_dir); });
  initialize_persistent_data_dir(data_dir);
  ASSERT_TRUE(std::filesystem::remove(data_dir / "hog-devnode-data.json"));

  auto wallet_contract = load_default_dev_wallet_contract();
  const std::string manifest =
      bootstrap_manifest_json(1, "groundhog-dev-accounts", "1000000000",
                              hex_string(sha256(*wallet_contract)));
  std::filesystem::path manifest_path = write_test_bootstrap_manifest(manifest);
  Defer cleanup_manifest([&] { std::filesystem::remove(manifest_path); });

  expect_startup_failure(
      {
          "--data-dir",
          data_dir.string(),
          "--bootstrap-manifest",
          manifest_path.string(),
      },
      "recovered chain network hash "
      "0000000000000000000000000000000000000000000000000000000000000000 "
      "does not match requested network hash "
      "00000000000000000000000000000000000000000000000000000000000000aa");
}

TEST(HogDevnode, DataDirRejectsCandidateOnlyOutboxWithoutCurrentOrigin)
{
  std::filesystem::path data_dir = make_test_data_dir();
  Defer cleanup_data_dir([&] { std::filesystem::remove_all(data_dir); });
  initialize_persistent_data_dir(data_dir, 0);

  {
    auto store = IndexerOutboxStore::open(
        (data_dir / "events" / "indexer-outbox").string());
    ASSERT_TRUE(store.has_value()) << store.error();
    IndexerOutboxCandidatePayload candidate{
        .format_version = kIndexerOutboxFormatVersion,
        .block_id = 91,
        .block_height = 1,
        .event_root = compute_indexer_outbox_event_root({}, {}),
    };
    ASSERT_TRUE(store->publish_candidate(candidate)().has_value());
  }
  ASSERT_TRUE(std::filesystem::remove(
      data_dir / "events" / "indexer-outbox" /
      IndexerOutboxStore::current_epoch_origin_filename()));

  expect_startup_failure({"--data-dir", data_dir.string(), "--dev-accounts",
                          "0", "--dev-account-seed", "groundhog-dev-accounts",
                          "--dev-account-tokens", "1000000000"},
                         "recovered devnode chain has no durable outbox "
                         "origin; clean chain rollout "
                         "required");
}

TEST(HogDevnode, DataDirRejectsArchivalOriginWithoutCurrentOrigin)
{
  std::filesystem::path data_dir = make_test_data_dir();
  Defer cleanup_data_dir([&] { std::filesystem::remove_all(data_dir); });
  initialize_persistent_data_dir(data_dir, 0);
  const std::filesystem::path outbox_dir =
      data_dir / "events" / "indexer-outbox";

  std::filesystem::rename(
      outbox_dir / IndexerOutboxStore::current_epoch_origin_filename(),
      outbox_dir / IndexerOutboxStore::epoch_origin_filename("hogp/archival"));

  expect_startup_failure({"--data-dir", data_dir.string(), "--dev-accounts",
                          "0", "--dev-account-seed", "groundhog-dev-accounts",
                          "--dev-account-tokens", "1000000000"},
                         "recovered devnode chain has no durable outbox "
                         "origin; clean chain rollout "
                         "required");
}

TEST(HogDevnode, DataDirRejectsLegacyChainWithEmptyDurableOutbox)
{
  std::filesystem::path data_dir = make_test_data_dir();
  Defer cleanup_data_dir([&] { std::filesystem::remove_all(data_dir); });
  initialize_persistent_data_dir(data_dir);
  ASSERT_GT(std::filesystem::remove_all(data_dir / "events" / "indexer-outbox"),
            0UL);

  expect_startup_failure({"--data-dir", data_dir.string(), "--dev-accounts",
                          "1", "--dev-account-seed", "groundhog-dev-accounts",
                          "--dev-account-tokens", "1000000000"},
                         "recovered devnode chain has no durable outbox "
                         "origin; clean chain rollout "
                         "required");
}

TEST(HogDevnode, DataDirAcceptsFinalizedOutboxAndIndexerAheadOfHogDb)
{
  std::filesystem::path data_dir = make_test_data_dir();
  Defer cleanup_data_dir([&] { std::filesystem::remove_all(data_dir); });
  std::filesystem::path ahead_data_dir = make_test_data_dir();
  Defer cleanup_ahead_data_dir(
      [&] { std::filesystem::remove_all(ahead_data_dir); });
  initialize_persistent_data_dir(data_dir);
  std::filesystem::copy(data_dir, ahead_data_dir,
                        std::filesystem::copy_options::recursive);
  advance_persistent_data_dir(ahead_data_dir);
  // Carry the event service's whole durable directory, not just the outbox
  // inside it: a committed record stores its payload as the content-addressed
  // <event_root>.events file next to the outbox, and on a real node that file
  // is durable before the record that references it, so an outbox that is
  // ahead always brings its events files along.
  ASSERT_GT(std::filesystem::remove_all(data_dir / "events"), 0UL);
  std::filesystem::copy(ahead_data_dir / "events", data_dir / "events",
                        std::filesystem::copy_options::recursive);
  ASSERT_GT(std::filesystem::remove_all(data_dir / "indexer" / "indexer-db"),
            0UL);
  std::filesystem::copy(ahead_data_dir / "indexer" / "indexer-db",
                        data_dir / "indexer" / "indexer-db",
                        std::filesystem::copy_options::recursive);

  HogDevnodeProcess process = HogDevnodeProcess::start(
      {"--data-dir", data_dir.string(), "--dev-accounts", "1",
       "--dev-account-seed", "groundhog-dev-accounts", "--dev-account-tokens",
       "1000000000"});
  Readiness readiness = process.wait_for_readiness();
  ASSERT_EQ(readiness.chain_state, "recovered");
  auto reader = create_client<IndexerBlockExplorer1>(readiness.indexer.host,
                                                     readiness.indexer.port,
                                                     /*retry=*/true)()
                    .value();
  AddressTransactionQuery query{};
  query.order = IterationOrder::IterationOrderAscending;
  query.anchor.kind(BlockAnchorKind::AnchorAtBeginning);
  EXPECT_EQ(reader.get_transactions_by_address(query)().latest_block, 2);
  expect_clean_exit(process);
}

TEST(HogDevnode, DataDirPromotesFinalizedEventsFileAfterRestart)
{
  std::filesystem::path data_dir = make_test_data_dir();
  Defer cleanup_data_dir([&] { std::filesystem::remove_all(data_dir); });
  std::filesystem::path finalized_data_dir = make_test_data_dir();
  Defer cleanup_finalized_data_dir(
      [&] { std::filesystem::remove_all(finalized_data_dir); });
  initialize_persistent_data_dir(data_dir);
  std::filesystem::copy(data_dir, finalized_data_dir,
                        std::filesystem::copy_options::recursive);
  advance_persistent_data_dir(finalized_data_dir);

  // Model a stop after HogDB finalized height two but before event promoted
  // the already-durable events file it wrote at close, or the indexer observed
  // that height.
  ASSERT_GT(std::filesystem::remove_all(data_dir / "ramdb"), 0UL);
  std::filesystem::copy(finalized_data_dir / "ramdb", data_dir / "ramdb",
                        std::filesystem::copy_options::recursive);

  constexpr BlockID kClosedCandidate = 900;
  {
    HogDbDAO db = open_persistent_hog_db(data_dir);
    HogDbStatus<DbTip> tip = db.read_dbtip();
    ASSERT_TRUE(tip.has_value()) << tip.error();
    ASSERT_EQ(tip->value.proposal_header.block_height, 2);
    ASSERT_NE(tip->id, kClosedCandidate);
  }
  ConsensusValue finalized;
  {
    auto finalized_store = IndexerOutboxStore::open(
        (finalized_data_dir / "events" / "indexer-outbox").string());
    ASSERT_TRUE(finalized_store.has_value()) << finalized_store.error();
    auto committed = finalized_store->load_committed(2)();
    ASSERT_TRUE(committed.has_value()) << committed.error();
    finalized = committed->consensus_value;

    // Model what closing left behind: the block's events file, content
    // addressed by its event root, with the height never promoted to a
    // committed record. The finalized run retains it -- its committed record
    // references it as its payload -- so copy it from there.
    std::filesystem::copy_file(
        EventBlockContext::final_events_filename(
            (finalized_data_dir / "events").string(),
            finalized.proposal_header.events_hash),
        EventBlockContext::final_events_filename(
            (data_dir / "events").string(),
            finalized.proposal_header.events_hash),
        std::filesystem::copy_options::overwrite_existing);

    auto recovering_store = IndexerOutboxStore::open(
        (data_dir / "events" / "indexer-outbox").string());
    ASSERT_TRUE(recovering_store.has_value()) << recovering_store.error();
    EXPECT_FALSE(recovering_store->has_committed(2).value());
  }

  HogDevnodeProcess process = HogDevnodeProcess::start(
      {"--data-dir", data_dir.string(), "--dev-accounts", "1",
       "--dev-account-seed", "groundhog-dev-accounts", "--dev-account-tokens",
       "1000000000"});
  Readiness readiness = process.wait_for_readiness();
  ASSERT_EQ(readiness.chain_state, "recovered");
  auto reader = create_client<IndexerBlockExplorer1>(readiness.indexer.host,
                                                     readiness.indexer.port,
                                                     /*retry=*/true)()
                    .value();
  AddressTransactionQuery query{};
  query.order = IterationOrder::IterationOrderAscending;
  query.anchor.kind(BlockAnchorKind::AnchorAtBeginning);
  EXPECT_EQ(reader.get_transactions_by_address(query)().latest_block, 2);
  expect_clean_exit(process);

  auto recovered_store = IndexerOutboxStore::open(
      (data_dir / "events" / "indexer-outbox").string());
  ASSERT_TRUE(recovered_store.has_value()) << recovered_store.error();
  auto committed = recovered_store->load_committed(2)();
  ASSERT_TRUE(committed.has_value()) << committed.error();
  EXPECT_EQ(committed->consensus_value, finalized);
  // The promoted committed record references this file as its payload, so it
  // is retained for as long as the record lives.
  EXPECT_TRUE(std::filesystem::exists(EventBlockContext::final_events_filename(
      (data_dir / "events").string(), finalized.proposal_header.events_hash)));
}

TEST(HogDevnode, DataDirPullReplaysFinalizedOutboxOneAheadOfHogDb)
{
  std::filesystem::path data_dir = make_test_data_dir();
  Defer cleanup_data_dir([&] { std::filesystem::remove_all(data_dir); });
  initialize_persistent_data_dir(data_dir);
  std::ignore = append_next_test_outbox_record(data_dir);

  HogDevnodeProcess process = HogDevnodeProcess::start(
      {"--data-dir", data_dir.string(), "--dev-accounts", "1",
       "--dev-account-seed", "groundhog-dev-accounts", "--dev-account-tokens",
       "1000000000"});
  Readiness readiness = process.wait_for_readiness();
  ASSERT_EQ(readiness.chain_state, "recovered");

  auto reader = create_client<IndexerBlockExplorer1>(readiness.indexer.host,
                                                     readiness.indexer.port,
                                                     /*retry=*/true)()
                    .value();
  AddressTransactionQuery query{};
  query.order = IterationOrder::IterationOrderAscending;
  query.anchor.kind(BlockAnchorKind::AnchorAtBeginning);
  EXPECT_EQ(reader.get_transactions_by_address(query)().latest_block, 2);
  expect_clean_exit(process);
}

TEST(HogDevnode, DataDirRejectsIndexerAheadOfHogDb)
{
  std::filesystem::path data_dir = make_test_data_dir();
  Defer cleanup_data_dir([&] { std::filesystem::remove_all(data_dir); });
  std::filesystem::path ahead_data_dir = make_test_data_dir();
  Defer cleanup_ahead_data_dir(
      [&] { std::filesystem::remove_all(ahead_data_dir); });
  initialize_persistent_data_dir(data_dir);
  std::filesystem::copy(data_dir, ahead_data_dir,
                        std::filesystem::copy_options::recursive);
  advance_persistent_data_dir(ahead_data_dir);
  ASSERT_GT(std::filesystem::remove_all(data_dir / "indexer" / "indexer-db"),
            0UL);
  std::filesystem::copy(ahead_data_dir / "indexer" / "indexer-db",
                        data_dir / "indexer" / "indexer-db",
                        std::filesystem::copy_options::recursive);

  expect_startup_failure(
      {"--data-dir", data_dir.string(), "--dev-accounts", "1",
       "--dev-account-seed", "groundhog-dev-accounts", "--dev-account-tokens",
       "1000000000"},
      "recovered hog-devnode indexer is ahead of finalized event outbox: "
      "indexed height 2, finalized height 1");
}

TEST(HogDevnode, DataDirAcceptsMatchingOutboxIndexerAndHogDb)
{
  std::filesystem::path data_dir = make_test_data_dir();
  Defer cleanup_data_dir([&] { std::filesystem::remove_all(data_dir); });
  initialize_persistent_data_dir(data_dir);

  HogDevnodeProcess process = HogDevnodeProcess::start(
      {"--data-dir", data_dir.string(), "--dev-accounts", "1",
       "--dev-account-seed", "groundhog-dev-accounts", "--dev-account-tokens",
       "1000000000"});
  Readiness readiness = process.wait_for_readiness();
  EXPECT_EQ(readiness.chain_state, "recovered");
  expect_clean_exit(process);
}

TEST(HogDevnode, DataDirRejectsUnreadableDbTip)
{
  std::filesystem::path data_dir = make_test_data_dir();
  Defer cleanup_data_dir([&] { std::filesystem::remove_all(data_dir); });
  initialize_persistent_data_dir(data_dir);

  {
    HogDbDAO db = open_persistent_hog_db(data_dir);
    constexpr std::string_view kDbTipKey = "<DBTIP";
    constexpr uint8_t kWrongType = 1;
    std::array<std::byte, 1> value{};
    ASSERT_TRUE(
        db.get_ramdb()
            .put(std::as_bytes(std::span(kDbTipKey.data(), kDbTipKey.size())),
                 kWrongType, value)
            .has_value());
    ASSERT_TRUE(db.commit().has_value());
  }

  expect_startup_failure(
      {
          "--data-dir",
          data_dir.string(),
          "--dev-accounts",
          "1",
          "--dev-account-seed",
          "groundhog-dev-accounts",
          "--dev-account-tokens",
          "1000000000",
      },
      "failed to read recovered DbTip: Type Mismatch");
}

TEST(HogDevnode, DataDirRejectsMismatchedStorageHash)
{
  std::filesystem::path data_dir = make_test_data_dir();
  Defer cleanup_data_dir([&] { std::filesystem::remove_all(data_dir); });
  initialize_persistent_data_dir(data_dir);

  {
    HogDbDAO db = open_persistent_hog_db(data_dir);
    HogDbStatus<DbTip> tip = db.read_dbtip();
    ASSERT_TRUE(tip.has_value());
    tip->storage_long_hash[0] ^= 1;
    ASSERT_TRUE(db.write_dbtip(*tip).has_value());
    ASSERT_TRUE(db.commit().has_value());
  }

  expect_startup_failure(
      {
          "--data-dir",
          data_dir.string(),
          "--dev-accounts",
          "1",
          "--dev-account-seed",
          "groundhog-dev-accounts",
          "--dev-account-tokens",
          "1000000000",
      },
      "recovered database storage hash does not match DbTip");
}

TEST(HogDevnode, DataDirRejectsRestartWithDifferentDevAccountSeed)
{
  std::filesystem::path data_dir = make_test_data_dir();
  Defer cleanup_data_dir([&] { std::filesystem::remove_all(data_dir); });
  initialize_persistent_data_dir(data_dir);

  expect_startup_failure(
      {
          "--data-dir",
          data_dir.string(),
          "--dev-accounts",
          "1",
          "--dev-account-seed",
          "different-seed",
          "--dev-account-tokens",
          "1000000000",
      },
      "different --dev-account-seed");
}

TEST(HogDevnode, DataDirRejectsRestartWithOmittedDevAccounts)
{
  std::filesystem::path data_dir = make_test_data_dir();
  Defer cleanup_data_dir([&] { std::filesystem::remove_all(data_dir); });
  initialize_persistent_data_dir(data_dir);

  expect_startup_failure(
      {
          "--data-dir",
          data_dir.string(),
      },
      "different --dev-accounts");
}

TEST(HogDevnode, DataDirRejectsRestartWithLargerDevAccountCount)
{
  std::filesystem::path data_dir = make_test_data_dir();
  Defer cleanup_data_dir([&] { std::filesystem::remove_all(data_dir); });
  initialize_persistent_data_dir(data_dir);

  expect_startup_failure(
      {
          "--data-dir",
          data_dir.string(),
          "--dev-accounts",
          "2",
          "--dev-account-seed",
          "groundhog-dev-accounts",
          "--dev-account-tokens",
          "1000000000",
      },
      "different --dev-accounts");
}

TEST(HogDevnode, DataDirRejectsRestartWithDifferentDevAccountTokens)
{
  std::filesystem::path data_dir = make_test_data_dir();
  Defer cleanup_data_dir([&] { std::filesystem::remove_all(data_dir); });
  initialize_persistent_data_dir(data_dir);

  expect_startup_failure(
      {
          "--data-dir",
          data_dir.string(),
          "--dev-accounts",
          "1",
          "--dev-account-seed",
          "groundhog-dev-accounts",
          "--dev-account-tokens",
          "42",
      },
      "different --dev-account-tokens");
}

TEST(HogDevnode, DataDirRejectsConcurrentSecondProcess)
{
  std::filesystem::path data_dir = make_test_data_dir();
  Defer cleanup_data_dir([&] { std::filesystem::remove_all(data_dir); });

  HogDevnodeProcess first = HogDevnodeProcess::start({
      "--data-dir",
      data_dir.string(),
  });
  first.wait_for_readiness();

  expect_startup_failure(
      {
          "--data-dir",
          data_dir.string(),
      },
      "already locked by another process");

  expect_clean_exit(first);
}

TEST(HogDevnode, DataDirRejectsCommittedChainWhenMetadataMissing)
{
  std::filesystem::path data_dir = make_test_data_dir();
  Defer cleanup_data_dir([&] { std::filesystem::remove_all(data_dir); });
  initialize_persistent_data_dir(data_dir);
  ASSERT_TRUE(std::filesystem::remove(data_dir / "hog-devnode-data.json"));

  expect_startup_failure(
      {
          "--data-dir",
          data_dir.string(),
          "--dev-accounts",
          "1",
          "--dev-account-seed",
          "groundhog-dev-accounts",
          "--dev-account-tokens",
          "1000000000",
      },
      "committed chain state but no bootstrap metadata; reinitialize the data "
      "dir");
}

TEST(HogDevnode, DataDirResumesMetadataWithoutCommittedChain)
{
  std::filesystem::path data_dir = make_test_data_dir();
  Defer cleanup_data_dir([&] { std::filesystem::remove_all(data_dir); });
  write_test_data_dir_metadata(data_dir, {.user_account_count = 1,
                                          .seed = "groundhog-dev-accounts",
                                          .user_storage_tokens = 1'000'000'000,
                                          .dev_wallet_profile = std::string(
                                              kDefaultDevWalletProfile)});

  HogDevnodeProcess process = HogDevnodeProcess::start({
      "--data-dir",
      data_dir.string(),
      "--dev-accounts",
      "1",
      "--dev-account-seed",
      "groundhog-dev-accounts",
      "--dev-account-tokens",
      "1000000000",
  });
  Readiness readiness = process.wait_for_readiness();
  ASSERT_EQ(readiness.bootstrap_accounts.size(), 2UL);
  expect_clean_exit(process);
}

TEST(HogDevnode, DataDirRejectsBootstrapGenesisWithoutInitializationBlock)
{
  std::filesystem::path data_dir = make_test_data_dir();
  Defer cleanup_data_dir([&] { std::filesystem::remove_all(data_dir); });
  const std::vector<std::string> args{
      "--data-dir",           data_dir.string(),
      "--dev-accounts",       "1",
      "--dev-account-seed",   "groundhog-dev-accounts",
      "--dev-account-tokens", "1000000000",
  };

  {
    HogDevnodeProcess process = HogDevnodeProcess::start(
        args, {{"HOG_DEVNODE_TEST_THROW_AFTER_GENESIS_LOAD", "1"}});
    std::optional<int> status =
        process.wait_for_exit_for_test(startup_timeout());
    if (!status.has_value()) {
      ADD_FAILURE() << "hog-devnode did not exit";
      return;
    }
    const int exit_status = *status;
    std::string stderr_output = process.read_remaining_stderr();
    ASSERT_TRUE(WIFEXITED(exit_status)) << stderr_output;
    EXPECT_EQ(WEXITSTATUS(exit_status), EXIT_FAILURE) << stderr_output;
    EXPECT_TRUE(process.read_remaining_stdout().empty());
    EXPECT_NE(stderr_output.find("injected hog-devnode post-genesis failure"),
              std::string::npos)
        << stderr_output;
  }

  expect_startup_failure(
      args,
      "hog-devnode data dir has bootstrap genesis but no initialization block");
}

TEST(HogDevnode, DataDirRejectsCorruptMetadataWithoutCommittedChain)
{
  std::filesystem::path data_dir = make_test_data_dir();
  Defer cleanup_data_dir([&] { std::filesystem::remove_all(data_dir); });
  std::filesystem::create_directories(data_dir);
  {
    std::ofstream metadata(data_dir / "hog-devnode-data.json",
                           std::ios::binary);
    ASSERT_TRUE(metadata);
    metadata << "{}";
  }

  expect_startup_failure(
      {
          "--data-dir",
          data_dir.string(),
      },
      "hog-devnode data metadata missing root.schemaVersion");
}

TEST(HogDevnode, DataDirRejectsLegacyBootstrapMetadata)
{
  std::filesystem::path data_dir = make_test_data_dir();
  Defer cleanup_data_dir([&] { std::filesystem::remove_all(data_dir); });
  write_test_data_dir_metadata(data_dir, {}, /*schema_version=*/1);

  expect_startup_failure(
      {
          "--data-dir",
          data_dir.string(),
      },
      "schemaVersion 1 uses the legacy bootstrap layout; reinitialize the data "
      "dir");
}

TEST(HogDevnode, DataDirRejectsCorruptMetadataOnRecovery)
{
  std::filesystem::path data_dir = make_test_data_dir();
  Defer cleanup_data_dir([&] { std::filesystem::remove_all(data_dir); });
  initialize_persistent_data_dir(data_dir);
  {
    std::ofstream metadata(data_dir / "hog-devnode-data.json",
                           std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(metadata);
    metadata << "{";
  }

  expect_startup_failure(
      {
          "--data-dir",
          data_dir.string(),
          "--dev-accounts",
          "1",
          "--dev-account-seed",
          "groundhog-dev-accounts",
          "--dev-account-tokens",
          "1000000000",
      },
      "hog-devnode data metadata JSON parse failed");
}

TEST(HogDevnode, BootstrapAccountCanDeployCliShapedTransaction)
{
  HogDevnodeProcess process = HogDevnodeProcess::start({
      "--dev-accounts",
      "1",
      "--dev-account-seed",
      "groundhog-dev-accounts",
      "--dev-account-tokens",
      "1000000000",
  });
  Readiness readiness = process.wait_for_readiness();
  ASSERT_EQ(readiness.bootstrap_accounts.size(), 2UL);

  {
    sched_coord_client_t scheduler =
        create_client<SchedulerCoord1>(readiness.scheduler.host,
                                       readiness.scheduler.port,
                                       /*retry=*/true)()
            .value();
    auto reader = create_client<IndexerBlockExplorer1>(readiness.indexer.host,
                                                       readiness.indexer.port,
                                                       /*retry=*/true)()
                      .value();

    EXPECT_EQ(reader.indexer_block_explorer_intro(version::kHogdProtocol())(),
              IndexerBlockExplorerIntroOutcome::IntroSuccess);

    auto fibonacci_contract = load_rv32_contract("fibonacci.rv32a");
    ASSERT_TRUE(fibonacci_contract);

    auto keypair =
        derive_bootstrap_account_keypair("groundhog-dev-accounts", /*index=*/1);
    using wallet_meta = ContractTraitMeta<dev_wallet::DevWallet>;
    const ContractAddress wallet_address =
        build_local_bootstrap_account_artifact({.user_account_count = 1})
            .accounts[1]
            .address;
    EXPECT_EQ(hex_string(wallet_address),
              readiness.bootstrap_accounts[1].address);
    const SHA256Hash deploy_nonce = make_nonce(7);
    const SHA256Hash contract_hash = sha256(*fibonacci_contract);

    Transaction tx{
        .invocation =
            Invocation{
                .invoked_address = wallet_address,
                .entrypoint = wallet_meta::entrypoint<u8"deploy">(),
                .gas_limit = GasAmount{10'000'000},
                .calldata =
                    wallet_meta::make_calldata<u8"deploy">(deploy_nonce,
                                                           contract_hash),
            },
        .uniquifier = Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = 7},
        .contracts_to_deploy = {*fibonacci_contract},
        .auth_claim = source_address_auth_claim(),
    };

    Signature signature(CryptographicScheme::Ed25519);
    auto signed_bytes = ed25519_sign(
        keypair.second,
        transaction_signature_preimage(readiness_network_hash(readiness), tx));
    ASSERT_TRUE(signed_bytes.has_value());
    signature.ed25519() = signed_bytes.value_or(Ed25519Signature{});
    SignedTransaction signed_tx{
        .tx = std::move(tx),
        .signatures = {DecoratedSignature{
            .public_key_id = 0,
            .signature = signature,
        }},
    };

    const TransactionHash tx_hash = sha256(signed_tx.tx);
    TransactionHashBatch nominated_hashes =
        submit_and_commit(scheduler, SignedTransactionBatch{signed_tx});
    if (nominated_hashes.size() != 1UL) {
      int status = process.terminate();
      ADD_FAILURE() << "hog-devnode exited with status " << status
                    << "\nstderr:\n"
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
    EXPECT_EQ(result.info().events.tx_hash, tx_hash);
  }

  expect_clean_exit(process);
}

TEST(HogDevnode, BootstrapAccountPrefundsThenCreatesInitializedDevWallet)
{
  HogDevnodeProcess process = HogDevnodeProcess::start({
      "--dev-accounts",
      "1",
      "--dev-account-seed",
      "groundhog-dev-accounts",
      "--dev-account-tokens",
      "1000000000",
  });
  Readiness readiness = process.wait_for_readiness();
  ASSERT_EQ(readiness.bootstrap_accounts.size(), 2UL);

  {
    sched_coord_client_t scheduler =
        create_client<SchedulerCoord1>(readiness.scheduler.host,
                                       readiness.scheduler.port,
                                       /*retry=*/true)()
            .value();
    auto reader = create_client<IndexerBlockExplorer1>(readiness.indexer.host,
                                                       readiness.indexer.port,
                                                       /*retry=*/true)()
                      .value();

    EXPECT_EQ(reader.indexer_block_explorer_intro(version::kHogdProtocol())(),
              IndexerBlockExplorerIntroOutcome::IntroSuccess);
    auto wait_for_transaction_info = [&reader](const TransactionHash &tx_hash) {
      const auto deadline =
          std::chrono::steady_clock::now() + kIndexPollTimeout;
      TxInfoResult result(TxInfoOutcome::TxInfoNotFound);
      while (std::chrono::steady_clock::now() < deadline) {
        result = reader.get_transaction_info(tx_hash)();
        if (result.outcome() == TxInfoOutcome::TxInfoFound) {
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      return result;
    };

    auto wallet_contract = load_default_dev_wallet_contract();
    ASSERT_TRUE(wallet_contract);
    auto sponsor_keypair =
        derive_bootstrap_account_keypair("groundhog-dev-accounts", /*index=*/0);
    Ed25519PublicKey owner_public_key =
        derive_bootstrap_account_keypair("created-wallet-owner", /*index=*/0)
            .first;
    using wallet_meta = ContractTraitMeta<dev_wallet::DevWallet>;
    const ContractAddress sponsor_wallet_address =
        build_local_bootstrap_account_artifact({.user_account_count = 1})
            .accounts[0]
            .address;
    EXPECT_EQ(hex_string(sponsor_wallet_address),
              readiness.bootstrap_accounts[0].address);

    const SHA256Hash wallet_hash = sha256(*wallet_contract);
    const SHA256Hash create_wallet_nonce = make_nonce(31);
    const ContractAddress created_wallet_address =
        sha256(ContractDeploymentHashPreimage{.deployer_address =
                                                  sponsor_wallet_address,
                                              .nonce = create_wallet_nonce,
                                              .contract_hash = wallet_hash});

    using erc20_meta = ContractTraitMeta<itc_erc20::ERC20>;
    Calldata transfer_calldata =
        x23::serialize(kNativeTokenContractAddress,
                       erc20_meta::entrypoint<u8"transfer">(),
                       GasAmount{10'000'000},
                       erc20_meta::make_calldata<u8"transfer">(
                           created_wallet_address,
                           make_erc20_amount(uint128_t{200'000'000})));
    Transaction prefund_wallet_tx{
        .invocation =
            Invocation{.invoked_address = sponsor_wallet_address,
                       .entrypoint = wallet_meta::entrypoint<u8"do_action">(),
                       .gas_limit = GasAmount{10'000'000},
                       .calldata = wallet_meta::make_calldata<u8"do_action">(
                           transfer_calldata)},
        .uniquifier = Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = 30},
        .auth_claim = source_address_auth_claim(),
    };
    SignedTransaction signed_prefund_wallet_tx =
        sign_transaction(std::move(prefund_wallet_tx), sponsor_keypair.second,
                         readiness_network_hash(readiness));
    const TransactionHash prefund_wallet_tx_hash =
        sha256(signed_prefund_wallet_tx.tx);
    TransactionHashBatch prefund_hashes =
        submit_and_commit(scheduler,
                          SignedTransactionBatch{signed_prefund_wallet_tx});
    ASSERT_EQ(prefund_hashes.size(), 1UL);
    ASSERT_EQ(prefund_hashes[0], prefund_wallet_tx_hash);
    TxInfoResult prefund_result =
        wait_for_transaction_info(prefund_wallet_tx_hash);
    ASSERT_EQ(prefund_result.outcome(), TxInfoOutcome::TxInfoFound);
    EXPECT_EQ(prefund_result.info().events.exec_result,
              ExecResult::ExecResultOK);
    EXPECT_EQ(prefund_result.info().events.tx_hash, prefund_wallet_tx_hash);
    ASSERT_EQ(prefund_result.info().events.root_output.status_code(),
              InvocationFrameStatusCode::Return);

    Transaction create_wallet_tx{
        .invocation =
            Invocation{
                .invoked_address = sponsor_wallet_address,
                .entrypoint = wallet_meta::entrypoint<u8"create_wallet">(),
                .gas_limit = GasAmount{10'000'000},
                .calldata = wallet_meta::make_calldata<u8"create_wallet">(
                    create_wallet_nonce, wallet_hash, owner_public_key)},
        .uniquifier = Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = 31},
        .contracts_to_deploy = {*wallet_contract},
        .auth_claim = source_address_auth_claim(),
    };
    SignedTransaction signed_create_wallet_tx =
        sign_transaction(std::move(create_wallet_tx), sponsor_keypair.second,
                         readiness_network_hash(readiness));
    const TransactionHash create_wallet_tx_hash =
        sha256(signed_create_wallet_tx.tx);
    TransactionHashBatch create_hashes =
        submit_and_commit(scheduler,
                          SignedTransactionBatch{signed_create_wallet_tx});
    if (create_hashes.size() != 1UL) {
      int status = process.terminate();
      ADD_FAILURE() << "hog-devnode exited with status " << status
                    << "\nstderr:\n"
                    << process.read_remaining_stderr();
      return;
    }
    ASSERT_EQ(create_hashes[0], create_wallet_tx_hash);

    TxInfoResult result = wait_for_transaction_info(create_wallet_tx_hash);
    ASSERT_EQ(result.outcome(), TxInfoOutcome::TxInfoFound);
    EXPECT_EQ(result.info().events.exec_result, ExecResult::ExecResultOK);
    EXPECT_EQ(result.info().events.tx_hash, create_wallet_tx_hash);
    ASSERT_EQ(result.info().events.root_output.status_code(),
              InvocationFrameStatusCode::Return);
    ReturnBuffer expected_return_buffer(created_wallet_address.begin(),
                                        created_wallet_address.end());
    EXPECT_EQ(result.info().events.root_output.return_buffer(),
              expected_return_buffer);
  }

  expect_clean_exit(process);
}

// The tests below exercise what a devnode data dir has to survive between
// runs. The existing DataDir* cases above pin the rejection paths a bad or
// mismatched directory takes; these pin the other half of the contract --
// that a good directory comes back with its committed state intact, that it
// does so after a crash and not just a clean stop, and that the recovery
// preflight rejects metadata it cannot honor.

// The slice of committed chain state a client can observe per account: the
// free native token balance and the address auth data, both labeled with the
// height they were read at. A restart has to reproduce all of it exactly.
DbGatewayPayload
read_account_gateway_items(auto &gateway,
                           std::vector<ContractAddress> const &addresses)
{
  AddressAndKeyBatch batch;
  for (ContractAddress const &address : addresses) {
    AddressAndKey balance;
    balance.address = address;
    balance.key.key_type(StorageKeyType::FreeNativeTokenBalance);
    batch.push_back(balance);

    AddressAndKey auth_data;
    auth_data.address = address;
    auth_data.key.key_type(StorageKeyType::AuthData);
    batch.push_back(auth_data);
  }

  DbGatewayResult result = gateway.dbgateway_get_items(batch)();
  CHECK(result.error() == RPCError::kRPCNone);
  return result.payload();
}

// The indexer catches up asynchronously after a restart, so its recovered
// height is only ever observable by polling. Returns the last height the
// indexer reported, so a caller that never sees [expected] fails on the
// height it did see.
BlockHeight
poll_indexer_latest_block(auto &reader, BlockHeight expected)
{
  const auto deadline = std::chrono::steady_clock::now() + kIndexPollTimeout;
  for (;;) {
    AddressTransactionQuery query{};
    query.order = IterationOrder::IterationOrderAscending;
    query.anchor.kind(BlockAnchorKind::AnchorAtBeginning);
    const BlockHeight latest =
        reader.get_transactions_by_address(query)().latest_block;
    if (latest == expected || std::chrono::steady_clock::now() >= deadline) {
      return latest;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

BlockHeight
committed_tip_height(sched_coord_client_t &scheduler)
{
  SchedulerGetTipResult tip = scheduler.schedcoord_get_tip()();
  if (tip.outcome() != SchedulerGetTipOutcome::SchedulerGetTipOK) {
    ADD_FAILURE() << "schedcoord_get_tip did not return OK";
    return -1;
  }
  return tip.db_tip().value.proposal_header.block_height;
}

// A deploy from dev account 1's wallet, the shape the CLI submits. Committing
// one leaves durable state in every subsystem a restart has to reconcile: a
// contract in HogDB, a consumed uniquifier and a gas charge against the
// wallet, a record in the event outbox, and a row in the indexer.
//
// Signing is deterministic, so rebuilding this with the same nonce after a
// restart yields the identical transaction -- which is what makes it usable
// as a replay.
SignedTransaction
make_wallet_deploy_tx(Readiness const &readiness, std::uint64_t nonce)
{
  auto fibonacci_contract = load_rv32_contract("fibonacci.rv32a");
  CHECK(fibonacci_contract != nullptr);
  auto keypair =
      derive_bootstrap_account_keypair("groundhog-dev-accounts", /*index=*/1);
  using wallet_meta = ContractTraitMeta<dev_wallet::DevWallet>;
  const ContractAddress wallet_address =
      build_local_bootstrap_account_artifact({.user_account_count = 1})
          .accounts[1]
          .address;

  Transaction tx{
      .invocation =
          Invocation{
              .invoked_address = wallet_address,
              .entrypoint = wallet_meta::entrypoint<u8"deploy">(),
              .gas_limit = GasAmount{10'000'000},
              .calldata = wallet_meta::make_calldata<u8"deploy">(
                  make_nonce(nonce), sha256(*fibonacci_contract)),
          },
      .uniquifier = Uniquifier{.block_expiry_height =
                                   std::numeric_limits<BlockHeight>::max(),
                               .nonce = nonce},
      .contracts_to_deploy = {*fibonacci_contract},
      .auth_claim = source_address_auth_claim(),
  };
  return sign_transaction(std::move(tx), keypair.second,
                          readiness_network_hash(readiness));
}

TransactionHash
commit_wallet_deploy(sched_coord_client_t &scheduler,
                     Readiness const &readiness, std::uint64_t nonce)
{
  SignedTransaction signed_tx = make_wallet_deploy_tx(readiness, nonce);
  const TransactionHash tx_hash = sha256(signed_tx.tx);
  TransactionHashBatch committed =
      submit_and_commit(scheduler, SignedTransactionBatch{signed_tx});
  EXPECT_EQ(committed.size(), 1UL);
  return tx_hash;
}

// Submits [txs] and proposes a block carrying them, deliberately stopping
// short of committing it. Returns the hashes the proposal picked up. Nothing
// this does is durable: the uniquifiers the proposed transactions would
// consume are only consumed when the block commits.
TransactionHashBatch
submit_and_propose_without_commit(sched_coord_client_t &scheduler,
                                  SignedTransactionBatch const &txs)
{
  scheduler.schedulercoord_submit_unvetted(t_to_parallel_batch(txs)())();

  SchedulerGetTipResult tip = scheduler.schedcoord_get_tip()();
  if (tip.outcome() != SchedulerGetTipOutcome::SchedulerGetTipOK) {
    ADD_FAILURE() << "schedcoord_get_tip did not return OK";
    return {};
  }
  SchedulerProposalResult proposal =
      scheduler.schedulercoord_propose_block(tip.db_tip().id,
                                             ProposalContext{})();
  if (proposal.outcome() != SchedulerProposalOutcome::SchedulerProposalOK) {
    ADD_FAILURE() << "schedulercoord_propose_block did not return OK";
    return {};
  }
  SignedTransactionBatch proposed_txs;
  EXPECT_TRUE(
      parallel_batch_to_t(proposal.proposed_block().txs, proposed_txs)());
  TransactionHashBatch tx_hashes;
  tx_hashes.reserve(proposed_txs.size());
  for (const SignedTransaction &tx : proposed_txs) {
    tx_hashes.push_back(sha256(tx.tx));
  }
  return tx_hashes;
}

// How long to let the mempool vet a submitted transaction before proposing.
// Vetting is asynchronous and a recovered node re-establishes its auth
// subscriptions during startup, so a proposal issued right after a
// submission can miss the transaction entirely.
//
// A retry loop that keeps committing blocks until the transaction shows up
// is NOT a safe substitute: schedulercoord_propose_block waits on
// wait_for_auth_data(), whose CHECK_EQ(current + 1, expected) assumes the
// auth-data height never falls more than one block behind the committed
// tip. Committing blocks faster than the auth pipeline publishes them
// violates that and wedges the node. So wait once, then take exactly one
// block -- the same single proposal the surrounding tests make.
constexpr auto kVettingSettleDelay = std::chrono::seconds(5);

// Submits [tx], gives vetting a wide margin, and commits exactly one block,
// returning the hashes that block carried. Empty means the transaction was
// kept out of it on the merits, not merely that vetting had not caught up.
TransactionHashBatch
submit_settle_and_commit(sched_coord_client_t &scheduler,
                         SignedTransaction const &tx)
{
  scheduler.schedulercoord_submit_unvetted(
      t_to_parallel_batch(SignedTransactionBatch{tx})())();
  std::this_thread::sleep_for(kVettingSettleDelay);
  return propose_and_commit(scheduler);
}

std::vector<std::string>
persistent_data_dir_args(std::filesystem::path const &data_dir)
{
  return {
      "--data-dir",           data_dir.string(),
      "--dev-accounts",       "1",
      "--dev-account-seed",   "groundhog-dev-accounts",
      "--dev-account-tokens", "1000000000",
  };
}

std::vector<ContractAddress>
single_dev_account_addresses()
{
  BootstrapAccountArtifact artifact =
      build_local_bootstrap_account_artifact({.user_account_count = 1});
  return {artifact.accounts[0].address, artifact.accounts[1].address};
}

TEST(HogDevnode, DataDirServesIdenticalCommittedStateAfterCleanRestart)
{
  std::filesystem::path data_dir = make_test_data_dir();
  Defer cleanup_data_dir([&] { std::filesystem::remove_all(data_dir); });
  const std::vector<std::string> args = persistent_data_dir_args(data_dir);
  const std::vector<ContractAddress> addresses = single_dev_account_addresses();

  TransactionHash tx_hash{};
  DbGatewayPayload committed_items;
  {
    HogDevnodeProcess process = HogDevnodeProcess::start(args);
    Readiness readiness = process.wait_for_readiness();
    ASSERT_EQ(readiness.chain_state, "fresh");

    sched_coord_client_t scheduler =
        create_client<SchedulerCoord1>(readiness.scheduler.host,
                                       readiness.scheduler.port,
                                       /*retry=*/true)()
            .value();
    tx_hash = commit_wallet_deploy(scheduler, readiness, /*nonce=*/51);
    ASSERT_EQ(committed_tip_height(scheduler), 2);

    auto gateway = create_client<DbGateway1>(readiness.db_gateway.host,
                                             readiness.db_gateway.port,
                                             /*retry=*/true)()
                       .value();
    committed_items = read_account_gateway_items(gateway, addresses);
    ASSERT_EQ(committed_items.block_height, 2);
    ASSERT_EQ(committed_items.items.size(), 2 * addresses.size());
    for (DbGatewayItemResult const &item : committed_items.items) {
      ASSERT_EQ(item.error(), RPCError::kRPCNone);
    }

    expect_clean_exit(process);
  }

  {
    HogDevnodeProcess process = HogDevnodeProcess::start(args);
    Readiness readiness = process.wait_for_readiness();
    ASSERT_EQ(readiness.chain_state, "recovered");

    sched_coord_client_t scheduler =
        create_client<SchedulerCoord1>(readiness.scheduler.host,
                                       readiness.scheduler.port,
                                       /*retry=*/true)()
            .value();
    EXPECT_EQ(committed_tip_height(scheduler), 2);

    auto gateway = create_client<DbGateway1>(readiness.db_gateway.host,
                                             readiness.db_gateway.port,
                                             /*retry=*/true)()
                       .value();
    // Same items, same height: the restart is invisible to a gateway client.
    EXPECT_EQ(read_account_gateway_items(gateway, addresses), committed_items);

    auto reader = create_client<IndexerBlockExplorer1>(readiness.indexer.host,
                                                       readiness.indexer.port,
                                                       /*retry=*/true)()
                      .value();
    EXPECT_EQ(poll_indexer_latest_block(reader, 2), 2);
    TxInfoResult result = reader.get_transaction_info(tx_hash)();
    ASSERT_EQ(result.outcome(), TxInfoOutcome::TxInfoFound);
    EXPECT_EQ(result.info().height, 2);
    EXPECT_EQ(result.info().events.exec_result, ExecResult::ExecResultOK);

    expect_clean_exit(process);
  }
}

TEST(HogDevnode, DataDirRecoversCommittedStateAfterUngracefulKill)
{
  std::filesystem::path data_dir = make_test_data_dir();
  Defer cleanup_data_dir([&] { std::filesystem::remove_all(data_dir); });
  const std::vector<std::string> args = persistent_data_dir_args(data_dir);
  const std::vector<ContractAddress> addresses = single_dev_account_addresses();

  TransactionHash tx_hash{};
  DbGatewayPayload committed_items;
  {
    HogDevnodeProcess process = HogDevnodeProcess::start(args);
    Readiness readiness = process.wait_for_readiness();
    ASSERT_EQ(readiness.chain_state, "fresh");

    sched_coord_client_t scheduler =
        create_client<SchedulerCoord1>(readiness.scheduler.host,
                                       readiness.scheduler.port,
                                       /*retry=*/true)()
            .value();
    tx_hash = commit_wallet_deploy(scheduler, readiness, /*nonce=*/61);
    auto gateway = create_client<DbGateway1>(readiness.db_gateway.host,
                                             readiness.db_gateway.port,
                                             /*retry=*/true)()
                       .value();
    committed_items = read_account_gateway_items(gateway, addresses);
    // schedulercoord_commit_block only returns once both the HogDB snapshot
    // and the event outbox record are durable, so everything read here has to
    // survive the kill below.
    ASSERT_EQ(committed_tip_height(scheduler), 2);
    ASSERT_EQ(committed_items.block_height, 2);

    expect_killed_exit(process);
  }

  {
    // The killed node held an flock on the data dir; the kernel dropped it
    // when the process died, so a restart is not locked out by its corpse.
    HogDevnodeProcess process = HogDevnodeProcess::start(args);
    Readiness readiness = process.wait_for_readiness();
    ASSERT_EQ(readiness.chain_state, "recovered");

    sched_coord_client_t scheduler =
        create_client<SchedulerCoord1>(readiness.scheduler.host,
                                       readiness.scheduler.port,
                                       /*retry=*/true)()
            .value();
    EXPECT_EQ(committed_tip_height(scheduler), 2);

    auto gateway = create_client<DbGateway1>(readiness.db_gateway.host,
                                             readiness.db_gateway.port,
                                             /*retry=*/true)()
                       .value();
    EXPECT_EQ(read_account_gateway_items(gateway, addresses), committed_items);

    auto reader = create_client<IndexerBlockExplorer1>(readiness.indexer.host,
                                                       readiness.indexer.port,
                                                       /*retry=*/true)()
                      .value();
    EXPECT_EQ(poll_indexer_latest_block(reader, 2), 2);
    TxInfoResult result = reader.get_transaction_info(tx_hash)();
    ASSERT_EQ(result.outcome(), TxInfoOutcome::TxInfoFound);
    EXPECT_EQ(result.info().height, 2);

    // Recovering read-only is not enough: the chain has to keep advancing on
    // top of what the crashed run committed.
    EXPECT_TRUE(submit_and_commit(scheduler, {}).empty());
    EXPECT_EQ(committed_tip_height(scheduler), 3);
    EXPECT_EQ(poll_indexer_latest_block(reader, 3), 3);

    expect_clean_exit(process);
  }
}

TEST(HogDevnode, DataDirRecoversAcrossRepeatedUngracefulKills)
{
  std::filesystem::path data_dir = make_test_data_dir();
  Defer cleanup_data_dir([&] { std::filesystem::remove_all(data_dir); });
  const std::vector<std::string> args = persistent_data_dir_args(data_dir);

  // Genesis leaves the chain at height 1, and each crash cycle adds one block.
  constexpr int kCrashCycles = 3;
  for (int cycle = 0; cycle < kCrashCycles; ++cycle) {
    SCOPED_TRACE(std::format("crash cycle {}", cycle));
    HogDevnodeProcess process = HogDevnodeProcess::start(args);
    Readiness readiness = process.wait_for_readiness();
    ASSERT_EQ(readiness.chain_state, cycle == 0 ? "fresh" : "recovered");

    sched_coord_client_t scheduler =
        create_client<SchedulerCoord1>(readiness.scheduler.host,
                                       readiness.scheduler.port,
                                       /*retry=*/true)()
            .value();
    ASSERT_EQ(committed_tip_height(scheduler), 1 + cycle);
    EXPECT_TRUE(submit_and_commit(scheduler, {}).empty());
    ASSERT_EQ(committed_tip_height(scheduler), 2 + cycle);

    expect_killed_exit(process);
  }

  HogDevnodeProcess process = HogDevnodeProcess::start(args);
  Readiness readiness = process.wait_for_readiness();
  ASSERT_EQ(readiness.chain_state, "recovered");

  sched_coord_client_t scheduler =
      create_client<SchedulerCoord1>(readiness.scheduler.host,
                                     readiness.scheduler.port,
                                     /*retry=*/true)()
          .value();
  EXPECT_EQ(committed_tip_height(scheduler), 1 + kCrashCycles);

  auto reader = create_client<IndexerBlockExplorer1>(readiness.indexer.host,
                                                     readiness.indexer.port,
                                                     /*retry=*/true)()
                    .value();
  EXPECT_EQ(poll_indexer_latest_block(reader, 1 + kCrashCycles),
            1 + kCrashCycles);

  expect_clean_exit(process);
}

TEST(HogDevnode, DataDirRejectsOutboxTwoBlocksAheadOfHogDb)
{
  std::filesystem::path data_dir = make_test_data_dir();
  Defer cleanup_data_dir([&] { std::filesystem::remove_all(data_dir); });
  initialize_persistent_data_dir(data_dir);

  // One outbox record ahead of HogDB is the ordinary crash window between the
  // event commit and the HogDB commit, and DataDirPullReplaysFinalizedOutbox-
  // OneAheadOfHogDb pins that it replays. Two records ahead is not a window
  // any crash can open, so recovery must refuse it rather than replay it.
  std::ignore = append_next_test_outbox_record(data_dir);
  std::ignore = append_next_test_outbox_record(data_dir);

  expect_startup_failure(
      persistent_data_dir_args(data_dir),
      "recovered event outbox does not reconcile with HogDB tip");
}

TEST(HogDevnode, DataDirRejectsHogDbAheadWithoutFinalizedEventsFile)
{
  std::filesystem::path data_dir = make_test_data_dir();
  Defer cleanup_data_dir([&] { std::filesystem::remove_all(data_dir); });
  std::filesystem::path finalized_data_dir = make_test_data_dir();
  Defer cleanup_finalized_data_dir(
      [&] { std::filesystem::remove_all(finalized_data_dir); });
  initialize_persistent_data_dir(data_dir);
  std::filesystem::copy(data_dir, finalized_data_dir,
                        std::filesystem::copy_options::recursive);
  advance_persistent_data_dir(finalized_data_dir);

  // Same stop as DataDirPromotesFinalizedEventsFileAfterRestart -- HogDB
  // finalized height two, the outbox never promoted it -- except that this
  // run's events directory does not hold the block's events file. Promotion
  // is what recovers an unpromoted height, and it is content addressed by the
  // event root, so with the file gone there is nothing to promote from and the
  // outbox stays a block behind a HogDB tip it can never reach.
  ASSERT_GT(std::filesystem::remove_all(data_dir / "ramdb"), 0UL);
  std::filesystem::copy(finalized_data_dir / "ramdb", data_dir / "ramdb",
                        std::filesystem::copy_options::recursive);

  {
    auto finalized_store = IndexerOutboxStore::open(
        (finalized_data_dir / "events" / "indexer-outbox").string());
    ASSERT_TRUE(finalized_store.has_value()) << finalized_store.error();
    auto committed = finalized_store->load_committed(2)();
    ASSERT_TRUE(committed.has_value()) << committed.error();
    ASSERT_FALSE(
        std::filesystem::exists(EventBlockContext::final_events_filename(
            (data_dir / "events").string(),
            committed->consensus_value.proposal_header.events_hash)));

    auto recovering_store = IndexerOutboxStore::open(
        (data_dir / "events" / "indexer-outbox").string());
    ASSERT_TRUE(recovering_store.has_value()) << recovering_store.error();
    ASSERT_FALSE(recovering_store->has_committed(2).value());
  }

  expect_startup_failure(
      persistent_data_dir_args(data_dir),
      "recovered event outbox does not reconcile with HogDB tip");
}

// The recovery preflight parses hog-devnode-data.json before it opens or
// migrates any persisted state, so every rejection below has to come out of
// the process before it touches the chain.

void
write_raw_test_data_dir_metadata(std::filesystem::path const &data_dir,
                                 std::string_view contents)
{
  std::filesystem::create_directories(data_dir);
  std::ofstream metadata(data_dir / "hog-devnode-data.json",
                         std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(metadata);
  metadata << contents;
}

TEST(HogDevnode, DataDirRejectsMetadataRootThatIsNotAnObject)
{
  std::filesystem::path data_dir = make_test_data_dir();
  Defer cleanup_data_dir([&] { std::filesystem::remove_all(data_dir); });
  write_raw_test_data_dir_metadata(data_dir, "[]");

  expect_startup_failure({"--data-dir", data_dir.string()},
                         "hog-devnode data metadata root is not an object");
}

TEST(HogDevnode, DataDirRejectsMetadataFromANewerSchemaVersion)
{
  std::filesystem::path data_dir = make_test_data_dir();
  Defer cleanup_data_dir([&] { std::filesystem::remove_all(data_dir); });
  write_test_data_dir_metadata(data_dir, {}, /*schema_version=*/3);

  expect_startup_failure(
      {"--data-dir", data_dir.string()},
      "unsupported hog-devnode data metadata schemaVersion 3, expected 2");
}

TEST(HogDevnode, DataDirRejectsMetadataWithUnexpectedKind)
{
  std::filesystem::path data_dir = make_test_data_dir();
  Defer cleanup_data_dir([&] { std::filesystem::remove_all(data_dir); });
  write_raw_test_data_dir_metadata(
      data_dir, R"({"schemaVersion":2,"kind":"hog-hosted-data"})");

  expect_startup_failure({"--data-dir", data_dir.string()},
                         "unsupported hog-devnode data metadata kind "
                         "'hog-hosted-data', expected 'hog-devnode-data'");
}

TEST(HogDevnode, DataDirRejectsMetadataWithoutBootstrapConfig)
{
  std::filesystem::path data_dir = make_test_data_dir();
  Defer cleanup_data_dir([&] { std::filesystem::remove_all(data_dir); });
  write_raw_test_data_dir_metadata(
      data_dir, R"({"schemaVersion":2,"kind":"hog-devnode-data"})");

  expect_startup_failure(
      {"--data-dir", data_dir.string()},
      "hog-devnode data metadata missing root.bootstrapConfig");
}

TEST(HogDevnode, DataDirRejectsMetadataWithDevAccountsAboveMaximum)
{
  std::filesystem::path data_dir = make_test_data_dir();
  Defer cleanup_data_dir([&] { std::filesystem::remove_all(data_dir); });
  write_test_data_dir_metadata(data_dir, {.user_account_count =
                                              kMaxBootstrapAccounts + 1});

  expect_startup_failure(
      {"--data-dir", data_dir.string()},
      std::format("hog-devnode data metadata devAccounts {} exceeds maximum {}",
                  kMaxBootstrapAccounts + 1, kMaxBootstrapAccounts));
}

TEST(HogDevnode, DataDirRejectsMetadataWithForeignFaucetTokens)
{
  std::filesystem::path data_dir = make_test_data_dir();
  Defer cleanup_data_dir([&] { std::filesystem::remove_all(data_dir); });
  // The faucet allocation is fixed by the local bootstrap profile, so a data
  // dir claiming a different one was not written by this bootstrap.
  write_test_data_dir_metadata(data_dir, {}, /*schema_version=*/2,
                               /*faucet_tokens=*/1);

  expect_startup_failure(
      {"--data-dir", data_dir.string()},
      std::format("hog-devnode data metadata faucetTokens must be {}, got 1",
                  kLocalFaucetStorageTokens));
}

TEST(HogDevnode, DataDirRejectsMetadataWithEmptyDevAccountSeed)
{
  std::filesystem::path data_dir = make_test_data_dir();
  Defer cleanup_data_dir([&] { std::filesystem::remove_all(data_dir); });
  write_test_data_dir_metadata(data_dir, {.seed = ""});

  expect_startup_failure(
      {"--data-dir", data_dir.string()},
      "hog-devnode data metadata devAccountSeed must not be empty");
}

TEST(HogDevnode, DataDirRejectsMetadataWithUnknownDevWalletProfile)
{
  std::filesystem::path data_dir = make_test_data_dir();
  Defer cleanup_data_dir([&] { std::filesystem::remove_all(data_dir); });
  write_test_data_dir_metadata(data_dir, {.dev_wallet_profile =
                                              "hog-devnode-from-the-future"});

  expect_startup_failure(
      {"--data-dir", data_dir.string()},
      "unsupported dev wallet profile 'hog-devnode-from-the-future'");
}

// Replay protection is durable state, so a restart has to preserve exactly
// where the line falls: consuming a uniquifier is an effect of the block that
// carried the transaction, so it survives iff that block committed.

// The control for the two replay tests below: a transaction the chain has
// never seen has to commit normally on a recovered node. Without this, an
// empty block after a restart cannot be read as "the replay was rejected" --
// it could just as well mean the node stopped accepting transactions at all.
// No other test submits a real transaction after a restart; they all commit
// empty blocks.
TEST(HogDevnode, DataDirAcceptsNewTransactionAfterRestart)
{
  std::filesystem::path data_dir = make_test_data_dir();
  Defer cleanup_data_dir([&] { std::filesystem::remove_all(data_dir); });
  const std::vector<std::string> args = persistent_data_dir_args(data_dir);
  initialize_persistent_data_dir(data_dir);

  HogDevnodeProcess process = HogDevnodeProcess::start(args);
  Readiness readiness = process.wait_for_readiness();
  ASSERT_EQ(readiness.chain_state, "recovered");

  sched_coord_client_t scheduler =
      create_client<SchedulerCoord1>(readiness.scheduler.host,
                                     readiness.scheduler.port,
                                     /*retry=*/true)()
          .value();
  ASSERT_EQ(committed_tip_height(scheduler), 1);

  SignedTransaction deploy_tx = make_wallet_deploy_tx(readiness, /*nonce=*/91);
  const TransactionHash tx_hash = sha256(deploy_tx.tx);
  TransactionHashBatch committed =
      submit_settle_and_commit(scheduler, deploy_tx);
  ASSERT_EQ(committed.size(), 1UL)
      << "a recovered node accepted no new transactions at all";
  EXPECT_EQ(committed[0], tx_hash);
  EXPECT_EQ(committed_tip_height(scheduler), 2);

  auto reader = create_client<IndexerBlockExplorer1>(readiness.indexer.host,
                                                     readiness.indexer.port,
                                                     /*retry=*/true)()
                    .value();
  EXPECT_EQ(poll_indexer_latest_block(reader, 2), 2);
  TxInfoResult result = reader.get_transaction_info(tx_hash)();
  ASSERT_EQ(result.outcome(), TxInfoOutcome::TxInfoFound);
  EXPECT_EQ(result.info().height, 2);
  EXPECT_EQ(result.info().events.exec_result, ExecResult::ExecResultOK);

  expect_clean_exit(process);
}

TEST(HogDevnode, DataDirRejectsReplayOfCommittedTransactionAfterRestart)
{
  std::filesystem::path data_dir = make_test_data_dir();
  Defer cleanup_data_dir([&] { std::filesystem::remove_all(data_dir); });
  const std::vector<std::string> args = persistent_data_dir_args(data_dir);

  SignedTransaction deploy_tx;
  TransactionHash tx_hash{};
  {
    HogDevnodeProcess process = HogDevnodeProcess::start(args);
    Readiness readiness = process.wait_for_readiness();
    ASSERT_EQ(readiness.chain_state, "fresh");

    sched_coord_client_t scheduler =
        create_client<SchedulerCoord1>(readiness.scheduler.host,
                                       readiness.scheduler.port,
                                       /*retry=*/true)()
            .value();
    deploy_tx = make_wallet_deploy_tx(readiness, /*nonce=*/71);
    tx_hash = sha256(deploy_tx.tx);
    TransactionHashBatch committed =
        submit_and_commit(scheduler, SignedTransactionBatch{deploy_tx});
    ASSERT_EQ(committed.size(), 1UL);
    ASSERT_EQ(committed[0], tx_hash);
    ASSERT_EQ(committed_tip_height(scheduler), 2);

    expect_clean_exit(process);
  }

  {
    HogDevnodeProcess process = HogDevnodeProcess::start(args);
    Readiness readiness = process.wait_for_readiness();
    ASSERT_EQ(readiness.chain_state, "recovered");

    sched_coord_client_t scheduler =
        create_client<SchedulerCoord1>(readiness.scheduler.host,
                                       readiness.scheduler.port,
                                       /*retry=*/true)()
            .value();
    ASSERT_EQ(committed_tip_height(scheduler), 2);

    // Block two committed, so it consumed this transaction's uniquifier for
    // good. The mempool that first vetted it is gone, so only the durable
    // replay cache can keep it out -- and it stays out of a block proposed
    // well after vetting would have finished, so an unfinished vetting pass
    // is not what is keeping it out.
    EXPECT_TRUE(submit_settle_and_commit(scheduler, deploy_tx).empty());

    // A block really was committed; the transaction just was not in it.
    EXPECT_EQ(committed_tip_height(scheduler), 3);

    auto reader = create_client<IndexerBlockExplorer1>(readiness.indexer.host,
                                                       readiness.indexer.port,
                                                       /*retry=*/true)()
                      .value();
    // The height-two execution is still the only one on record. Waiting on
    // the record rather than on the indexer reaching the tip keeps this off
    // the back of the empty blocks the window above committed.
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
    EXPECT_EQ(result.info().height, 2);

    expect_clean_exit(process);
  }
}

TEST(HogDevnode, DataDirReplaysTransactionWhoseBlockNeverCommitted)
{
  std::filesystem::path data_dir = make_test_data_dir();
  Defer cleanup_data_dir([&] { std::filesystem::remove_all(data_dir); });
  const std::vector<std::string> args = persistent_data_dir_args(data_dir);

  SignedTransaction deploy_tx;
  TransactionHash tx_hash{};
  {
    HogDevnodeProcess process = HogDevnodeProcess::start(args);
    Readiness readiness = process.wait_for_readiness();
    ASSERT_EQ(readiness.chain_state, "fresh");

    sched_coord_client_t scheduler =
        create_client<SchedulerCoord1>(readiness.scheduler.host,
                                       readiness.scheduler.port,
                                       /*retry=*/true)()
            .value();
    deploy_tx = make_wallet_deploy_tx(readiness, /*nonce=*/81);
    tx_hash = sha256(deploy_tx.tx);

    // Propose a block carrying the transaction and crash before committing
    // it. The proposal executed the transaction, but only a commit makes its
    // consumed uniquifier durable, so nothing here may outlive the process.
    TransactionHashBatch proposed =
        submit_and_propose_without_commit(scheduler,
                                          SignedTransactionBatch{deploy_tx});
    ASSERT_EQ(proposed.size(), 1UL);
    ASSERT_EQ(proposed[0], tx_hash);
    ASSERT_EQ(committed_tip_height(scheduler), 1);

    expect_killed_exit(process);
  }

  {
    HogDevnodeProcess process = HogDevnodeProcess::start(args);
    Readiness readiness = process.wait_for_readiness();
    ASSERT_EQ(readiness.chain_state, "recovered");

    sched_coord_client_t scheduler =
        create_client<SchedulerCoord1>(readiness.scheduler.host,
                                       readiness.scheduler.port,
                                       /*retry=*/true)()
            .value();
    // The abandoned proposal left no trace: the chain is still at the height
    // it committed before the crash.
    ASSERT_EQ(committed_tip_height(scheduler), 1);

    // So the transaction is still live and commits, as itself, on top of the
    // recovered chain.
    TransactionHashBatch committed =
        submit_settle_and_commit(scheduler, deploy_tx);
    ASSERT_EQ(committed.size(), 1UL)
        << "transaction from the abandoned proposal never committed";
    EXPECT_EQ(committed[0], tx_hash);
    EXPECT_EQ(committed_tip_height(scheduler), 2);

    auto reader = create_client<IndexerBlockExplorer1>(readiness.indexer.host,
                                                       readiness.indexer.port,
                                                       /*retry=*/true)()
                      .value();
    EXPECT_EQ(poll_indexer_latest_block(reader, 2), 2);
    TxInfoResult result = reader.get_transaction_info(tx_hash)();
    ASSERT_EQ(result.outcome(), TxInfoOutcome::TxInfoFound);
    EXPECT_EQ(result.info().height, 2);
    EXPECT_EQ(result.info().events.exec_result, ExecResult::ExecResultOK);

    expect_clean_exit(process);
  }
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
