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
//   --auth_bench_saturated_batch_sizes=64,512
//                                          transactions per call in the
//                                          saturated benchmarks
//   --auth_bench_saturated_in_flight=1024  concurrent AuthMempool1 calls in
//                                          the saturated benchmarks
//   --auth_bench_saturated_connections=1   connections those calls share
//
// verify_warm and verify_saturated also report this process's CPU cost;
// verify_saturated runs its client in a child process to keep it out.
//
// scripts/ci/auth_bench.sh runs the CI sweep.

#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <print>
#include <ratio>
#ifdef __linux__
#include <sched.h>
#endif
#include <span>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <unistd.h>
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
#include "x23/progname.h"
#include "x23/sockio.h"
#include "x23/x23.h"

// For posix_spawn: the load client inherits this process's environment.
extern char **environ; // NOLINT(readability-redundant-declaration)

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

// Selects the load client mode (see LoadClient); its value is the port of the
// AuthMempool1 server to load and the number of connections to open, as
// "port,connections".
constexpr std::string_view kLoadClientFlag = "--auth_bench_load_client=";

struct Params {
  std::vector<int64_t> batch_sizes{64, 512, 4096};
  std::vector<int64_t> parallelism{1, 2, 0};
  std::vector<int64_t> in_flight{1, 4, 16};
  std::vector<int64_t> txs_per_account{1, 4};
  int64_t cache_buckets = 65536;
  // Far more calls than pool threads, so each thread always has a call
  // waiting however a call is split.
  std::vector<int64_t> saturated_batch_sizes{64, 512};
  std::vector<int64_t> saturated_in_flight{1024};
  int64_t saturated_connections = 1;
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
  // Join before returning: without copy elision the return moves [corpus]
  // out from under threads still writing to it.
  workers.clear();
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

std::optional<std::vector<int64_t>> parse_list(std::string_view value);

// CPU time this process has used, summed over all of its threads.
double
process_cpu_seconds()
{
  timespec now{};
  if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now) != 0) {
    panic("auth bench: clock_gettime: {}", std::strerror(errno));
  }
  return static_cast<double>(now.tv_sec) +
         (static_cast<double>(now.tv_nsec) * 1e-9);
}

// This process's CPU time and the wall time since construction.
class CpuStopwatch {
public:
  double cpu_seconds() const { return process_cpu_seconds() - cpu_start_; }
  double wall_seconds() const
  {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                         wall_start_)
        .count();
  }

private:
  double cpu_start_ = process_cpu_seconds();
  std::chrono::steady_clock::time_point wall_start_ =
      std::chrono::steady_clock::now();
};

// One line from [file], without its newline; nullopt at EOF.
std::optional<std::string>
read_line(FILE *file)
{
  std::string line;
  for (;;) {
    const int c = std::fgetc(file);
    if (c == '\n') {
      return line;
    }
    if (c == EOF) {
      return std::nullopt;
    }
    line.push_back(static_cast<char>(c));
  }
}

// An AuthMempool1 client in a child process: this binary, rerun with
// --auth_bench_load_client.
//
// The child reads one command per line on its stdin and answers each with
// one line on its stdout:
//   "run B,N,K"  N concurrent workers each make K calls of B transactions,
//                spread over the child's connections; answers "ok", or
//                "failed" if any transaction failed to verify
//   "latency"    answers "P50,P99", the call latency percentiles in
//                nanoseconds over every call since the last "latency"
// It answers "ready" once connected, and exits at EOF.
class LoadClient {
public:
  LoadClient(uint16_t port, size_t connections)
  {
    std::array<int, 2> to_child{};
    std::array<int, 2> from_child{};
    if (pipe(to_child.data()) != 0 || pipe(from_child.data()) != 0) {
      panic("auth bench: pipe: {}", std::strerror(errno));
    }
    // Only the dup2()ed copies below may reach the child.
    for (int fd : {to_child[0], to_child[1], from_child[0], from_child[1]}) {
      if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
        panic("auth bench: fcntl: {}", std::strerror(errno));
      }
    }

    const std::string &exe = x23::getprogpath();
    std::string exe_arg = exe;
    std::string flag =
        std::format("{}{},{}", kLoadClientFlag, port, connections);
    std::array<char *, 3> argv{exe_arg.data(), flag.data(), nullptr};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, to_child[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, from_child[1], STDOUT_FILENO);
    const int err = posix_spawn(&pid_, exe.c_str(), &actions,
                                /*attrp=*/nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(to_child[0]);
    close(from_child[1]);
    if (err != 0) {
      panic("auth bench: starting the load client {}: {}", exe,
            std::strerror(err));
    }

    to_child_ = fdopen(to_child[1], "w");
    from_child_ = fdopen(from_child[0], "r");
    if (to_child_ == nullptr || from_child_ == nullptr) {
      panic("auth bench: fdopen: {}", std::strerror(errno));
    }
    if (read_reply() != "ready") {
      panic("auth bench: the load client did not start");
    }
  }

  LoadClient(const LoadClient &) = delete;
  LoadClient &operator=(const LoadClient &) = delete;

  ~LoadClient()
  {
    // EOF on its stdin tells the child to close its connections and exit.
    std::fclose(to_child_);
    std::fclose(from_child_);
    int status = 0;
    while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
    }
  }

  // [in_flight] concurrent workers each make [calls_per_worker] calls of
  // [batch_size] transactions. Returns false if a transaction failed to
  // verify.
  bool run(size_t batch_size, size_t in_flight, size_t calls_per_worker)
  {
    return request(std::format("run {},{},{}", batch_size, in_flight,
                               calls_per_worker)) == "ok";
  }

  // The p50 and p99 call latency in microseconds over every call since the
  // last take_latency_us().
  std::pair<double, double> take_latency_us()
  {
    const std::string reply = request("latency");
    const auto values = parse_list(reply);
    if (!values || values->size() != 2) {
      panic("auth bench: bad latency reply from the load client: {}", reply);
    }
    return {static_cast<double>((*values)[0]) / 1000,
            static_cast<double>((*values)[1]) / 1000};
  }

private:
  pid_t pid_ = -1;
  FILE *to_child_ = nullptr;
  FILE *from_child_ = nullptr;

  std::string request(std::string_view command)
  {
    std::println(to_child_, "{}", command);
    if (std::fflush(to_child_) != 0) {
      panic("auth bench: the load client exited");
    }
    return read_reply();
  }

  std::string read_reply()
  {
    std::optional<std::string> line = read_line(from_child_);
    if (!line) {
      panic("auth bench: the load client exited");
    }
    return std::move(*line);
  }
};

// The mock DB and an auth server with a warm cache, both serving over
// loopback, plus an AuthMempool1 client: the mempool's view of hog-auth.
class BenchEnv {
public:
  BenchEnv(const Corpus &corpus, int cache_buckets, size_t load_connections)
    : corpus_(corpus), load_connections_(load_connections)
  {
    db_port_ = db_server_.start_server(0);
    MockDBAuthRpc &db = *db_server_.impl();
    for (size_t account = 0; account < kAccounts; ++account) {
      db.set(account_address(account), corpus_.auth_data[account]);
    }
    db.increment_height();
    CHECK_EQ(db.get_current_height(), kHeight);

    warm_server_ = make_server(cache_buckets);
    // Every account into the cache, then a second pass to prove they all
    // stayed: a cache too small for the corpus evicts, and the "warm"
    // benchmarks would quietly measure DB lookups.
    const SignedTransactionBatch all =
        make_batch(corpus_, kAccounts, /*txs_per_account=*/1);
    for (int pass = 0; pass < 2; ++pass) {
      const size_t lookups_before = db_lookups();
      CHECK(all_succeeded(warm_server_->verify_transactions(
                              kHeight, std::span(all.data(), all.size()),
                              available_parallelism())(),
                          all.size()));
      if (pass == 1 && db_lookups() != lookups_before) {
        panic("auth bench: {} cache buckets cannot hold all {} accounts "
              "({} DB lookups on a warm pass); raise "
              "--auth_bench_cache_buckets",
              cache_buckets, kAccounts, db_lookups() - lookups_before);
      }
    }

    mempool_rpc_->max_message_size_ = kBulkRpcMaxMessageSize;
    mempool_rpc_->impl()->set_auth_server(*warm_server_);
    mempool_port_ = mempool_rpc_.start_server(0);
    x23::HostPort host_port;
    CHECK(host_port.parse(std::format("localhost:{}", mempool_port_)));
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
  size_t db_lookups() { return db_server_.impl()->lookup_count(); }
  AuthServer &warm_server() { return *warm_server_; }
  AuthMempoolClient &mempool_client() { return *mempool_client_; }
  // Started on first use, so a run that filters out the saturated benchmarks
  // never signs a second corpus.
  LoadClient &load_client()
  {
    if (!load_client_) {
      load_client_ =
          std::make_unique<LoadClient>(mempool_port_, load_connections_);
    }
    return *load_client_;
  }

private:
  // Destroyed in reverse: the clients close their connections before the
  // servers they talk to wait out theirs, and the default CoroPoll's
  // destructor stalls exit on any connection still open.
  const Corpus &corpus_;
  MockDBAuthServer db_server_;
  uint16_t db_port_ = 0;
  std::unique_ptr<AuthServer> warm_server_;
  RpcServer<AuthMempoolRpc> mempool_rpc_;
  uint16_t mempool_port_ = 0;
  std::unique_ptr<AuthMempoolClient> mempool_client_;
  size_t load_connections_;
  std::unique_ptr<LoadClient> load_client_;
};

BenchEnv *g_env = nullptr;

// Reports throughput, and the DB lookups per iteration since [lookups_before]
// so a "warm" result that reached the DB shows it.
void
report_txs(benchmark::State &state, size_t txs_per_iteration,
           size_t lookups_before)
{
  if (state.iterations() > 0) {
    state.counters["db_lookups_per_iter"] =
        static_cast<double>(g_env->db_lookups() - lookups_before) /
        static_cast<double>(state.iterations());
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(txs_per_iteration));
  state.counters["txs_per_s"] =
      benchmark::Counter(static_cast<double>(state.iterations()) *
                             static_cast<double>(txs_per_iteration),
                         benchmark::Counter::kIsRate);
}

// Reports the CPU cost of the [txs] transactions verified since [stopwatch]
// started: this process's CPU time per transaction, transactions per
// CPU-second, and the CPUs busy on average.
void
report_cpu(benchmark::State &state, size_t txs, const CpuStopwatch &stopwatch)
{
  const double cpu_seconds = stopwatch.cpu_seconds();
  const double wall_seconds = stopwatch.wall_seconds();
  if (txs == 0 || cpu_seconds <= 0 || wall_seconds <= 0) {
    return;
  }
  const auto count = static_cast<double>(txs);
  state.counters["cpu_us_per_tx"] = cpu_seconds * 1e6 / count;
  state.counters["txs_per_cpu_s"] = count / cpu_seconds;
  state.counters["cpus_busy"] = cpu_seconds / wall_seconds;
}

// AuthServer::verify_transactions called directly, every account cached:
// signature checks plus cache lookups, no RPC.
void
bm_verify_warm(benchmark::State &state, size_t batch_size, size_t parallelism)
{
  const SignedTransactionBatch batch =
      make_batch(g_env->corpus(), batch_size, /*txs_per_account=*/1);
  const std::span<const SignedTransaction> txs(batch.data(), batch.size());
  const size_t lookups_before = g_env->db_lookups();
  const CpuStopwatch stopwatch;
  for (auto _ : state) {
    AuthErrorCodeBatch results =
        g_env->warm_server().verify_transactions(kHeight, txs, parallelism)();
    if (!all_succeeded(results, batch_size)) {
      state.SkipWithError("verification failed");
      return;
    }
    benchmark::DoNotOptimize(results);
  }
  report_cpu(state, batch_size * static_cast<size_t>(state.iterations()),
             stopwatch);
  report_txs(state, batch_size, lookups_before);
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
  const size_t lookups_before = g_env->db_lookups();
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
  report_txs(state, batch_size, lookups_before);
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

// One rpc_worker per element of [latencies_us], each making
// [calls_per_worker] calls, dealt round-robin over [clients].
x23::Async<void>
run_in_flight(std::span<AuthMempoolClient *const> clients,
              const SignedTransactionBatch &batch, size_t calls_per_worker,
              std::vector<std::vector<double>> &latencies_us,
              std::atomic<bool> &failed)
{
  std::vector<x23::Async<void>> workers;
  workers.reserve(latencies_us.size());
  for (size_t i = 0; i < latencies_us.size(); ++i) {
    workers.push_back(rpc_worker(*clients[i % clients.size()], batch,
                                 calls_per_worker, latencies_us[i], failed));
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
  const size_t lookups_before = g_env->db_lookups();
  AuthMempoolClient *const client = &g_env->mempool_client();
  for (auto _ : state) {
    run_in_flight(std::span(&client, 1), batch, kCallsPerWorker, latencies_us,
                  failed)();
    if (failed.load(std::memory_order_relaxed)) {
      state.SkipWithError("verification failed");
      return;
    }
  }
  report_txs(state, batch_size * in_flight * kCallsPerWorker, lookups_before);

  std::vector<double> all;
  for (const std::vector<double> &worker_latencies : latencies_us) {
    all.insert(all.end(), worker_latencies.begin(), worker_latencies.end());
  }
  state.counters["call_p50_us"] = percentile(all, 0.50);
  state.counters["call_p99_us"] = percentile(all, 0.99);
}

// Verify over RPC with [in_flight] calls outstanding from the load client,
// against a warm cache; CPU per transaction is the figure of interest.
void
bm_verify_saturated(benchmark::State &state, size_t batch_size,
                    size_t in_flight)
{
  LoadClient &client = g_env->load_client();
  // Forget the calls of earlier runs.
  client.take_latency_us();
  const size_t lookups_before = g_env->db_lookups();
  const CpuStopwatch stopwatch;
  for (auto _ : state) {
    if (!client.run(batch_size, in_flight, kCallsPerWorker)) {
      state.SkipWithError("verification failed");
      return;
    }
  }
  const size_t txs_per_iteration = batch_size * in_flight * kCallsPerWorker;
  report_cpu(state, txs_per_iteration * static_cast<size_t>(state.iterations()),
             stopwatch);
  report_txs(state, txs_per_iteration, lookups_before);
  const auto [p50_us, p99_us] = client.take_latency_us();
  state.counters["call_p50_us"] = p50_us;
  state.counters["call_p99_us"] = p99_us;
}

// The child side of LoadClient. [config] is "port,connections".
int
run_load_client(const char *argv0, std::string_view config)
{
  const auto values = parse_list(config);
  if (!values || values->size() != 2 || (*values)[0] == 0 ||
      (*values)[0] > std::numeric_limits<uint16_t>::max() ||
      (*values)[1] == 0) {
    std::println(stderr, "invalid value in {}{}", kLoadClientFlag, config);
    return EXIT_FAILURE;
  }
  const auto port = static_cast<uint16_t>((*values)[0]);
  const auto connections = static_cast<size_t>((*values)[1]);

  std::string argv0_copy = argv0;
  std::array<char *, 2> init_argv{argv0_copy.data(), nullptr};
  if (hogd_init(1, init_argv.data(), /*do_print_version=*/false,
                LogLevel::kError) != 0) {
    return EXIT_FAILURE;
  }

  const Corpus corpus = make_corpus();
  x23::HostPort host_port;
  CHECK(host_port.parse(std::format("localhost:{}", port)));
  std::vector<std::unique_ptr<AuthMempoolClient>> clients;
  std::vector<AuthMempoolClient *> client_ptrs;
  for (size_t i = 0; i < connections; ++i) {
    auto client = make_rpc_client<AuthMempool1>(host_port, /*retry=*/false,
                                                kBulkRpcMaxMessageSize)();
    if (!client) {
      std::println(stderr,
                   "auth bench load client: connecting to port {} "
                   "failed",
                   port);
      return EXIT_FAILURE;
    }
    clients.push_back(std::make_unique<AuthMempoolClient>(std::move(*client)));
    client_ptrs.push_back(clients.back().get());
  }
  std::println(stdout, "ready");
  std::fflush(stdout);

  std::map<size_t, SignedTransactionBatch> batches;
  std::vector<double> latencies_us;
  constexpr std::string_view kRun = "run ";
  while (const std::optional<std::string> line = read_line(stdin)) {
    std::string reply;
    if (*line == "latency") {
      reply = std::format(
          "{},{}", static_cast<int64_t>(percentile(latencies_us, 0.50) * 1000),
          static_cast<int64_t>(percentile(latencies_us, 0.99) * 1000));
      latencies_us.clear();
    }
    else if (line->starts_with(kRun)) {
      const auto args = parse_list(std::string_view(*line).substr(kRun.size()));
      if (!args || args->size() != 3 ||
          std::ranges::find(*args, 0) != args->end()) {
        std::println(stderr, "auth bench load client: bad command {}", *line);
        return EXIT_FAILURE;
      }
      const auto batch_size = static_cast<size_t>((*args)[0]);
      auto batch = batches.find(batch_size);
      if (batch == batches.end()) {
        batch = batches
                    .emplace(batch_size, make_batch(corpus, batch_size,
                                                    /*txs_per_account=*/1))
                    .first;
      }
      std::vector<std::vector<double>> worker_latencies(
          static_cast<size_t>((*args)[1]));
      std::atomic<bool> failed{false};
      run_in_flight(client_ptrs, batch->second, static_cast<size_t>((*args)[2]),
                    worker_latencies, failed)();
      for (const std::vector<double> &worker : worker_latencies) {
        latencies_us.insert(latencies_us.end(), worker.begin(), worker.end());
      }
      reply = failed.load(std::memory_order_relaxed) ? "failed" : "ok";
    }
    else {
      std::println(stderr, "auth bench load client: unknown command {}", *line);
      return EXIT_FAILURE;
    }
    std::println(stdout, "{}", reply);
    std::fflush(stdout);
  }
  return EXIT_SUCCESS;
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
  const std::array<ListFlag, 6> list_flags{{
      {.name = "--auth_bench_batch_sizes=", .values = &params.batch_sizes},
      {.name = "--auth_bench_parallelism=", .values = &params.parallelism},
      {.name = "--auth_bench_in_flight=", .values = &params.in_flight},
      {.name = "--auth_bench_txs_per_account=",
       .values = &params.txs_per_account},
      {.name = "--auth_bench_saturated_batch_sizes=",
       .values = &params.saturated_batch_sizes},
      {.name = "--auth_bench_saturated_in_flight=",
       .values = &params.saturated_in_flight},
  }};
  constexpr std::string_view kBucketsFlag = "--auth_bench_cache_buckets=";
  constexpr std::string_view kConnectionsFlag =
      "--auth_bench_saturated_connections=";

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
    if (arg.starts_with(kConnectionsFlag)) {
      auto values = parse_list(arg.substr(kConnectionsFlag.size()));
      if (!values || values->size() != 1 || values->front() == 0 ||
          values->front() > std::numeric_limits<int>::max()) {
        std::println(stderr, "invalid value in {}", arg);
        return false;
      }
      params.saturated_connections = values->front();
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
  if (!std::ranges::all_of(params.saturated_batch_sizes, positive)) {
    std::println(stderr, "--auth_bench_saturated_batch_sizes must be positive");
    return false;
  }
  if (!std::ranges::all_of(params.saturated_in_flight, positive)) {
    std::println(stderr, "--auth_bench_saturated_in_flight must be positive");
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
  for (int64_t batch : params.saturated_batch_sizes) {
    for (int64_t in_flight : params.saturated_in_flight) {
      benchmark::RegisterBenchmark(
          std::format("verify_saturated/batch:{}/in_flight:{}", batch,
                      in_flight),
          bm_verify_saturated, static_cast<size_t>(batch),
          static_cast<size_t>(in_flight))
          ->Unit(benchmark::kMillisecond)
          ->UseRealTime();
    }
  }
}

#ifdef __linux__
std::vector<pid_t>
auth_bench_threads()
{
  std::vector<pid_t> threads;
  for (const auto &task :
       std::filesystem::directory_iterator("/proc/self/task")) {
    threads.push_back(
        static_cast<pid_t>(std::stoi(task.path().filename().string())));
  }
  std::ranges::sort(threads);
  return threads;
}
#endif

bool
auth_bench_post_init_affinity(std::string_view requested, const Params &params)
{
#ifdef __linux__
  const auto matches_env = [](const char *key, std::string_view value) {
    const char *actual = std::getenv(key);
    return actual && actual == value;
  };
  if (!matches_env("GIT_BRANCH", "codex/auth-affinity-diagnostics") ||
      !matches_env("HOGD_CI_CONTAINER", "1") ||
      !matches_env("HOG_HOST", "hog9")) {
    std::println(stderr, "Auth affinity requires the isolated hog9 diagnostic");
    return false;
  }
  if (benchmark::GetBenchmarkFilter() !=
          "^verify_warm/batch:(64|512)/parallelism:(1|2)/real_time$" ||
      params.batch_sizes != std::vector<int64_t>{64, 512} ||
      params.parallelism != std::vector<int64_t>{1, 2}) {
    std::println(stderr,
                 "Auth affinity requires the four diagnostic warm cases");
    return false;
  }
  cpu_set_t inherited;
  if (sched_getaffinity(0, sizeof(inherited), &inherited) != 0) {
    std::println(stderr, "Auth affinity could not read inherited CPUs: {}",
                 strerror(errno));
    return false;
  }
  const auto threads = auth_bench_threads();
  const bool pin = requested != "inherit";
  std::vector<int> cpus;
  if (pin) {
    while (!requested.empty()) {
      const auto separator = requested.find(',');
      const auto part = requested.substr(0, separator);
      int cpu = -1;
      const auto [end, error] =
          std::from_chars(part.data(), part.data() + part.size(), cpu);
      if (error != std::errc{} || end != part.end() || cpu < 0 ||
          cpu >= CPU_SETSIZE || !CPU_ISSET(cpu, &inherited) ||
          (!cpus.empty() && cpu <= cpus.back())) {
        std::println(stderr,
                     "Auth affinity requires unique, ordered inherited CPUs");
        return false;
      }
      cpus.push_back(cpu);
      if (separator == std::string_view::npos) {
        break;
      }
      requested.remove_prefix(separator + 1);
      if (requested.empty()) {
        return false;
      }
    }
    if (cpus.empty() || threads.size() < cpus.size()) {
      std::println(stderr, "Auth affinity cannot cover every selected CPU");
      return false;
    }
  }
  // The benchmark process owns all these threads. Warm setup starts no child.
  // Snapshot all existing threads before altering even the main thread.
  for (auto tid : threads) {
    cpu_set_t mask;
    if (sched_getaffinity(tid, sizeof(mask), &mask) != 0 ||
        !CPU_EQUAL(&inherited, &mask)) {
      std::println(stderr,
                   "Auth affinity thread {} differs from inherited CPUs", tid);
      return false;
    }
  }
  // Order by TID within a process and cycle through the selected CPU IDs.
  // This uses the full selection but makes no assumptions about worker roles.
  for (size_t i = 0; i < threads.size(); ++i) {
    auto expected = inherited;
    int cpu = -1;
    if (pin) {
      cpu = cpus[i % cpus.size()];
      CPU_ZERO(&expected);
      CPU_SET(cpu, &expected);
      if (sched_setaffinity(threads[i], sizeof(expected), &expected) != 0) {
        std::println(stderr, "Auth affinity failed to pin {}: {}", threads[i],
                     strerror(errno));
        return false;
      }
    }
    cpu_set_t actual;
    if (sched_getaffinity(threads[i], sizeof(actual), &actual) != 0 ||
        !CPU_EQUAL(&expected, &actual)) {
      std::println(stderr, "Auth affinity readback failed for thread {}",
                   threads[i]);
      return false;
    }
    std::println(stderr, "auth_affinity_thread tid={} cpu={}", threads[i], cpu);
  }
  if (auth_bench_threads() != threads) {
    std::println(stderr,
                 "Auth affinity thread set changed during post-init placement");
    return false;
  }
  std::println(stderr,
               "auth_affinity_ready mode={} inherited_cpus={} threads={} "
               "cache_buckets={}",
               pin ? "pin" : "inherit", CPU_COUNT(&inherited), threads.size(),
               params.cache_buckets);
  return std::fflush(stderr) == 0;
#else
  std::println(stderr, "Auth affinity diagnostics require Linux");
  return false;
#endif
}

} // namespace

} // namespace hogd

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  if (argc == 2 &&
      std::string_view(argv[1]).starts_with(hogd::kLoadClientFlag)) {
    return hogd::run_load_client(argv[0], std::string_view(argv[1]).substr(
                                              hogd::kLoadClientFlag.size()));
  }

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
  auto env = std::make_unique<hogd::BenchEnv>(
      corpus, static_cast<int>(params.cache_buckets),
      static_cast<size_t>(params.saturated_connections));
  hogd::g_env = env.get();
  hogd::register_benchmarks(params);
  benchmark::AddCustomContext("auth_bench_cores",
                              std::to_string(hogd::available_parallelism()));
  if (const char *placement = std::getenv("HOGD_AUTH_BENCH_POST_INIT_CPUS");
      placement && !hogd::auth_bench_post_init_affinity(placement, params)) {
    return EXIT_FAILURE;
  }
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  hogd::g_env = nullptr;
  env.reset();
  return EXIT_SUCCESS;
}
