// Native singleton submission benchmark. Start hog-devnode from this build;
// the client prepares its own signed workload and drives periodic blocks.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <latch>
#include <numeric>
#include <optional>
#include <print>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "auth_claim.hh"
#include "built_contracts/dev_wallet.h"
#include "built_contracts/itc_erc20.h"
#include "exec_result.hh"
#include "hogd/auth_claim/signature_preimage.h"
#include "hogd/base/json.h"
#include "hogd/base/log.h"
#include "hogd/base/network_hash.h"
#include "hogd/base/os.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/string.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/devnode/block_driver.h"
#include "hogd/devnode/bootstrap_accounts.h"
#include "hogd/genesis/genesis_constants.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/init.h"
#include "hogd/version.h"
#include "indexer.hh"
#include "parallel.hh"
#include "sched_coord.hh"
#include "sched_gateway.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/event.h"
#include "x23/serialize.h"
#include "x23/sockio.h"
#include "x23/threadpool.h"

namespace hogd {
namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t kWorkers = 8;

void
require(bool condition, const char *message)
{
  if (!condition)
    throw std::runtime_error(message);
}

const json::Value &
field(const json::Value &v, std::string_view key)
{
  const auto *p = v.find(key);
  require(p != nullptr, "missing manifest field");
  return *p;
}

std::string
str(const json::Value &v)
{
  auto s = v.as_string();
  if (!s)
    throw std::runtime_error("expected manifest string");
  return std::string(*s);
}

json::Value
read_manifest(const char *path)
{
  require(std::filesystem::file_size(path) <= 2 * 1024 * 1024,
          "manifest exceeds 2 MiB");
  std::ifstream in(path);
  require(in.good(), "cannot open manifest");
  std::string bytes{std::istreambuf_iterator<char>(in), {}};
  auto parsed = json::parse(bytes);
  if (!parsed)
    throw std::runtime_error("invalid manifest JSON");
  return std::move(*parsed);
}

int
number(const char *s, int max)
{
  std::string value(s);
  size_t used = 0;
  int n = std::stoi(value, &used);
  require(used == value.size() && n >= 1 && n <= max,
          "invalid numeric argument");
  return n;
}

template<typename T>
T
hex_value(const json::Value &v)
{
  auto s = str(v);
  if (s.starts_with("0x"))
    s.erase(0, 2);
  T result{};
  require(s.size() == result.size() * 2 &&
              s.find_first_not_of("0123456789abcdefABCDEF") ==
                  std::string::npos,
          "invalid manifest hex field");
  auto bytes = decode_hex(s);
  std::transform(bytes.begin(), bytes.end(), result.begin(),
                 [](std::byte b) { return std::to_integer<uint8_t>(b); });
  return result;
}

template<typename Service>
rpc_client_t<Service>
connect(const json::Value &manifest, std::string_view endpoint,
        x23::CoroPoll &cp)
{
  x23::HostPort address;
  require(address.parse(str(field(field(manifest, "endpoints"), endpoint))),
          "invalid endpoint address");
  auto client = make_rpc_client<Service>(address, cp, /*retry=*/false,
                                         64 * 1024 * 1024)();
  require(client.has_value(), "backend connection failed");
  return std::move(*client);
}

struct Account {
  ContractAddress address;
  Ed25519SecretKey secret;
};

std::vector<Account>
accounts_from_manifest(const json::Value &manifest)
{
  const auto &derivation = field(manifest, "bootstrapAccountDerivation");
  require(str(field(derivation, "scheme")) == "groundhog-bootstrap-account-v1",
          "unsupported bootstrap account derivation");
  const auto seed = str(field(derivation, "seed"));
  std::vector<Account> accounts;
  for (const auto &account : field(manifest, "bootstrapAccounts").as_array()) {
    if (str(field(account, "role")) != "local_user")
      continue;
    const auto index = field(account, "index").as_integer();
    if (!index || *index < 1 ||
        !std::cmp_less_equal(*index, kMaxBootstrapAccounts))
      throw std::runtime_error("invalid bootstrap account index");
    auto [public_key, secret] = derive_bootstrap_account_keypair(seed, *index);
    require(public_key ==
                hex_value<Ed25519PublicKey>(field(account, "publicKey")),
            "bootstrap public key mismatch");
    accounts.push_back(
        {.address = hex_value<ContractAddress>(field(account, "walletAddress")),
         .secret = secret});
  }
  require(accounts.size() >= 2,
          "start a fresh devnode with --dev-accounts >= 2");
  return accounts;
}

SignedTransaction
make_transfer(const std::vector<Account> &accounts, size_t nonce,
              const NetworkHash &network)
{
  using wallet = ContractTraitMeta<dev_wallet::DevWallet>;
  using token = ContractTraitMeta<itc_erc20::ERC20>;
  const auto &sender = accounts[nonce % accounts.size()];
  const auto &recipient = accounts[(nonce + 1) % accounts.size()];
  Calldata action = x23::serialize(
      kNativeTokenContractAddress, token::entrypoint<u8"transfer">(),
      GasAmount{10'000'000},
      token::make_calldata<u8"transfer">(recipient.address,
                                         std::tuple<uint64_t, uint64_t>{0, 1}));
  AuthClaim claim(AuthClaimType::kSourceAddressAuthData);
  claim.source_address_claim().allowed_keys = 1;
  claim.source_address_claim().required_weight = 1;
  Transaction tx{
      .invocation = {.invoked_address = sender.address,
                     .entrypoint = wallet::entrypoint<u8"do_action">(),
                     .gas_limit = GasAmount{10'000'000},
                     .calldata = wallet::make_calldata<u8"do_action">(action)},
      .uniquifier = {.block_expiry_height = 10'000'000, .nonce = nonce},
      .auth_claim = claim,
  };
  auto bytes =
      ed25519_sign(sender.secret, transaction_signature_preimage(network, tx));
  if (!bytes)
    throw std::runtime_error("transaction signing failed");
  Signature signature(CryptographicScheme::Ed25519);
  signature.ed25519() = *bytes;
  return {.tx = std::move(tx),
          .signatures = {
              DecoratedSignature{.public_key_id = 0, .signature = signature}}};
}

void
submit(rpc_client_t<SchedGateway1> &client,
       const ParallelSignedTransactionBatch &batch)
{
  auto outcomes = client.schedgateway_submit_tx_batch(batch)();
  require(outcomes.size() == 1 &&
              outcomes[0] ==
                  SchedGatewaySubmitOutcome::SchedGatewaySubmitAdmitted,
          "submission not admitted");
}

struct WorkerResult {
  std::vector<int64_t> latency_us;
  int64_t warmup = 0, drain = 0;
  std::string error;
};

struct Config {
  int connections;
  int duration;
  int warmup;
  int count;
  int block_ms;
};

struct RunResult {
  WorkerResult samples;
  uint64_t blocks;
  uint64_t committed;
};

RunResult
run_load(const Config &config,
         std::vector<rpc_client_t<SchedGateway1>> &clients,
         const std::vector<ParallelSignedTransactionBatch> &corpus,
         rpc_client_t<SchedulerCoord1> &scheduler)
{
  std::atomic<size_t> next{1};
  std::stop_source stop;
  std::latch ready{config.connections}, start{1};
  Clock::time_point epoch;
  std::vector<WorkerResult> results(config.connections);
  BlockDriver block_driver(scheduler);
  std::string driver_error;
  std::vector<std::jthread> callers;
  std::jthread driver;
  try {
    // One synchronous caller per connection exposes the cost of each RPC.
    for (int i = 0; i < config.connections; ++i) {
      callers.emplace_back([&, i] {
        auto &r = results[i];
        ready.count_down();
        start.wait();
        const auto measured_start = epoch + std::chrono::seconds(config.warmup);
        const auto end = measured_start + std::chrono::seconds(config.duration);
        try {
          while (!stop.stop_requested()) {
            const auto started = Clock::now();
            if (started >= end)
              break;
            const auto n = next.fetch_add(1);
            require(n < corpus.size(),
                    "corpus exhausted; increase TRANSACTIONS");
            submit(clients[i], corpus[n]);
            const auto completed = Clock::now();
            // Only calls wholly inside the window contribute rate/latency.
            if (started < measured_start)
              ++r.warmup;
            else if (completed > end)
              ++r.drain;
            else
              r.latency_us.push_back(
                  std::chrono::duration_cast<std::chrono::microseconds>(
                      completed - started)
                      .count());
          }
        } catch (const std::exception &e) {
          r.error = e.what();
          stop.request_stop();
        }
      });
    }
    driver = std::jthread([&](const std::stop_token &token) {
      std::stop_callback cancel(token, [&] { stop.request_stop(); });
      start.wait();
      try {
        block_driver.run(epoch, std::chrono::milliseconds(config.block_ms),
                         stop.get_token());
      } catch (const std::exception &e) {
        driver_error = e.what();
        stop.request_stop();
      }
    });
  } catch (...) {
    // Release already-created threads if another thread could not be started.
    stop.request_stop();
    start.count_down();
    throw;
  }
  ready.wait();
  epoch = Clock::now();
  start.count_down();
  for (auto &caller : callers)
    caller.join();
  stop.request_stop();
  driver.join();
  WorkerResult total;
  total.error = driver_error;
  for (const auto &r : results) {
    total.latency_us.insert(total.latency_us.end(), r.latency_us.begin(),
                            r.latency_us.end());
    total.warmup += r.warmup;
    total.drain += r.drain;
    if (!r.error.empty())
      total.error += r.error + "; ";
  }
  const auto admitted = total.latency_us.size() + total.warmup + total.drain;
  // Stop load first, then reconcile block inclusion outside the timed phase.
  const auto drain_deadline = Clock::now() + std::chrono::seconds(15);
  while (total.error.empty() && block_driver.transactions() < admitted) {
    block_driver.produce_block();
    if (Clock::now() >= drain_deadline)
      total.error = "block drain timeout";
  }
  if (block_driver.transactions() != admitted)
    total.error += "committed/admitted count mismatch; ";
  std::ranges::sort(total.latency_us);
  return {.samples = std::move(total),
          .blocks = block_driver.blocks(),
          .committed = block_driver.transactions()};
}

void
write_result(const char *path, const Config &config,
             const json::Value &manifest, size_t account_count,
             const RunResult &result)
{
  const auto &total = result.samples;
  const auto measured = total.latency_us.size();
  auto quantile = [&](size_t percent) {
    return measured ? total.latency_us[(measured - 1) * percent / 100] : 0;
  };
  std::string output;
  json::Writer(output).object([&](json::Writer &w) {
    w.field("schema", 1);
    w.field("workload", "native-token-transfer-singleton");
    w.field("clientCommit", version::kHogdGitHash);
    w.field("backendCommit",
            str(field(field(manifest, "producer"), "commitHash")));
    w.field("protocol", str(field(manifest, "protocol")));
    w.field("connections", config.connections);
    w.field("outstanding", config.connections);
    w.field("x23Workers", kWorkers);
    w.field("helperWorkers", available_parallelism());
    w.field("batchSize", 1);
    w.field("accounts", account_count);
    w.field("preparedTransactions", config.count);
    w.field("seconds", config.duration);
    w.field("warmupSeconds", config.warmup);
    w.field("blockIntervalMs", config.block_ms);
    w.field("measuredSuccess", measured);
    w.field("warmupSuccess", total.warmup);
    w.field("drainSuccess", total.drain);
    w.field("committedTransactions", result.committed);
    w.field("blocks", result.blocks);
    w.field("latencySumUs",
            std::accumulate(total.latency_us.begin(), total.latency_us.end(),
                            int64_t{0}));
    w.field("latencyP50Us", quantile(50));
    w.field("latencyP95Us", quantile(95));
    w.field("latencyP99Us", quantile(99));
    w.field("error", total.error);
  });
  std::ofstream out(path, std::ios::out | std::ios::noreplace);
  require(out.good(), "cannot create output");
  out << output << '\n';
  out.close();
  require(out.good(), "cannot write output");
  std::println("{}", output);
}

int
run_benchmark(int argc, char **argv)
{
  if (argc < 3 || argc > 8) {
    std::println(
        stderr,
        "Usage: {} MANIFEST OUTPUT [CONNECTIONS=4 [SECONDS=5 [WARMUP=1 "
        "[TRANSACTIONS=60000 [BLOCK_MS=500]]]]]\n"
        "Uses a fresh hog-devnode and submits native-token transfers. "
        "Run under a process timeout.",
        argv[0]);
    return 2;
  }
  try {
    const Config config{
        .connections = argc > 3 ? number(argv[3], 64) : 4,
        .duration = argc > 4 ? number(argv[4], 60) : 5,
        .warmup = argc > 5 ? number(argv[5], 10) : 1,
        .count = argc > 6 ? number(argv[6], 1'000'000) : 60000,
        .block_ms = argc > 7 ? number(argv[7], 10000) : 500,
    };
    require(!std::filesystem::exists(argv[2]), "output already exists");
    if (hogd_init(argc, argv, false, LogLevel::kError))
      return 2;
    auto manifest = read_manifest(argv[1]);
    require(field(manifest, "schemaVersion").as_integer() == 2 &&
                str(field(manifest, "kind")) == "hogd-dev-runtime" &&
                str(field(manifest, "chainState")) == "fresh",
            "requires a fresh hog-devnode runtime manifest");
    auto protocol = version::kHogdProtocol();
    require(str(field(manifest, "protocol")) ==
                std::string(protocol.begin(), protocol.end()),
            "client/backend protocol mismatch");
    auto network = parse_network_hash(str(field(manifest, "networkHash")));
    if (!network)
      throw std::runtime_error("invalid network hash");
    auto accounts = accounts_from_manifest(manifest);
    x23::ThreadPool pool{kWorkers};
    x23::CoroPoll cp{pool};
    auto scheduler = connect<SchedulerCoord1>(manifest, "scheduler", cp);
    auto tip = scheduler.schedcoord_get_tip()();
    require(tip.outcome() == SchedulerGetTipOutcome::SchedulerGetTipOK &&
                tip.db_tip().value.proposal_header.block_height == 1,
            "requires a newly bootstrapped node with no produced blocks");
    std::vector<rpc_client_t<SchedGateway1>> clients;
    for (int i = 0; i < config.connections; ++i) {
      auto client = connect<SchedGateway1>(manifest, "schedulerGateway", cp);
      require(client.schedgateway_intro(protocol)() ==
                  SchedGatewayIntroOutcome::SchedGatewayIntroSuccess,
              "scheduler intro failed");
      clients.push_back(std::move(client));
    }
    require(clients[0].schedgateway_pending_tx_count()() == 0,
            "requires an empty mempool");

    // Sign and pack before timing; nonce 0 is the preflight tx.
    std::vector<ParallelSignedTransactionBatch> corpus;
    corpus.reserve(config.count + 1);
    TransactionHash preflight_hash{};
    for (int n = 0; n <= config.count; ++n) {
      auto tx = make_transfer(accounts, n, *network);
      if (n == 0)
        preflight_hash = sha256(tx.tx);
      corpus.push_back(t_to_parallel_batch(SignedTransactionBatch{tx})());
    }
    submit(clients[0], corpus[0]);
    require(BlockDriver(scheduler).produce_block() == 1,
            "preflight block must contain one tx");
    auto indexer = connect<IndexerBlockExplorer1>(manifest, "indexer", cp);
    require(indexer.indexer_block_explorer_intro(protocol)() ==
                IndexerBlockExplorerIntroOutcome::IntroSuccess,
            "indexer intro failed");
    const auto preflight_deadline = Clock::now() + std::chrono::seconds(10);
    for (;;) {
      auto receipt = indexer.get_transaction_info(preflight_hash)();
      if (receipt.outcome() == TxInfoOutcome::TxInfoFound) {
        require(receipt.info().events.exec_result == ExecResult::ExecResultOK,
                "preflight transfer did not execute successfully");
        break;
      }
      require(Clock::now() < preflight_deadline, "preflight receipt timeout");
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const auto result = run_load(config, clients, corpus, scheduler);
    write_result(argv[2], config, manifest, accounts.size(), result);
    return !result.samples.error.empty() || result.samples.latency_us.empty()
               ? 1
               : 0;
  } catch (const std::exception &e) {
    std::println(stderr, "{}", e.what());
    return 2;
  }
}

} // namespace
} // namespace hogd

int
main(int argc, char **argv) // NOLINT(bugprone-exception-escape)
{
  return hogd::run_benchmark(argc, argv);
}
