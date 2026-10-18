// Native singleton submission benchmark. Start hog-devnode from this build;
// the client prepares its own signed workload and drives periodic blocks.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <map>
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
#include "built_contracts/default_wallet.h"
#include "built_contracts/default_wallet.hh"
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
#include "hogd/crypto/domain_separators.h"
#include "hogd/devnode/block_driver.h"
#include "hogd/devnode/bootstrap_accounts.h"
#include "hogd/devnode/manifest_hex.h"
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
#include "x23/sockio.h"
#include "x23/threadpool.h"
#include "x23/vec.h"

namespace hogd {
namespace {
using Clock = std::chrono::steady_clock;

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
  // hog-devnode's own record limit: a proposed block, which the block driver
  // receives whole, holds up to a million transactions.
  auto client =
      make_rpc_client<Service>(address, cp, /*retry=*/false, 0x7fffffff)();
  require(client.has_value(), "backend connection failed");
  return std::move(*client);
}

struct Account {
  ContractAddress address;
  Ed25519PublicKey public_key;
  Ed25519SecretKey secret;
};

// Shared seed so [emit_manifest] and [accounts_from_manifest] derive the
// same keys.
constexpr std::string_view kBootstrapSeed = "schedgateway-bench-accounts";

// Write the public bootstrap intent hog-devnode needs for a run of
// [accounts] signing accounts, which [accounts_from_manifest] then re-derives.
void
emit_manifest(const char *path, std::uint32_t accounts)
{
  // [make_transfer] needs a distinct sender and recipient, so refuse to write
  // a manifest the run would only reject after bootstrapping the node.
  require(accounts >= 2, "a run needs at least two accounts");
  std::string output;
  json::Writer(output).object([&](json::Writer &w) {
    w.field("schemaVersion", std::int64_t{4});
    w.field("kind", "groundhog-bootstrap");
    w.field("networkHash", std::string(64, '0'));
    w.field("profile", "local-dev");
    w.object_field("bootstrapFaucet", [&](json::Writer &w) {
      w.field("publicKey",
              hex_string(
                  derive_bootstrap_account_keypair(kBootstrapSeed, 0).first));
      w.field("initialStorageTokens",
              std::to_string(kLocalFaucetStorageTokens));
    });
    w.array_field("bootstrapAccounts", [&](json::Writer &w) {
      for (std::uint32_t i = 1; i <= accounts; ++i) {
        w.object([&](json::Writer &w) {
          w.field("publicKey",
                  hex_string(derive_bootstrap_account_keypair(kBootstrapSeed, i)
                                 .first));
          w.field("initialStorageTokens",
                  std::to_string(kDefaultLocalUserStorageTokens));
        });
      }
    });
    w.object_field("storageCostModel", [&](json::Writer &w) {
      w.field("profile", "hogd-default");
    });
  });
  std::ofstream out(path, std::ios::out | std::ios::noreplace);
  require(out.good(), "cannot create bootstrap manifest");
  out << output << '\n';
  out.close();
  require(out.good(), "cannot write bootstrap manifest");
}

std::vector<Account>
accounts_from_manifest(const json::Value &manifest)
{
  // The runtime manifest carries only public allocations, so recover each
  // signer by re-deriving this run's keys and matching on the public key.
  const auto allocations = field(manifest, "bootstrapAccounts").as_array();
  const auto is_local_user = [](const json::Value &account) {
    return str(field(account, "role")) == "local_user";
  };
  const auto local_users = std::ranges::count_if(allocations, is_local_user);
  std::map<Ed25519PublicKey, Ed25519SecretKey> signers;
  for (std::uint32_t i = 1; std::cmp_less_equal(i, local_users); ++i) {
    auto [public_key, secret] =
        derive_bootstrap_account_keypair(kBootstrapSeed, i);
    signers.emplace(public_key, secret);
  }
  std::vector<Account> accounts;
  for (const auto &account : allocations) {
    if (!is_local_user(account))
      continue;
    auto public_key = hex_value<Ed25519PublicKey>(field(account, "publicKey"));
    auto signer = signers.find(public_key);
    require(signer != signers.end(), "bootstrap account is not ours to sign");
    accounts.push_back(
        {.address = hex_value<ContractAddress>(field(account, "walletAddress")),
         .public_key = public_key,
         .secret = signer->second});
  }
  require(accounts.size() >= 2,
          "devnode must be bootstrapped from an emitted manifest of >= 2 "
          "accounts");
  return accounts;
}

SignedTransaction
make_transfer(const std::vector<Account> &accounts, size_t nonce,
              const NetworkHash &network)
{
  using wallet = ContractTraitMeta<default_wallet::DefaultWallet>;
  using token = ContractTraitMeta<itc_erc20::ERC20>;
  const auto &sender = accounts[nonce % accounts.size()];
  const auto &recipient = accounts[(nonce + 1) % accounts.size()];
  x23::Vec<default_wallet::WalletCall, UINT32_MAX> calls{
      default_wallet::WalletCall{
          .target = kNativeTokenContractAddress,
          .entrypoint = token::entrypoint<u8"transfer">(),
          .gas_limit = 10'000'000,
          .calldata = token::make_calldata<u8"transfer">(
              recipient.address, std::tuple<int64_t, uint64_t>{0, 1}),
      }};
  // A bootstrap account is a standalone pubkey account with no on-chain auth
  // data, so it authorizes itself with the preimage of its own address.
  PublicKey sender_key(CryptographicScheme::Ed25519);
  sender_key.ed25519() = sender.public_key;
  AuthClaim claim(AuthClaimType::kPubkeySignature);
  claim.pubkey_preimage() = make_pubkey_account_preimage(sender_key, 0);
  Transaction tx{
      .invocation = {.invoked_address = sender.address,
                     .entrypoint = wallet::entrypoint<u8"execute">(),
                     .gas_limit = GasAmount{10'000'000},
                     .calldata = wallet::make_calldata<u8"execute">(calls)},
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
          .signatures = {DecoratedSignature{.public_key_id = 0,
                                            .signature = signature}},
          .pubkey_claim_key = sender_key};
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

// Per caller: latencies are per call; the counts are transactions.
struct WorkerResult {
  std::vector<int64_t> latency_us;
  int64_t measured = 0, warmup = 0, drain = 0;
  // Turned away for load, as a full mempool answers: counted, not retried.
  int64_t retryable = 0;
  // Completion of the last measured call, for a run that ends early.
  Clock::time_point last_measured{};
  std::string error;
};

struct Config {
  int connections;
  int duration;
  int warmup;
  int count;
  int block_ms;
  // Calls each connection keeps in flight, and transactions per call.
  int outstanding;
  int batch;
  int client_workers;
};

// The measured window, from the run's start.
struct Window {
  Clock::time_point measured_start, end;
};

// One of [outstanding] callers on a connection: submits the next batch of
// the corpus as soon as its previous call is answered, until the window
// closes or the corpus runs out, and counts down [remaining] when done.
x23::Detached
caller(rpc_client_t<SchedGateway1> &client, x23::ThreadPool &pool,
       const std::vector<ParallelSignedTransactionBatch> &corpus,
       const std::vector<size_t> &sizes, std::atomic<size_t> &next,
       const Window &window, std::stop_source &stop,
       std::atomic<bool> &exhausted, WorkerResult &r,
       std::atomic<size_t> &remaining, x23::CoroCB<void> done)
{
  co_await pool.bg();
  try {
    while (!stop.stop_requested()) {
      const auto started = Clock::now();
      if (started >= window.end)
        break;
      const auto n = next.fetch_add(1);
      if (n >= corpus.size()) {
        // The corpus bounds the run: what was measured so far still counts.
        exhausted = true;
        stop.request_stop();
        break;
      }
      auto outcomes = co_await client.schedgateway_submit_tx_batch(corpus[n]);
      const auto completed = Clock::now();
      require(outcomes.size() == sizes[n], "wrong outcome count");
      int64_t admitted = 0;
      for (auto outcome : outcomes) {
        if (outcome == SchedGatewaySubmitOutcome::SchedGatewaySubmitAdmitted)
          ++admitted;
        else if (outcome ==
                 SchedGatewaySubmitOutcome::SchedGatewaySubmitRetryable)
          ++r.retryable;
        else
          throw std::runtime_error("submission rejected");
      }
      // Only calls wholly inside the window contribute rate/latency.
      if (started < window.measured_start)
        r.warmup += admitted;
      else if (completed > window.end)
        r.drain += admitted;
      else {
        r.measured += admitted;
        r.last_measured = std::max(r.last_measured, completed);
        r.latency_us.push_back(
            std::chrono::duration_cast<std::chrono::microseconds>(completed -
                                                                  started)
                .count());
      }
    }
  } catch (const std::exception &e) {
    r.error = e.what();
    stop.request_stop();
  }
  if (remaining.fetch_sub(1) == 1)
    done();
}

struct RunResult {
  WorkerResult samples;
  std::chrono::microseconds measured;
  uint64_t blocks;
  uint64_t committed;
};

RunResult
run_load(const Config &config,
         std::vector<rpc_client_t<SchedGateway1>> &clients,
         const std::vector<ParallelSignedTransactionBatch> &corpus,
         const std::vector<size_t> &sizes, x23::ThreadPool &pool,
         rpc_client_t<SchedulerCoord1> &scheduler)
{
  std::atomic<size_t> next{1};
  std::stop_source stop;
  std::atomic<bool> exhausted{false};
  const size_t callers = static_cast<size_t>(config.connections) *
                         static_cast<size_t>(config.outstanding);
  std::vector<WorkerResult> results(callers);
  BlockDriver block_driver(scheduler);
  std::string driver_error;
  const auto epoch = Clock::now();
  const Window window{.measured_start =
                          epoch + std::chrono::seconds(config.warmup),
                      .end = epoch + std::chrono::seconds(config.warmup +
                                                          config.duration)};
  std::jthread driver([&](const std::stop_token &token) {
    std::stop_callback cancel(token, [&] { stop.request_stop(); });
    try {
      block_driver.run(epoch, std::chrono::milliseconds(config.block_ms),
                       stop.get_token());
    } catch (const std::exception &e) {
      driver_error = e.what();
      stop.request_stop();
    }
  });
  {
    // [outstanding] callers per connection keep that many calls in flight on
    // it; with one, each call's full cost is exposed.
    std::atomic<size_t> remaining{callers};
    auto [async, done] = x23::make_async<void>();
    for (size_t i = 0; i < callers; ++i)
      caller(clients[i % clients.size()], pool, corpus, sizes, next, window,
             stop, exhausted, results[i], remaining, done);
    async();
  }
  stop.request_stop();
  driver.join();
  WorkerResult total;
  total.error = driver_error;
  for (const auto &r : results) {
    total.latency_us.insert(total.latency_us.end(), r.latency_us.begin(),
                            r.latency_us.end());
    total.measured += r.measured;
    total.warmup += r.warmup;
    total.drain += r.drain;
    total.retryable += r.retryable;
    total.last_measured = std::max(total.last_measured, r.last_measured);
    if (!r.error.empty())
      total.error += r.error + "; ";
  }
  // A run the corpus cut short is measured up to its last measured call.
  auto measured = std::chrono::duration_cast<std::chrono::microseconds>(
      window.end - window.measured_start);
  if (exhausted) {
    if (total.measured == 0)
      total.error += "corpus exhausted before the measured window; ";
    else
      measured = std::chrono::duration_cast<std::chrono::microseconds>(
          total.last_measured - window.measured_start);
  }
  const auto admitted =
      static_cast<uint64_t>(total.measured + total.warmup + total.drain);
  // Stop load first, then reconcile block inclusion outside the timed phase.
  // Blocks commit at the executor's pace, so allow for a large backlog.
  const auto drain_deadline =
      Clock::now() + std::chrono::seconds(30) +
      std::chrono::microseconds(static_cast<int64_t>(admitted) * 50);
  while (total.error.empty() && block_driver.transactions() < admitted) {
    block_driver.produce_block();
    if (Clock::now() >= drain_deadline)
      total.error = "block drain timeout";
  }
  // A worker that stopped early left the load unfinished, so the counts are
  // expected to disagree; reporting that on top of its error only buries it.
  if (total.error.empty() && block_driver.transactions() != admitted)
    total.error += "committed/admitted count mismatch; ";
  std::ranges::sort(total.latency_us);
  return {.samples = std::move(total),
          .measured = measured,
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
    w.field("outstanding", config.connections * config.outstanding);
    w.field("outstandingPerConnection", config.outstanding);
    w.field("x23Workers", config.client_workers);
    w.field("helperWorkers", available_parallelism());
    w.field("batchSize", config.batch);
    w.field("accounts", account_count);
    w.field("preparedTransactions", config.count);
    w.field("seconds", config.duration);
    w.field("warmupSeconds", config.warmup);
    w.field("blockIntervalMs", config.block_ms);
    // Transactions admitted; calls are counted by the latency samples.
    w.field("measuredSuccess", total.measured);
    w.field("measuredCalls", measured);
    w.field("measuredMicros", static_cast<int64_t>(result.measured.count()));
    w.field("retryable", total.retryable);
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
  if (argc == 4 && std::string_view(argv[1]) == "--emit-bootstrap-manifest") {
    try {
      // Key derivation runs through the crypto subsystem, same as a run.
      if (hogd_init(argc, argv, false, LogLevel::kError))
        return 2;
      emit_manifest(argv[2], static_cast<std::uint32_t>(
                                 number(argv[3], static_cast<int>(
                                                     kMaxBootstrapAccounts))));
      return 0;
    } catch (const std::exception &e) {
      std::println(stderr, "{}", e.what());
      return 1;
    }
  }
  if (argc < 3 || argc > 11) {
    std::println(
        stderr,
        "Usage: {0} MANIFEST OUTPUT [CONNECTIONS=4 [SECONDS=5 [WARMUP=1 "
        "[TRANSACTIONS=60000 [BLOCK_MS=500 [OUTSTANDING=1 [BATCH=1 "
        "[CLIENT_WORKERS=8]]]]]]]]\n"
        "       {0} --emit-bootstrap-manifest OUTPUT ACCOUNTS\n"
        "Uses a fresh hog-devnode bootstrapped from the manifest the second "
        "form emits, and submits native-token transfers: OUTSTANDING calls "
        "in flight on each connection, BATCH transfers per call. A run that "
        "exhausts its TRANSACTIONS ends early and is measured up to then. "
        "Run under a process timeout.",
        argv[0]);
    return 2;
  }
  try {
    const Config config{
        .connections = argc > 3 ? number(argv[3], 64) : 4,
        .duration = argc > 4 ? number(argv[4], 60) : 5,
        .warmup = argc > 5 ? number(argv[5], 10) : 1,
        .count = argc > 6 ? number(argv[6], 20'000'000) : 60000,
        .block_ms = argc > 7 ? number(argv[7], 10000) : 500,
        .outstanding = argc > 8 ? number(argv[8], 1024) : 1,
        .batch = argc > 9 ? number(argv[9], 100000) : 1,
        .client_workers = argc > 10 ? number(argv[10], 1024) : 8,
    };
    require(!std::filesystem::exists(argv[2]), "output already exists");
    if (hogd_init(argc, argv, false, LogLevel::kError))
      return 2;
    auto manifest = read_manifest(argv[1]);
    require(field(manifest, "schemaVersion").as_integer() == 4 &&
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
    x23::ThreadPool pool{static_cast<size_t>(config.client_workers)};
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

    // Sign and pack before timing, on every core: a multi-million corpus
    // signed on one thread would take minutes. Entry 0 is the preflight tx,
    // nonce 0; entry k > 0 holds nonces (k - 1) * BATCH + 1 onward.
    const size_t calls =
        (static_cast<size_t>(config.count) + config.batch - 1) / config.batch;
    std::vector<ParallelSignedTransactionBatch> corpus(calls + 1);
    std::vector<size_t> sizes(calls + 1);
    const auto preflight = make_transfer(accounts, 0, *network);
    const TransactionHash preflight_hash = sha256(preflight.tx);
    corpus[0] = t_to_parallel_batch(SignedTransactionBatch{preflight})();
    sizes[0] = 1;
    {
      std::atomic<size_t> next_call{1};
      std::vector<std::jthread> signers;
      for (size_t t = 0; t < available_parallelism(); ++t)
        signers.emplace_back([&] {
          for (size_t k = next_call.fetch_add(1); k <= calls;
               k = next_call.fetch_add(1)) {
            const size_t first = (k - 1) * config.batch + 1;
            const size_t last =
                std::min<size_t>(first + config.batch - 1, config.count);
            SignedTransactionBatch txs;
            for (size_t n = first; n <= last; ++n)
              txs.push_back(make_transfer(accounts, n, *network));
            sizes[k] = txs.size();
            corpus[k] = t_to_parallel_batch(txs)();
          }
        });
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

    const auto result =
        run_load(config, clients, corpus, sizes, pool, scheduler);
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
