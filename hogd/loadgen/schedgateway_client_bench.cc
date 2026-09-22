// Native singleton submission benchmark. Start hog-devnode from this build;
// the client prepares its own signed workload and drives periodic blocks.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <latch>
#include <mutex>
#include <numeric>
#include <print>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "built_contracts/dev_wallet.h"
#include "built_contracts/itc_erc20.h"
#include "consensus.hh"
#include "exec_result.hh"
#include "hogd/auth_claim/signature_preimage.h"
#include "hogd/base/json.h"
#include "hogd/base/network_hash.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/string.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/devnode/bootstrap_accounts.h"
#include "hogd/genesis/genesis_constants.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/init.h"
#include "hogd/version.h"
#include "hogd/xdr/protocol_token.h"
#include "indexer.hh"
#include "proposal_hash.h"
#include "sched_coord.hh"
#include "sched_gateway.hh"
#include "x23/serialize.h"

namespace {
using namespace hogd;
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
  auto p = v.find(key);
  require(p != nullptr, "missing manifest field");
  return *p;
}

std::string
str(const json::Value &v)
{
  auto s = v.as_string();
  require(s.has_value(), "expected manifest string");
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
  require(parsed.has_value(), "invalid manifest JSON");
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
    require(index.has_value() && *index >= 1 && *index <= kMaxBootstrapAccounts,
            "invalid bootstrap account index");
    auto [public_key, secret] = derive_bootstrap_account_keypair(seed, *index);
    require(public_key ==
                hex_value<Ed25519PublicKey>(field(account, "publicKey")),
            "bootstrap public key mismatch");
    accounts.push_back(
        {hex_value<ContractAddress>(field(account, "walletAddress")), secret});
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
  require(bytes.has_value(), "transaction signing failed");
  Signature signature(CryptographicScheme::Ed25519);
  signature.ed25519() = *bytes;
  return {.tx = std::move(tx),
          .signatures = {
              DecoratedSignature{.public_key_id = 0, .signature = signature}}};
}

size_t
produce_block(rpc_client_t<SchedulerCoord1> &scheduler)
{
  auto tip = scheduler.schedcoord_get_tip()();
  require(tip.outcome() == SchedulerGetTipOutcome::SchedulerGetTipOK,
          "cannot read chain tip");
  auto proposal = scheduler.schedulercoord_propose_block(tip.db_tip().id,
                                                         ProposalContext{})();
  require(proposal.outcome() == SchedulerProposalOutcome::SchedulerProposalOK,
          "block proposal failed");
  const auto &block = proposal.proposed_block();
  SignedTransactionBatch txs;
  require(parallel_batch_to_t(block.txs, txs, kNoBatchSizeLimit)(),
          "cannot decode proposed transactions");
  ConsensusValue value{};
  value.prev_hash = sha256(tip.db_tip().value);
  value.proposal_header = block.proposal.header;
  value.proposal_txs_hash =
      proposal_txs_compatibility_hash(block.proposal.transactions);
  require(scheduler.schedulercoord_commit_block(block.block_id, value)() ==
              SchedulerCommitOutcome::SchedulerCommitOK,
          "block commit failed");
  return txs.size();
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
} // namespace

int
main(int argc, char **argv)
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
    const int connections = argc > 3 ? number(argv[3], 64) : 4;
    const int duration = argc > 4 ? number(argv[4], 60) : 5;
    const int warmup = argc > 5 ? number(argv[5], 10) : 1;
    const int count = argc > 6 ? number(argv[6], 1'000'000) : 60000;
    const int block_ms = argc > 7 ? number(argv[7], 10000) : 500;
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
    require(network.has_value(), "invalid network hash");
    auto accounts = accounts_from_manifest(manifest);
    x23::ThreadPool pool{kWorkers};
    x23::CoroPoll cp{pool};
    auto scheduler = connect<SchedulerCoord1>(manifest, "scheduler", cp);
    auto tip = scheduler.schedcoord_get_tip()();
    require(tip.outcome() == SchedulerGetTipOutcome::SchedulerGetTipOK &&
                tip.db_tip().value.proposal_header.block_height == 1,
            "requires a newly bootstrapped node with no produced blocks");
    std::vector<rpc_client_t<SchedGateway1>> clients;
    for (int i = 0; i < connections; ++i) {
      auto client = connect<SchedGateway1>(manifest, "schedulerGateway", cp);
      require(client.schedgateway_intro(protocol)() ==
                  SchedGatewayIntroOutcome::SchedGatewayIntroSuccess,
              "scheduler intro failed");
      clients.push_back(std::move(client));
    }
    require(clients[0].schedgateway_pending_tx_count()() == 0,
            "requires an empty mempool");

    // Sign and pack before timing. Nonce zero is reserved for the execution
    // preflight; measured callers consume the remaining corpus once, never
    // replay.
    std::vector<ParallelSignedTransactionBatch> corpus;
    corpus.reserve(count + 1);
    TransactionHash preflight_hash{};
    for (int n = 0; n <= count; ++n) {
      auto tx = make_transfer(accounts, n, *network);
      if (n == 0)
        preflight_hash = sha256(tx.tx);
      corpus.push_back(t_to_parallel_batch(SignedTransactionBatch{tx})());
    }
    submit(clients[0], corpus[0]);
    require(produce_block(scheduler) == 1,
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

    std::atomic<size_t> next{1};
    std::atomic<bool> stop{false};
    std::latch ready{connections}, start{1};
    Clock::time_point epoch;
    std::vector<WorkerResult> results(connections);
    std::vector<std::jthread> callers;
    std::jthread driver;
    uint64_t blocks = 0, committed = 0;
    std::string driver_error;
    try {
      // One synchronous caller per connection exposes the cost of each RPC.
      for (int i = 0; i < connections; ++i) {
        callers.emplace_back([&, i] {
          auto &r = results[i];
          ready.count_down();
          start.wait();
          const auto measured_start = epoch + std::chrono::seconds(warmup);
          const auto end = measured_start + std::chrono::seconds(duration);
          try {
            while (!stop.load()) {
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
            stop.store(true);
          }
        });
      }
      driver = std::jthread([&](std::stop_token token) {
        start.wait();
        std::mutex mutex;
        std::unique_lock lock(mutex);
        std::condition_variable_any wake;
        try {
          const auto interval = std::chrono::milliseconds(block_ms);
          auto tick = epoch + interval;
          while (!token.stop_requested() && !stop.load()) {
            wake.wait_until(lock, token, tick, [] { return false; });
            if (token.stop_requested() || stop.load())
              break;
            committed += produce_block(scheduler);
            ++blocks;
            // Keep the requested cadence; skip missed ticks without bursts.
            tick += interval;
            const auto now = Clock::now();
            if (tick <= now)
              tick += interval * ((now - tick) / interval + 1);
          }
        } catch (const std::exception &e) {
          driver_error = e.what();
          stop.store(true);
        }
      });
    } catch (...) {
      // Release already-created threads if another thread could not be started.
      stop.store(true);
      start.count_down();
      throw;
    }
    ready.wait();
    epoch = Clock::now();
    start.count_down();
    for (auto &caller : callers)
      caller.join();
    driver.request_stop();
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
    while (total.error.empty() && committed < admitted) {
      committed += produce_block(scheduler);
      ++blocks;
      if (Clock::now() >= drain_deadline)
        total.error = "block drain timeout";
    }
    if (committed != admitted)
      total.error += "committed/admitted count mismatch; ";
    std::sort(total.latency_us.begin(), total.latency_us.end());
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
      w.field("connections", connections);
      w.field("outstanding", connections);
      w.field("x23Workers", kWorkers);
      w.field("helperWorkers", available_parallelism());
      w.field("batchSize", 1);
      w.field("accounts", accounts.size());
      w.field("preparedTransactions", count);
      w.field("seconds", duration);
      w.field("warmupSeconds", warmup);
      w.field("blockIntervalMs", block_ms);
      w.field("measuredSuccess", measured);
      w.field("warmupSuccess", total.warmup);
      w.field("drainSuccess", total.drain);
      w.field("committedTransactions", committed);
      w.field("blocks", blocks);
      w.field("latencySumUs",
              std::accumulate(total.latency_us.begin(), total.latency_us.end(),
                              int64_t{0}));
      w.field("latencyP50Us", quantile(50));
      w.field("latencyP95Us", quantile(95));
      w.field("latencyP99Us", quantile(99));
      w.field("error", total.error);
    });
    std::ofstream out(argv[2], std::ios::out | std::ios::noreplace);
    require(out.good(), "cannot create output");
    out << output << '\n';
    out.close();
    require(out.good(), "cannot write output");
    std::println("{}", output);
    return !total.error.empty() || !measured ? 1 : 0;
  } catch (const std::exception &e) {
    std::println(stderr, "{}", e.what());
    return 2;
  }
}
