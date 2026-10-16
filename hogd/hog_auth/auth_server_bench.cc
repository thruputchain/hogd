// Auth server throughput benchmarks.
//
// Every benchmark verifies single-signature Ed25519 transactions against a
// mock DB, so the numbers cover the auth server alone: cache lookups, DB
// fetches on a miss, and signature checks. The sweep is set by the
// --auth_bench_* flags below; the --benchmark_* flags work as usual.
//
//   --auth_bench_batch_sizes=64,512,4096   transactions per verify call
//   --auth_bench_parallelism=1,2,0         max_parallelism of in-process
//                                          verify calls (0 = all cores, as
//                                          the AuthMempool1 handler uses)
//   --auth_bench_in_flight=1,4,16          concurrent AuthMempool1 calls on
//                                          one connection
//   --auth_bench_txs_per_account=1,4       transactions sharing an account in
//                                          a cold-cache batch (1..4)
//   --auth_bench_cache_buckets=65536       AuthDataCache buckets (16 entries
//                                          each) for the warm benchmarks
//
// scripts/ci/auth_bench.sh runs the CI sweep.

#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <print>
#include <ratio>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "address_auth.hh"
#include "auth_claim.hh"
#include "auth_rpc.hh"
#include "hogd/auth_claim/signature_preimage.h"
#include "hogd/base/chunked_vector.h"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/os.h"
#include "hogd/base/rpc_client.h"
#include "hogd/base/rpc_message_size.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_auth/auth_server.h"
#include "hogd/hog_auth/dbauth_client.h"
#include "hogd/hog_auth/mock_dbauth_rpc.h"
#include "hogd/init.h"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/sockio.h"
#include "x23/x23.h"

namespace hogd {

namespace {

constexpr size_t kAccounts = 16384;
// Distinct transactions (by nonce) signed for each account.
constexpr size_t kTxsPerAccount = 4;
constexpr size_t kCalldataBytes = 64;
// The mock DB and every auth server sit at this height.
constexpr BlockHeight kHeight = 1;
// Non-zero, so a preimage that dropped the network hash would fail to verify.
constexpr NetworkHash kNetworkHash{0x5a};
// Cold-cache servers are rebuilt every iteration; 65536 entries hold every
// account without allocating the production-sized cache each time.
constexpr int kColdCacheBuckets = 4096;
// Sequential calls each in-flight worker makes per benchmark iteration.
constexpr size_t kCallsPerWorker = 4;

struct Params {
  std::vector<int64_t> batch_sizes{64, 512, 4096};
  std::vector<int64_t> parallelism{1, 2, 0};
  std::vector<int64_t> in_flight{1, 4, 16};
  std::vector<int64_t> txs_per_account{1, 4};
  int64_t cache_buckets = 65536;
};

ContractAddress
account_address(size_t account)
{
  ContractAddress address{};
  static_assert(sizeof(account) <= sizeof(address));
  std::memcpy(address.data(), &account, sizeof(account));
  return address;
}

// One Ed25519 key per account, at public key id 0, deterministic so every run
// signs the same transactions.
struct Corpus {
  std::vector<AddressAuthData> auth_data;
  // txs[copy * kAccounts + account] is [account]'s [copy]th transaction.
  std::vector<SignedTransaction> txs;

  const SignedTransaction &tx(size_t account, size_t copy) const
  {
    return txs[(copy * kAccounts) + account];
  }
};

void
sign_accounts(Corpus &corpus, size_t begin, size_t end)
{
  for (size_t account = begin; account < end; ++account) {
    std::array<std::byte, 32> seed{};
    std::memcpy(seed.data(), &account, sizeof(account));
    auto [pk, sk] = ed25519_keygen_from_seed(seed);

    PublicKey key(CryptographicScheme::Ed25519);
    key.ed25519() = pk;
    AddressAuthData &auth_data = corpus.auth_data[account];
    auth_data.public_keys[0] = x23::Ptr<PublicKeyConfig>(
        new PublicKeyConfig{.key = key, .max_weight = 100});
    auth_data.required_weight = 1;

    for (size_t copy = 0; copy < kTxsPerAccount; ++copy) {
      Transaction tx{};
      tx.invocation.invoked_address = account_address(account);
      tx.invocation.calldata.resize(kCalldataBytes);
      tx.uniquifier.block_expiry_height = kHeight + 1'000;
      tx.uniquifier.nonce = copy;
      tx.auth_claim = AuthClaim(AuthClaimType::kSourceAddressAuthData);
      tx.auth_claim.source_address_claim().allowed_keys = 1;
      tx.auth_claim.source_address_claim().required_weight = 1;

      DecoratedSignature signature;
      signature.public_key_id = 0;
      signature.signature = Signature(CryptographicScheme::Ed25519);
      std::optional<Ed25519Signature> sig =
          ed25519_sign(sk, transaction_signature_preimage(kNetworkHash, tx));
      if (!sig) {
        panic("auth bench: signing failed");
      }
      signature.signature.ed25519() = *sig;

      SignedTransaction &stx = corpus.txs[(copy * kAccounts) + account];
      stx.tx = std::move(tx);
      stx.signatures.push_back(std::move(signature));
    }
  }
}

Corpus
make_corpus()
{
  Corpus corpus;
  corpus.auth_data.resize(kAccounts);
  corpus.txs.resize(kAccounts * kTxsPerAccount);
  const size_t threads = std::max<size_t>(1, available_parallelism());
  const size_t per_thread = (kAccounts + threads - 1) / threads;
  std::vector<std::jthread> workers;
  for (size_t begin = 0; begin < kAccounts; begin += per_thread) {
    workers.emplace_back(sign_accounts, std::ref(corpus), begin,
                         std::min(kAccounts, begin + per_thread));
  }
  return corpus;
}

// [batch_size] transactions spread over [batch_size / txs_per_account]
// accounts, each account's transactions adjacent.
SignedTransactionBatch
make_batch(const Corpus &corpus, size_t batch_size, size_t txs_per_account)
{
  CHECK_GE(txs_per_account, 1UL);
  CHECK_LE(txs_per_account, kTxsPerAccount);
  SignedTransactionBatch batch;
  batch.reserve(batch_size);
  for (size_t i = 0; i < batch_size; ++i) {
    const size_t account = (i / txs_per_account) % kAccounts;
    const size_t copy =
        ((i % txs_per_account) + (i / (txs_per_account * kAccounts))) %
        kTxsPerAccount;
    batch.push_back(corpus.tx(account, copy));
  }
  return batch;
}

bool
all_succeeded(const AuthErrorCodeBatch &results, size_t expected)
{
  return results.size() == expected &&
         std::ranges::all_of(results, [](AuthErrorCode code) {
           return code == AuthErrorCode::kAuthSuccess;
         });
}

using AuthMempoolClient = rpc_client_t<AuthMempool1>;

// The mock DB and an auth server with a warm cache, both serving over
// loopback, plus an AuthMempool1 client: the mempool's view of hog-auth.
class BenchEnv {
public:
  BenchEnv(const Corpus &corpus, int cache_buckets) : corpus_(corpus)
  {
    db_port_ = db_server_.start_server(0);
    MockDBAuthRpc &db = *db_server_.impl();
    for (size_t account = 0; account < kAccounts; ++account) {
      db.set(account_address(account), corpus_.auth_data[account]);
    }
    db.increment_height();
    CHECK_EQ(db.get_current_height(), kHeight);

    warm_server_ = make_server(cache_buckets);
    // Every account into the cache; the warm benchmarks then never reach
    // the DB.
    const SignedTransactionBatch all =
        make_batch(corpus_, kAccounts, /*txs_per_account=*/1);
    CHECK(all_succeeded(warm_server_->verify_transactions(
                            kHeight, std::span(all.data(), all.size()),
                            available_parallelism())(),
                        all.size()));

    mempool_rpc_->max_message_size_ = kBulkRpcMaxMessageSize;
    mempool_rpc_->impl()->set_auth_server(*warm_server_);
    const uint16_t mempool_port = mempool_rpc_.start_server(0);
    x23::HostPort host_port;
    CHECK(host_port.parse(std::format("localhost:{}", mempool_port)));
    auto client = make_rpc_client<AuthMempool1>(host_port, /*retry=*/false,
                                                kBulkRpcMaxMessageSize)();
    if (!client) {
      panic("auth bench: connecting to the auth server failed");
    }
    mempool_client_ = std::make_unique<AuthMempoolClient>(std::move(*client));
  }

  // A fresh auth server at [kHeight] with nothing cached.
  std::unique_ptr<AuthServer> make_server(int cache_buckets)
  {
    auto dbauth_client =
        make_dbauth_client(std::format("localhost:{}", db_port_))();
    if (!dbauth_client) {
      panic("auth bench: connecting to the mock DB failed");
    }
    auto server = std::make_unique<AuthServer>(std::move(*dbauth_client),
                                               kNetworkHash, cache_buckets);
    server->register_auth_data_updates(kHeight,
                                       ChunkedVector<KeyChangeBatch>{})();
    return server;
  }

  const Corpus &corpus() const { return corpus_; }
  AuthServer &warm_server() { return *warm_server_; }
  AuthMempoolClient &mempool_client() { return *mempool_client_; }

private:
  // Destroyed in reverse: the client closes its connection before the
  // servers it talks to wait out theirs, and the default CoroPoll's
  // destructor stalls exit on any connection still open.
  const Corpus &corpus_;
  MockDBAuthServer db_server_;
  uint16_t db_port_ = 0;
  std::unique_ptr<AuthServer> warm_server_;
  RpcServer<AuthMempoolRpc> mempool_rpc_;
  std::unique_ptr<AuthMempoolClient> mempool_client_;
};

BenchEnv *g_env = nullptr;

void
report_txs(benchmark::State &state, size_t txs_per_iteration)
{
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(txs_per_iteration));
  state.counters["txs_per_s"] =
      benchmark::Counter(static_cast<double>(state.iterations()) *
                             static_cast<double>(txs_per_iteration),
                         benchmark::Counter::kIsRate);
}

// AuthServer::verify_transactions called directly, every account cached:
// signature checks plus cache lookups, no RPC.
void
bm_verify_warm(benchmark::State &state, size_t batch_size, size_t parallelism)
{
  const SignedTransactionBatch batch =
      make_batch(g_env->corpus(), batch_size, /*txs_per_account=*/1);
  const std::span<const SignedTransaction> txs(batch.data(), batch.size());
  for (auto _ : state) {
    AuthErrorCodeBatch results =
        g_env->warm_server().verify_transactions(kHeight, txs, parallelism)();
    if (!all_succeeded(results, batch_size)) {
      state.SkipWithError("verification failed");
      return;
    }
    benchmark::DoNotOptimize(results);
  }
  report_txs(state, batch_size);
}

// As bm_verify_warm, but each iteration starts from an empty cache, so every
// distinct account costs one DB lookup before its signatures are checked.
void
bm_verify_cold(benchmark::State &state, size_t batch_size,
               size_t txs_per_account)
{
  const SignedTransactionBatch batch =
      make_batch(g_env->corpus(), batch_size, txs_per_account);
  const std::span<const SignedTransaction> txs(batch.data(), batch.size());
  for (auto _ : state) {
    state.PauseTiming();
    std::unique_ptr<AuthServer> server = g_env->make_server(kColdCacheBuckets);
    state.ResumeTiming();
    AuthErrorCodeBatch results =
        server->verify_transactions(kHeight, txs, available_parallelism())();
    state.PauseTiming();
    const bool ok = all_succeeded(results, batch_size);
    server.reset();
    state.ResumeTiming();
    if (!ok) {
      state.SkipWithError("verification failed");
      return;
    }
  }
  report_txs(state, batch_size);
  state.counters["accounts"] = static_cast<double>(
      std::min(kAccounts,
               (batch_size + txs_per_account - 1) / txs_per_account));
}

// Issues [calls] verify calls back to back, recording each call's latency.
x23::Async<void>
rpc_worker(AuthMempoolClient &client, const SignedTransactionBatch &batch,
           size_t calls, std::vector<double> &latencies_us,
           std::atomic<bool> &failed)
{
  for (size_t i = 0; i < calls; ++i) {
    // A reply resumes this coroutine on the connection's reader thread; hop
    // off it before serializing the next call.
    co_await default_coropoll().pool_.bg();
    const auto start = std::chrono::steady_clock::now();
    AuthErrorCodeBatch results =
        co_await client.authmempool_verify_transactions(kHeight, batch);
    latencies_us.push_back(std::chrono::duration<double, std::micro>(
                               std::chrono::steady_clock::now() - start)
                               .count());
    if (!all_succeeded(results, batch.size())) {
      failed.store(true, std::memory_order_relaxed);
    }
  }
}

x23::Async<void>
run_in_flight(AuthMempoolClient &client, const SignedTransactionBatch &batch,
              std::vector<std::vector<double>> &latencies_us,
              std::atomic<bool> &failed)
{
  std::vector<x23::Async<void>> workers;
  workers.reserve(latencies_us.size());
  for (std::vector<double> &worker_latencies : latencies_us) {
    workers.push_back(
        rpc_worker(client, batch, kCallsPerWorker, worker_latencies, failed));
  }
  for (x23::Async<void> &worker : workers) {
    co_await worker;
  }
}

double
percentile(std::vector<double> &values, double p)
{
  if (values.empty()) {
    return 0;
  }
  const auto rank =
      static_cast<size_t>(p * static_cast<double>(values.size() - 1));
  std::ranges::nth_element(values,
                           values.begin() + static_cast<ptrdiff_t>(rank));
  return values[rank];
}

// The AuthMempool1 verify call over loopback, as the mempool makes it, with
// [in_flight] calls outstanding on the one connection. The handler verifies
// with every core, against a warm cache.
void
bm_verify_rpc(benchmark::State &state, size_t batch_size, size_t in_flight)
{
  const SignedTransactionBatch batch =
      make_batch(g_env->corpus(), batch_size, /*txs_per_account=*/1);
  std::vector<std::vector<double>> latencies_us(in_flight);
  std::atomic<bool> failed{false};
  for (auto _ : state) {
    run_in_flight(g_env->mempool_client(), batch, latencies_us, failed)();
    if (failed.load(std::memory_order_relaxed)) {
      state.SkipWithError("verification failed");
      return;
    }
  }
  report_txs(state, batch_size * in_flight * kCallsPerWorker);

  std::vector<double> all;
  for (const std::vector<double> &worker_latencies : latencies_us) {
    all.insert(all.end(), worker_latencies.begin(), worker_latencies.end());
  }
  state.counters["call_p50_us"] = percentile(all, 0.50);
  state.counters["call_p99_us"] = percentile(all, 0.99);
}

std::optional<std::vector<int64_t>>
parse_list(std::string_view value)
{
  std::vector<int64_t> out;
  while (!value.empty()) {
    const size_t comma = value.find(',');
    const std::string_view item = value.substr(0, comma);
    int64_t parsed = 0;
    const auto [end, ec] =
        std::from_chars(item.data(), item.data() + item.size(), parsed);
    if (item.empty() || ec != std::errc{} || end != item.data() + item.size() ||
        parsed < 0) {
      return std::nullopt;
    }
    out.push_back(parsed);
    value = comma == std::string_view::npos ? std::string_view{}
                                            : value.substr(comma + 1);
  }
  if (out.empty()) {
    return std::nullopt;
  }
  return out;
}

// Removes the --auth_bench_* flags from argv, leaving the rest for
// benchmark::Initialize.
bool
parse_params(int &argc, char *argv[], Params &params)
{
  struct ListFlag {
    std::string_view name;
    std::vector<int64_t> *values;
  };
  const std::array<ListFlag, 4> list_flags{{
      {.name = "--auth_bench_batch_sizes=", .values = &params.batch_sizes},
      {.name = "--auth_bench_parallelism=", .values = &params.parallelism},
      {.name = "--auth_bench_in_flight=", .values = &params.in_flight},
      {.name = "--auth_bench_txs_per_account=",
       .values = &params.txs_per_account},
  }};
  constexpr std::string_view kBucketsFlag = "--auth_bench_cache_buckets=";

  int kept = 1;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (!arg.starts_with("--auth_bench_")) {
      argv[kept++] = argv[i];
      continue;
    }
    bool matched = false;
    for (const ListFlag &flag : list_flags) {
      if (arg.starts_with(flag.name)) {
        auto values = parse_list(arg.substr(flag.name.size()));
        if (!values) {
          std::println(stderr, "invalid list in {}", arg);
          return false;
        }
        *flag.values = std::move(*values);
        matched = true;
      }
    }
    if (arg.starts_with(kBucketsFlag)) {
      auto values = parse_list(arg.substr(kBucketsFlag.size()));
      if (!values || values->size() != 1 || values->front() == 0 ||
          values->front() > std::numeric_limits<int>::max()) {
        std::println(stderr, "invalid value in {}", arg);
        return false;
      }
      params.cache_buckets = values->front();
      matched = true;
    }
    if (!matched) {
      std::println(stderr, "unknown flag {}", arg);
      return false;
    }
  }
  argc = kept;
  argv[argc] = nullptr;

  const auto positive = [](int64_t value) { return value > 0; };
  if (!std::ranges::all_of(params.batch_sizes, positive)) {
    std::println(stderr, "--auth_bench_batch_sizes must be positive");
    return false;
  }
  if (!std::ranges::all_of(params.in_flight, positive)) {
    std::println(stderr, "--auth_bench_in_flight must be positive");
    return false;
  }
  if (!std::ranges::all_of(params.txs_per_account, [](int64_t value) {
        return value >= 1 && std::cmp_less_equal(value, kTxsPerAccount);
      })) {
    std::println(stderr, "--auth_bench_txs_per_account must be in [1, {}]",
                 kTxsPerAccount);
    return false;
  }
  return true;
}

void
register_benchmarks(const Params &params)
{
  for (int64_t batch : params.batch_sizes) {
    const auto batch_size = static_cast<size_t>(batch);
    for (int64_t requested : params.parallelism) {
      const size_t parallelism = requested == 0
                                     ? available_parallelism()
                                     : static_cast<size_t>(requested);
      benchmark::RegisterBenchmark(
          std::format("verify_warm/batch:{}/parallelism:{}", batch_size,
                      parallelism),
          bm_verify_warm, batch_size, parallelism)
          ->Unit(benchmark::kMillisecond)
          ->UseRealTime();
    }
  }
  for (int64_t batch : params.batch_sizes) {
    for (int64_t txs_per_account : params.txs_per_account) {
      benchmark::RegisterBenchmark(
          std::format("verify_cold/batch:{}/txs_per_account:{}", batch,
                      txs_per_account),
          bm_verify_cold, static_cast<size_t>(batch),
          static_cast<size_t>(txs_per_account))
          ->Unit(benchmark::kMillisecond)
          ->UseRealTime();
    }
  }
  for (int64_t batch : params.batch_sizes) {
    for (int64_t in_flight : params.in_flight) {
      benchmark::RegisterBenchmark(
          std::format("verify_rpc/batch:{}/in_flight:{}", batch, in_flight),
          bm_verify_rpc, static_cast<size_t>(batch),
          static_cast<size_t>(in_flight))
          ->Unit(benchmark::kMillisecond)
          ->UseRealTime();
    }
  }
}

} // namespace

} // namespace hogd

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  hogd::Params params;
  if (!hogd::parse_params(argc, argv, params)) {
    return EXIT_FAILURE;
  }
  int ret = hogd::hogd_init(argc, argv, /*do_print_version=*/false,
                            hogd::LogLevel::kError);
  if (ret != 0) {
    return EXIT_FAILURE;
  }
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
    return EXIT_FAILURE;
  }

  const hogd::Corpus corpus = hogd::make_corpus();
  auto env =
      std::make_unique<hogd::BenchEnv>(corpus,
                                       static_cast<int>(params.cache_buckets));
  hogd::g_env = env.get();
  hogd::register_benchmarks(params);
  benchmark::AddCustomContext("auth_bench_cores",
                              std::to_string(hogd::available_parallelism()));
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  hogd::g_env = nullptr;
  env.reset();
  return EXIT_SUCCESS;
}
