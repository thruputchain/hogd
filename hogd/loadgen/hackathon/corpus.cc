// Offline signed corpus generation: no compilation or signing in measured runs.
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "built_contracts/contract_loader.h"
#include "hackathon_app.h"
#include "hackathon_wallet.h"
#include "hogd/auth_claim/signature_preimage.h"
#include "hogd/base/base58.h"
#include "hogd/base/json.h"
#include "hogd/base/string.h"
#include "hogd/crypto/crypto.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/init.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "loadgen.hh"
#include "storage_item.hh"
#include "x23/serialize.h"

namespace hogd {
namespace {
using App = ContractTraitMeta<hackathon_app::HackathonApp>;
using Wallet = ContractTraitMeta<hackathon_wallet::HackathonWallet>;
struct User {
  std::array<Ed25519PublicKey, 2> keys;
  std::array<Ed25519SecretKey, 2> secrets;
  ContractAddress address;
  uint64_t sequence = 0;
  uint32_t weight = 1;
};
std::string
hex(auto const &bytes)
{
  return "0x" + encode_hex(std::as_bytes(std::span(bytes)));
}
void
write_bytes(const std::filesystem::path &path, auto const &value)
{
  auto bytes = x23::serialize(value);
  std::ofstream out(path, std::ios::binary);
  out.exceptions(std::ios::failbit | std::ios::badbit);
  out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}
struct Corpus {
  NetworkHash network{};
  std::vector<User> users;
  std::vector<std::shared_ptr<Contract>> apps;
  std::vector<ContractAddress> addresses;
  uint64_t seed;
  SignedTransaction transaction(size_t user, ContractEntrypointID method,
                                Calldata data, uint64_t gas = 20'000'000)
  {
    auto &u = users.at(user);
    auto sequence = u.sequence++;
    AuthClaim claim(AuthClaimType::kSourceAddressAuthData);
    claim.source_address_claim().allowed_keys = u.weight == 1 ? 1 : 3;
    claim.source_address_claim().required_weight = u.weight;
    return SignedTransaction{
        .tx = Transaction{.invocation = {.invoked_address = u.address,
                                         .entrypoint = method,
                                         .gas_limit = gas,
                                         .calldata = std::move(data)},
                          .uniquifier = {.block_expiry_height = BlockHeight(
                                             1'000'000 + sequence / 128),
                                         .nonce = sequence},
                          .auth_claim = claim}};
  }
  void sign(SignedTransaction &tx, size_t user)
  {
    const auto &u = users.at(user);
    for (uint32_t key = 0; key < u.weight; ++key) {
      Signature sig(CryptographicScheme::Ed25519);
      sig.ed25519() =
          ed25519_sign(u.secrets[key],
                       transaction_signature_preimage(network, tx.tx))
              .value();
      tx.signatures.push_back({.public_key_id = key, .signature = sig});
    }
  }
  SignedTransaction deploy(size_t app)
  {
    size_t user = app % users.size();
    const auto nonce = sha256(std::tuple(seed, uint64_t(app)));
    auto tx =
        transaction(user, Wallet::entrypoint<u8"deploy">(),
                    Wallet::make_calldata<u8"deploy">(
                        nonce, sha256(*apps.at(app)),
                        App::method_id<u8"initialize">(),
                        std::tuple<int64_t, uint64_t>(0, 100'000'000'000ULL)),
                    100'000'000);
    tx.tx.contracts_to_deploy.push_back(*apps.at(app));
    sign(tx, user);
    return tx;
  }
};
void
generate(int argc, char **argv)
{
  std::map<std::string, std::string> args;
  for (int i = 1; i < argc; i += 2) {
    if (i + 1 >= argc)
      throw std::runtime_error("arguments require values");
    args[argv[i]] = argv[i + 1];
  }
  for (const auto &[name, value] : args) {
    if (name != "--out" && name != "--artifacts" && name != "--apps" &&
        name != "--users" && name != "--wallets" && name != "--seed" &&
        name != "--stage-seconds" && name != "--warmup-seconds" &&
        name != "--shared-code" && name != "--rate-divisor" &&
        name != "--rate-multiplier")
      throw std::runtime_error("unknown argument " + name);
  }
  const auto number = [&](const char *key, uint64_t fallback) {
    if (!args.contains(key))
      return fallback;
    size_t end = 0;
    auto n = std::stoull(args.at(key), &end);
    if (end != args.at(key).size() || args.at(key).starts_with('-'))
      throw std::runtime_error("invalid number");
    return uint64_t(n);
  };
  const size_t count = number("--apps", 500), nusers = number("--users", 100);
  const size_t nwallets = number("--wallets", 5);
  const uint64_t stage = number("--stage-seconds", 60),
                 warmup = number("--warmup-seconds", 30);
  const uint64_t divisor = number("--rate-divisor", 1);
  const uint64_t multiplier = number("--rate-multiplier", 1);
  const bool shared = number("--shared-code", 0) != 0;
  if (count < 5 || nusers < 2 || nusers > 10000 || count > 10000 ||
      nwallets < 1 || nwallets > 5 || stage < 2 || stage > 3600 ||
      warmup > 3600 || divisor < 1 || divisor > 100 || multiplier < 1 ||
      multiplier > 100)
    throw std::runtime_error("invalid corpus size or duration");
  std::filesystem::path out = args.at("--out"),
                        artifacts = args.at("--artifacts");
  if (std::filesystem::exists(out))
    throw std::runtime_error("output directory already exists");
  std::filesystem::create_directories(out);
  set_contract_artifact_directories({artifacts});
  Corpus c{.seed = number("--seed", 1)};
  c.network.back() = 1;
  // Keep each wallet variant in its own ceremony batches: the protocol caps
  // attached contracts per transaction below our five default wallet variants.
  std::array<GenesisConfigurationBuilder, 5> builders;
  for (size_t i = 0; i < nusers; ++i) {
    const size_t variant = shared ? 0 : i % nwallets;
    auto contract =
        load_rv32_contract("wallet_" + std::to_string(variant) + ".rv32a");
    User u;
    for (size_t k = 0; k < 2; ++k) {
      auto entropy = sha256(std::tuple(c.seed, uint64_t(i), uint64_t(k)));
      auto pair = ed25519_keygen_from_seed(std::as_bytes(std::span(entropy)));
      u.keys[k] = pair.first;
      u.secrets[k] = pair.second;
    }
    u.weight = i % 2 + 1;
    auto nonce = sha256(std::tuple(c.seed, uint64_t(i)));
    u.address = calculate_genesis_deployment_address(sha256(*contract), nonce);
    const int128_t allocation =
        (int128_t{kTotalHogSupply} * kHogDivisibility) / (nusers + 2);
    builders[variant].add_distribution(
        contract, nonce, Wallet::entrypoint<u8"initialize">(),
        Wallet::make_calldata<u8"initialize">(u.keys[0], u.keys[1], u.weight),
        allocation);
    c.users.push_back(u);
  }
  for (size_t i = 0; i < count; ++i) {
    c.apps.push_back(load_rv32_contract(
        "app_" + std::to_string(shared ? i % 5 : i) + ".rv32a"));
    c.addresses.push_back(sha256(ContractDeploymentHashPreimage{
        .deployer_address = c.users[i % nusers].address,
        .nonce = sha256(std::tuple(c.seed, uint64_t(i))),
        .contract_hash = sha256(*c.apps.back())}));
  }
  auto cost = StorageCostModel::default_cost_model();
  LoadgenSuccessResult init{};
  init.genesis_configuration =
      builders[0].build(c.network, cost, *load_stock_genesis_root_contract(),
                        stock_network_config());
  for (size_t variant = 0; variant < (shared ? 1 : std::min(nwallets, nusers));
       ++variant) {
    auto transactions =
        builders[variant].try_make_genesis_ceremony_transactions(cost);
    if (!transactions)
      throw std::runtime_error("cannot generate wallet genesis ceremony");
    for (auto &tx : *transactions) {
      // Each builder starts at nonce zero. Preserve unique root invocations
      // across the combined, unsigned genesis ceremony.
      tx.tx.uniquifier.nonce = init.genesis_ceremony_txs.size();
      init.genesis_ceremony_txs.push_back(std::move(tx));
    }
  }
  size_t predeployed = count * 4 / 5;
  // Limit each setup block's attached code volume. Each deployment still takes
  // the real ingestion/execution path after the wallet-only genesis ceremony.
  for (size_t i = 0; i < predeployed; ++i) {
    if (i % 10 == 0)
      init.initialization_txs.emplace_back();
    init.initialization_txs.back().push_back(c.deploy(i));
  }
  // Exercise every app family and both signature weights before measurement.
  for (size_t i = 0; i < predeployed; ++i) {
    if (i % 10 == 0)
      init.initialization_txs.emplace_back();
    auto tx =
        c.transaction(i % nusers, Wallet::entrypoint<u8"call">(),
                      Wallet::make_calldata<u8"call">(
                          c.addresses[i], App::method_id<u8"act">(),
                          App::make_calldata<u8"act">(uint64_t(i),
                                                      c.addresses[0],
                                                      Calldata{1, 2, 3})));
    c.sign(tx, i % nusers);
    init.initialization_txs.back().push_back(std::move(tx));
  }
  write_bytes(out / "genesis.xdr", init);
  std::filesystem::copy_file(artifacts / "artifacts.json",
                             out / "artifacts.json");
  std::ofstream txout(out / "transactions.jsonl");
  txout.exceptions(std::ios::failbit | std::ios::badbit);
  auto emit = [&](const SignedTransaction &tx, uint64_t due, int phase,
                  const char *kind, size_t app, size_t user, int dependency) {
    std::string json;
    auto bytes = x23::serialize(tx);
    json::Writer(json).object([&](json::Writer &w) {
      w.field("dueNs", int64_t(due));
      w.field("phase", int64_t(phase));
      w.field("kind", kind);
      w.field("app", int64_t(app));
      w.field("user", int64_t(user));
      w.field("dependency", int64_t(dependency));
      w.field("hash", encode_base58(sha256(tx.tx)));
      w.field("raw", hex(bytes));
    });
    txout << json << '\n';
  };
  std::mt19937_64 rng(c.seed);
  size_t next_deploy = predeployed;
  uint64_t operations = 0;
  const std::array<uint64_t, 5> rates{100, 100, 500, 1000, 2500};
  uint64_t offset = 0;
  for (int phase = 0; phase < 5; ++phase) {
    uint64_t duration = phase == 0 ? warmup : stage;
    for (uint64_t second = 0; second < duration; ++second) {
      const bool burst =
          phase != 0 && second >= duration / 2 &&
          second < duration / 2 + std::min<uint64_t>(5, duration / 2);
      const uint64_t rate =
          std::max<uint64_t>(1, rates[phase] * multiplier / divisor) *
          (burst ? 5 : 1);
      if (phase > 0) {
        auto desired =
            predeployed + ((uint64_t(phase - 1) * stage + second + 1) *
                           (count - predeployed)) /
                              (stage * 4);
        while (next_deploy < desired) {
          emit(c.deploy(next_deploy), offset * 1'000'000'000, phase, "deploy",
               next_deploy, next_deploy % nusers, -1);
          ++next_deploy;
          ++operations;
        }
      }
      for (uint64_t j = 0; j < rate; ++j) {
        size_t user = rng() % nusers;
        size_t app;
        if (rng() % 5 == 0)
          app = rng() % std::min<size_t>(5, predeployed);
        else {
          auto candidates =
              (user < count ? (count + nusers - 1 - user) / nusers : 0);
          app = candidates > 0 ? user + (rng() % candidates) * nusers
                               : rng() % predeployed;
        }
        // Calls to not-yet-deployed apps are intentionally retained; the
        // replay records them as dependency-blocked instead of retargeting.
        bool auth = (rng() % 100 == 0);
        SignedTransaction tx;
        if (auth)
          tx = c.transaction(
              user, Wallet::entrypoint<u8"refresh_auth">(),
              Wallet::make_calldata<u8"refresh_auth">(c.users[user].keys[0],
                                                      uint32_t(1 + rng() % 2)));
        else {
          Calldata payload;
          payload.resize(std::array<size_t, 3>{32, 256, 1024}[rng() % 3],
                         uint8_t(rng()));
          auto data = App::make_calldata<u8"act">(c.users[user].sequence,
                                                  c.addresses[0], payload);
          tx = c.transaction(
              user, Wallet::entrypoint<u8"call">(),
              Wallet::make_calldata<u8"call">(c.addresses[app],
                                              App::method_id<u8"act">(), data));
        }
        c.sign(tx, user);
        emit(tx, offset * 1'000'000'000 + j * 1'000'000'000 / rate, phase,
             auth ? "auth" : "call", app, user,
             !auth && app >= predeployed ? int(app) : -1);
        ++operations;
      }
      ++offset;
    }
  }
  std::string metadata;
  json::Writer(metadata).object([&](json::Writer &w) {
    w.field("schemaVersion", int64_t(2));
    w.field("seed", int64_t(c.seed));
    w.field("users", int64_t(nusers));
    w.field("apps", int64_t(count));
    w.field("predeployed", int64_t(predeployed));
    w.field("walletVariants", int64_t(nwallets));
    w.field("sharedCode", shared);
    w.field("operations", int64_t(operations));
    w.field("durationSeconds", int64_t(offset));
    w.field("warmupSeconds", int64_t(warmup));
    w.field("stageSeconds", int64_t(stage));
    w.field("rateDivisor", int64_t(divisor));
    w.field("rateMultiplier", int64_t(multiplier));
    w.field("networkHash", encode_base58(c.network));
    w.array_field("initializationHashes", [&](json::Writer &a) {
      for (const auto &batch : init.initialization_txs)
        for (const auto &tx : batch)
          a.value(encode_base58(sha256(tx.tx)));
    });
    w.array_field("contracts", [&](json::Writer &a) {
      for (size_t i = 0; i < count; ++i)
        a.object([&](json::Writer &v) {
          v.field("app", int64_t(i));
          v.field("hash", encode_base58(sha256(*c.apps[i])));
          v.field("address", encode_base58(c.addresses[i]));
          v.field("family", int64_t(i % 5));
        });
    });
  });
  std::ofstream(out / "manifest.json") << metadata << '\n';
  std::cout << "Wrote " << operations << " operations to " << out << '\n';
}
} // namespace
} // namespace hogd
int
main(int argc, char **argv)
{
  if (hogd::hogd_init(argc, argv, false))
    return 1;
  try {
    hogd::generate(argc, argv);
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
