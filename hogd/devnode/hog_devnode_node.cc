#include "hogd/devnode/hog_devnode_node.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <system_error>
#include <utility>

#include "address_auth.hh"
#include "auth_card.hh"
#include "auth_claim.hh"
#include "auth_rpc.hh"
#include "db_sched.hh"
#include "event.hh"
#include "genesis.hh"
#include "hogd/auth/auth_server.h"
#include "hogd/auth/dbauth_client.h"
#include "hogd/base/cleanup.h"
#include "hogd/base/io.h"
#include "hogd/base/json.h"
#include "hogd/base/log.h"
#include "hogd/base/network.h"
#include "hogd/base/rpc_server.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/devnode/bootstrap_accounts.h"
#include "hogd/devnode/dev_wallet_profile.h"
#include "hogd/devnode/manifest_hex.h"
#include "hogd/devnode/manifest_parse.h"
#include "hogd/event/event_block_context.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/rpc_context.h"
#include "hogd/hogdb/rpc_from_auth.h"
#include "hogd/hogdb/rpc_from_exec.h"
#include "hogd/hogdb/rpc_from_gateway.h"
#include "hogd/hogdb/rpc_from_sched.h"
#include "hogd/hogdb/status.h"
#include "hogd/hogexec/interface/rpc_from_view_gateway.h"
#include "hogd/indexer/db.h"
#include "hogd/indexer/rpc_from_block_explorer.h"
#include "hogd/indexer/rpc_from_event.h"
#include "hogd/indexer/status.h"
#include "hogd/mempool/scheduler.h"
#include "hogd/mempool/shared.h"
#include "hogd/ramdb/db.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/version.h"
#include "indexer.hh"
#include "rpc_types.hh"
#include "sched_coord.hh"
#include "sched_db.hh"
#include "sched_event.hh"
#include "transaction.hh"
#include "types.hh"
#include "x23/async.h"
#include "x23/err.h"
#include "x23/fileio.h"
#include "x23/serialize.h"
#include "x23/vec.h"

namespace hogd {

namespace {
constexpr char kIndexerDbSubdir[] = "indexer-db";
constexpr char kRuntimeSubdir[] = "runtime";
constexpr char kRamDbSubdir[] = "ramdb";
constexpr char kEventSubdir[] = "events";
constexpr char kIndexerSubdir[] = "indexer";
constexpr char kDataDirMetadataFile[] = "hog-devnode-data.json";
constexpr std::size_t kMaxDataDirMetadataBytes = 64 * 1024;
constexpr int kMetadataTempOpenFlags =
    O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC; // NOLINT(misc-include-cleaner)

void
maybe_inject_post_executor_failure()
{
  const char *value =
      std::getenv("HOG_DEVNODE_TEST_THROW_AFTER_EXECUTOR_START");
  if (value != nullptr && std::string_view(value) == "1") {
    throw std::runtime_error("injected hog-devnode constructor failure");
  }
}

void
maybe_inject_post_genesis_failure()
{
  const char *value = std::getenv("HOG_DEVNODE_TEST_THROW_AFTER_GENESIS_LOAD");
  if (value != nullptr && std::string_view(value) == "1") {
    throw std::runtime_error("injected hog-devnode post-genesis failure");
  }
}

void
validate_bootstrap_account_auth_data(BootstrapAccountMetadata const &account,
                                     AddressAuthData const &auth_data)
{
  if (auth_data.required_weight != Weight{1}) {
    throw std::runtime_error("bootstrap account auth data required weight "
                             "does not match persisted metadata");
  }
  if (auth_data.public_keys[0] == nullptr) {
    throw std::runtime_error(
        "bootstrap account auth data is missing public key "
        "from persisted metadata");
  }
  if (auth_data.public_keys[0]->max_weight != Weight{1}) {
    throw std::runtime_error("bootstrap account auth data public key weight "
                             "does not match persisted metadata");
  }
  if (auth_data.public_keys[0]->key.scheme() != CryptographicScheme::Ed25519 ||
      auth_data.public_keys[0]->key.ed25519() != account.public_key) {
    throw std::runtime_error("bootstrap account public key does not match "
                             "persisted metadata");
  }
}

template<typename T>
T
require_io(ErrnoStatus<T> status, std::string_view operation)
{
  if (!status.has_value()) {
    throw std::runtime_error(
        std::format("{} failed: {}", operation, status.error()));
  }
  return std::move(*status);
}

void
require_io(ErrnoStatus<void> status, std::string_view operation)
{
  if (!status.has_value()) {
    throw std::runtime_error(
        std::format("{} failed: {}", operation, status.error()));
  }
}

std::filesystem::path
metadata_path(std::filesystem::path const &data_dir)
{
  return data_dir / kDataDirMetadataFile;
}

GenesisConfiguration
build_genesis_configuration(BootstrapAccountConfig const &config,
                            StorageCostModel const &cost_model)
{
  if (config.count == 0) {
    GenesisConfigurationBuilder builder;
    return builder.build(cost_model);
  }
  BootstrapAccountArtifact artifact = build_bootstrap_account_artifact(config);
  artifact.genesis_configuration.storage_cost_model = cost_model.to_xdr();
  return std::move(artifact.genesis_configuration);
}

std::string
build_data_dir_metadata_json(BootstrapAccountConfig const &config,
                             GenesisConfiguration const &genesis_configuration)
{
  std::string buffer;
  json::Writer(buffer).object([&](json::Writer &w) {
    w.field("schemaVersion", std::int64_t{1});
    w.field("kind", "hog-devnode-data");
    w.field("genesisHash", hex_string(sha256(genesis_configuration)));
    w.object_field("bootstrapConfig", [&](json::Writer &w) {
      w.field("devWalletProfile", config.dev_wallet_profile);
      w.field("devAccounts", static_cast<std::int64_t>(config.count));
      w.field("devAccountSeed", config.seed);
      w.field("devAccountTokens", std::to_string(config.storage_tokens));
    });
  });
  return buffer;
}

std::string
read_metadata_file(std::filesystem::path const &path)
{
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    if (ec) {
      throw std::runtime_error(
          std::format("could not inspect hog-devnode data metadata '{}': {}",
                      path.string(), ec.message()));
    }
    throw std::runtime_error(
        std::format("hog-devnode data metadata file is missing: {}",
                    path.string()));
  }

  const std::uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec) {
    throw std::runtime_error(
        std::format("could not size hog-devnode data metadata '{}': {}",
                    path.string(), ec.message()));
  }
  if (size > kMaxDataDirMetadataBytes) {
    throw std::runtime_error(
        std::format("hog-devnode data metadata exceeds {} bytes",
                    kMaxDataDirMetadataBytes));
  }

  File file = require_io(File::open(path.string(), O_RDONLY),
                         "open hog-devnode data metadata");

  std::string bytes(size, '\0');
  std::span<std::byte> remaining = std::as_writable_bytes(std::span(bytes));
  while (!remaining.empty()) {
    std::size_t count =
        require_io(file.read(remaining), "read hog-devnode data metadata");
    if (count == 0) {
      break;
    }
    remaining = remaining.subspan(count);
  }
  bytes.resize(bytes.size() - remaining.size());
  return bytes;
}

const json::Value &
require_object_field(const json::Value &parent, std::string_view key,
                     std::string_view context)
{
  const json::Value *field = parent.find(key);
  if (field == nullptr) {
    throw std::runtime_error(
        std::format("hog-devnode data metadata missing {}.{}", context, key));
  }
  if (field->type() != json::Value::Type::kObject) {
    throw std::runtime_error(
        std::format("hog-devnode data metadata field {}.{} is not an object",
                    context, key));
  }
  return *field;
}

std::string_view
require_string_field(const json::Value &parent, std::string_view key,
                     std::string_view context)
{
  const json::Value *field = parent.find(key);
  if (field == nullptr) {
    throw std::runtime_error(
        std::format("hog-devnode data metadata missing {}.{}", context, key));
  }
  std::optional<std::string_view> value = field->as_string();
  if (!value.has_value()) {
    throw std::runtime_error(
        std::format("hog-devnode data metadata field {}.{} is not a string",
                    context, key));
  }
  return *value;
}

std::int64_t
require_integer_field(const json::Value &parent, std::string_view key,
                      std::string_view context)
{
  const json::Value *field = parent.find(key);
  if (field == nullptr) {
    throw std::runtime_error(
        std::format("hog-devnode data metadata missing {}.{}", context, key));
  }
  std::optional<std::int64_t> value = field->as_integer();
  if (!value.has_value()) {
    throw std::runtime_error(
        std::format("hog-devnode data metadata field {}.{} is not an integer",
                    context, key));
  }
  return *value;
}

void
require_string_value(std::string_view actual, std::string_view expected,
                     std::string_view field)
{
  if (actual != expected) {
    throw std::runtime_error(std::format(
        "unsupported hog-devnode data metadata {} '{}', expected '{}'", field,
        actual, expected));
  }
}

BootstrapAccountConfig
parse_data_dir_metadata(std::string_view bytes)
{
  std::expected<json::Value, json::ParseError> parsed = json::parse(bytes);
  if (!parsed.has_value()) {
    throw std::runtime_error(
        std::format("hog-devnode data metadata JSON parse failed at {}: {}",
                    parsed.error().offset, parsed.error().message));
  }
  if (parsed->type() != json::Value::Type::kObject) {
    throw std::runtime_error("hog-devnode data metadata root is not an object");
  }

  const std::int64_t schema_version =
      require_integer_field(*parsed, "schemaVersion", "root");
  if (schema_version != 1) {
    throw std::runtime_error(std::format(
        "unsupported hog-devnode data metadata schemaVersion {}, expected 1",
        schema_version));
  }
  require_string_value(require_string_field(*parsed, "kind", "root"),
                       "hog-devnode-data", "kind");

  const json::Value &config_json =
      require_object_field(*parsed, "bootstrapConfig", "root");
  const std::int64_t count =
      require_integer_field(config_json, "devAccounts", "bootstrapConfig");
  if (count < 0 || std::cmp_greater(count, kMaxBootstrapAccounts)) {
    throw std::runtime_error(std::format(
        "hog-devnode data metadata devAccounts {} exceeds maximum {}", count,
        kMaxBootstrapAccounts));
  }

  BootstrapAccountConfig config{
      .count = static_cast<std::uint32_t>(count),
      .seed = std::string(require_string_field(config_json, "devAccountSeed",
                                               "bootstrapConfig")),
      .storage_tokens =
          parse_u64(require_string_field(config_json, "devAccountTokens",
                                         "bootstrapConfig"),
                    "hog-devnode data metadata devAccountTokens"),
      .dev_wallet_profile =
          std::string(require_string_field(config_json, "devWalletProfile",
                                           "bootstrapConfig")),
  };
  if (config.seed.empty()) {
    throw std::runtime_error(
        "hog-devnode data metadata devAccountSeed must not be empty");
  }
  if (config.dev_wallet_profile.empty()) {
    throw std::runtime_error(
        "hog-devnode data metadata devWalletProfile must not be empty");
  }
  resolve_dev_wallet_profile(config.dev_wallet_profile);

  const StorageCostModel cost_model = StorageCostModel::default_cost_model();
  const GenesisConfiguration genesis =
      build_genesis_configuration(config, cost_model);
  const std::string expected_genesis_hash = hex_string(sha256(genesis));
  require_string_value(require_string_field(*parsed, "genesisHash", "root"),
                       expected_genesis_hash, "genesisHash");
  return config;
}

void
require_matching_bootstrap_config(BootstrapAccountConfig const &persisted,
                                  BootstrapAccountConfig const &requested)
{
  auto fail = [](std::string_view flag, std::string_view expected,
                 std::string_view actual) {
    throw std::runtime_error(std::format(
        "hog-devnode data dir was initialized with different {}; expected "
        "'{}', got '{}'",
        flag, expected, actual));
  };
  if (persisted.count != requested.count) {
    fail("--dev-accounts", std::to_string(persisted.count),
         std::to_string(requested.count));
  }
  if (persisted.seed != requested.seed) {
    fail("--dev-account-seed", persisted.seed, requested.seed);
  }
  if (persisted.storage_tokens != requested.storage_tokens) {
    fail("--dev-account-tokens", std::to_string(persisted.storage_tokens),
         std::to_string(requested.storage_tokens));
  }
  if (persisted.dev_wallet_profile != requested.dev_wallet_profile) {
    fail("--dev-wallet-profile", persisted.dev_wallet_profile,
         requested.dev_wallet_profile);
  }
}

void
write_metadata_atomically(std::filesystem::path const &path,
                          std::string_view bytes)
{
  Directory dir = require_io(Directory::open(path.parent_path().string(),
                                             /*create=*/false),
                             "open hog-devnode data metadata directory");
  const std::string filename = path.filename().string();
  const std::string tmp_filename = std::format("{}.tmp", filename);
  Defer cleanup_tmp([&] { std::ignore = dir.unlink(tmp_filename); });
  File file = require_io(dir.openat(tmp_filename, kMetadataTempOpenFlags, 0600),
                         "open hog-devnode data metadata temp file");
  std::span<const std::byte> remaining =
      std::as_bytes(std::span(bytes.data(), bytes.size()));
  while (!remaining.empty()) {
    std::size_t count = require_io(file.write(remaining),
                                   "write hog-devnode data metadata temp file");
    if (count == 0) {
      throw std::runtime_error("write hog-devnode data metadata wrote 0 bytes");
    }
    remaining = remaining.subspan(count);
  }
  require_io(file.sync(), "fsync hog-devnode data metadata temp file");
  require_io(file.close(), "close hog-devnode data metadata temp file");
  require_io(dir.rename(tmp_filename, filename),
             "rename hog-devnode data metadata");
  require_io(dir.sync(), "fsync hog-devnode data metadata directory");
}

TxEventLogBatch
read_event_log_file(std::filesystem::path const &path)
{
  std::expected<x23::Buf, x23::Errno> bytes = x23::readfile<x23::Buf>(path);
  if (!bytes.has_value()) {
    throw std::runtime_error(
        std::format("failed to read recovered event log '{}': {}",
                    path.string(), bytes.error()));
  }

  TxEventLogBatch events;
  TxEventLog log;
  std::size_t offset = 0;
  std::span<const std::uint8_t> data(*bytes);
  try {
    while (offset < data.size()) {
      offset += x23::deserialize_from(&log, x23::BufView(data.subspan(offset)));
      events.push_back(log);
    }
  } catch (const x23::XdrBadInput &e) {
    throw std::runtime_error(
        std::format("failed to parse recovered event log '{}': {}",
                    path.string(), e.what()));
  }
  return events;
}
} // namespace

HogDevnodeNode::HogDevnodeNode(
    std::string bind_host, std::uint16_t requested_scheduler_port,
    BootstrapAccountConfig const &bootstrap_config,
    std::optional<std::filesystem::path> const &data_dir)
  : bind_host_(std::move(bind_host)), data_dir_(data_dir),
    bootstrap_config_(bootstrap_config), runtime_dir_(data_dir, kRuntimeSubdir),
    db_dir_(data_dir, kRamDbSubdir), event_dir_(data_dir, kEventSubdir),
    indexer_dir_(data_dir, kIndexerSubdir), event_ctx_(event_dir_.directory()),
    event_sched_server_(event_ctx_), event_exec_server_(event_ctx_),
    event_indexer_server_(event_ctx_)
{
  try {
    // Genesis bootstrap payloads can be larger than the transport default.
    mempool_coord_server_->max_message_size_ = kDevRpcMaxMessageSize;
    mempool_auth_server_->max_message_size_ = kDevRpcMaxMessageSize;

    // Start the mempool RPC servers. All of the subsystems register with the
    // mempool, so the mempool RPC servers need to start first.
    sched_db_server_.start_server(make_bind_host_port(bind_host_, /*port=*/0));
    sched_event_server_.start_server(
        make_bind_host_port(bind_host_, /*port=*/0));
    sched_exec_server_.start_server(
        make_bind_host_port(bind_host_, /*port=*/0));
    mempool_coord_server_.start_server(
        make_bind_host_port(bind_host_, requested_scheduler_port));
    mempool_auth_server_.start_server(
        make_bind_host_port(bind_host_, /*port=*/0));

    auto sched_db_client = std::make_unique<sched_db_client_t>(
        create_client<SchedDb1>(bind_host_, sched_db_server_.port())());
    init_db_backend(std::move(sched_db_client));

    // [db_executor_server_] handles RPCs from the executor.
    db_executor_server_ = std::make_unique<HogDBExecutorServer>(*ctx_);
    db_executor_server_->start_server(
        make_bind_host_port(bind_host_, /*port=*/0));
    // [db_scheduler_server_] handles RPCs from the mempool.
    db_scheduler_server_ = std::make_unique<HogDBSchedServer>(*ctx_);
    (*db_scheduler_server_)->max_message_size_ = kDevRpcMaxMessageSize;
    db_scheduler_server_->start_server(make_bind_host_port(bind_host_,
                                                           /*port=*/0));
    db_auth_server_ = std::make_unique<HogDBAuthServer>(*ctx_);
    db_auth_server_->start_server(make_bind_host_port(bind_host_, /*port=*/0));
    // Read-only state reads served to the public RPC gateway over x23.
    db_gateway_server_ = std::make_unique<HogDBGatewayServer>(*ctx_);
    db_gateway_server_->start_server(
        make_bind_host_port(bind_host_, /*port=*/0));

    std::string dbauth_address =
        format_advertised_address(bind_host_, db_auth_server_->port());
    dbauth_client_t dbauth_client = make_dbauth_client(dbauth_address)();
    auth_server_ = std::make_unique<AuthServer>(std::move(dbauth_client));

    auth_db_server_->max_message_size_ = kDevRpcMaxMessageSize;
    auth_mempool_server_->max_message_size_ = kDevRpcMaxMessageSize;
    auth_db_server_.start_server(make_bind_host_port(bind_host_, /*port=*/0));
    auth_mempool_server_.start_server(make_bind_host_port(bind_host_,
                                                          /*port=*/0));
    auth_db_server_->impl()->set_auth_server(*auth_server_);
    auth_mempool_server_->impl()->set_auth_server(*auth_server_);

    event_sched_server_.max_message_size_ = kDevRpcMaxMessageSize;
    event_exec_server_.max_message_size_ = kDevRpcMaxMessageSize;
    event_indexer_server_.max_message_size_ = kDevRpcMaxMessageSize;
    event_sched_server_.start_server(make_bind_host_port(bind_host_,
                                                         /*port=*/0));
    event_exec_server_.start_server(make_bind_host_port(bind_host_,
                                                        /*port=*/0));
    event_indexer_server_.start_server(make_bind_host_port(bind_host_,
                                                           /*port=*/0));

    view_gateway_server_ = std::make_unique<ViewGatewayServer>(
        format_advertised_address(bind_host_, db_executor_server_->port()),
        bind_host_, event_exec_server_.port());
    view_gateway_server_->start_server(make_bind_host_port(bind_host_,
                                                           /*port=*/0));

    init_indexer_backend();
    reconcile_indexer_with_recovered_tip();

    // We start the executor server in [register_executor_with_mempool()].

    // Start the fake coordinator. The real coordinator lives in chuckd, but
    // hog-devnode only needs the scheduler back-channel that the mempool
    // requires during startup.
    coordinator_sched_server_->max_message_size_ = kDevRpcMaxMessageSize;
    coordinator_sched_server_.start_server(make_bind_host_port(bind_host_,
                                                               /*port=*/0));

    // Now have all of the subsystems register themselves with the mempool.
    register_indexer_with_event_service();
    register_db_with_mempool();
    register_event_with_mempool();
    register_auth_with_mempool();
    // The executor must register itself *after* the database and event servers
    // have registered!
    register_executor_with_mempool();
    maybe_inject_post_executor_failure();
    register_coordinator_with_mempool()();
    if (recovered_existing_chain_) {
      bootstrap_config_ = validate_recovered_data_dir(bootstrap_config);
      load_recovered_bootstrap_metadata(bootstrap_config_);
      if (recovered_data_dir_metadata_missing_) {
        write_data_dir_metadata();
      }
    }
    else {
      bootstrap_config_ = validate_fresh_data_dir(bootstrap_config);
      write_data_dir_metadata();
      materialize_bootstrap_accounts(bootstrap_config_);
    }
  } catch (...) {
    prepare_for_failed_construction();
    throw;
  }
}

HogDevnodeNode::~HogDevnodeNode()
{
  try {
    // Shutdown is intentionally loud while this topology is still mirroring the
    // fragile MempoolTestInfra lifecycle; unexpected exceptions here are bugs.
    // [sched_db_server_] sends RPCs to [db_scheduler_server_] (via the
    // scheduler). Reset the scheduler pointer before destructing
    // [db_scheduler_server_] so that [sched_db_server_] does not send an RPC to
    // a stopped server.
    sched_db_server_.impl()->reset_scheduler()();
    // The scheduler sends RPCs to [db_scheduler_server_] in a detached
    // coroutine. Shut it down before destroying [db_scheduler_server_], or the
    // detached coroutine may send on an invalid file descriptor.
    mempool_coord_server_.impl()->scheduler()->shut_down()();
    // This mirrors MempoolTestInfra's current shutdown assumption: the RPC
    // servers do not yet have a coordinated clean-shutdown API, but executor
    // shutdown is required so the [Executor] destructor does not block.
    executor_.shut_down();

    view_gateway_server_->impl()->shut_down();
    view_gateway_server_.reset();
    db_gateway_server_.reset();
    db_auth_server_.reset();
    db_scheduler_server_.reset();
    db_executor_server_.reset();
    // [ctx_] holds a sched_db_client, so destroy it after the scheduler-facing
    // database servers are gone.
    ctx_.reset();
  } catch (const std::exception &e) {
    error("~HogDevnodeNode received exception: {}", e.what());
    CHECK(false);
  } catch (...) {
    error("~HogDevnodeNode received exception");
    CHECK(false);
  }
}

void
HogDevnodeNode::prepare_for_failed_construction() noexcept
{
  try {
    // Mirror ~HogDevnodeNode's scheduler teardown. If construction failed after
    // the coordinator registered the scheduler (e.g. the post-genesis failure
    // injection), the scheduler is running a detached coroutine that sends RPCs
    // to [db_scheduler_server_]. The member destructors that run after we
    // rethrow will tear down [db_scheduler_server_], so shut the scheduler down
    // first or the detached coroutine may send on an invalid file descriptor.
    // The scheduler is only registered once register_coordinator_with_mempool()
    // has run, so skip this when construction failed before then.
    std::shared_ptr<Scheduler> scheduler =
        mempool_coord_server_.impl()->scheduler();
    if (scheduler != nullptr) {
      sched_db_server_.impl()->reset_scheduler()();
      scheduler->shut_down()();
    }
    executor_.shut_down();
    if (view_gateway_server_ != nullptr) {
      view_gateway_server_->impl()->shut_down();
    }
  } catch (const std::exception &e) {
    error("HogDevnodeNode failed-start cleanup received exception: {}",
          e.what());
  } catch (...) {
    error("HogDevnodeNode failed-start cleanup received exception");
  }
}

void
HogDevnodeNode::init_indexer_backend()
{
  std::filesystem::path db_path =
      std::filesystem::path(indexer_dir_.directory()) / kIndexerDbSubdir;
  IndexerStatus<IndexDB> db_status = IndexDB::open_or_make_new(db_path);
  if (!db_status) {
    panic("[IndexDB::open_or_make_new] failed with {}", db_status.error());
  }
  IndexDB &db = db_status.value();
  indexer_db_ = std::make_unique<IndexDB>(std::move(db));

  indexer_event_server_ =
      std::make_unique<RpcServer<IndexerEventRpc>>(*indexer_db_);
  indexer_event_server_->max_message_size_ = kDevRpcMaxMessageSize;
  indexer_event_server_->start_server(make_bind_host_port(bind_host_,
                                                          /*port=*/0));

  indexer_block_explorer_server_ =
      std::make_unique<RpcServer<IndexerBlockExplorerRpc>>(*indexer_db_);
  indexer_block_explorer_server_->max_message_size_ = kDevRpcMaxMessageSize;
  indexer_block_explorer_server_->start_server(
      make_bind_host_port(bind_host_, /*port=*/0));
}

void
HogDevnodeNode::reconcile_indexer_with_recovered_tip()
{
  if (!recovered_existing_chain_) {
    return;
  }

  const DbTip tip = ctx_->get_tip();
  const BlockHeight tip_height = tip.value.proposal_header.block_height;
  const BlockHeight indexed_height = indexer_db_->latest_block();
  if (indexed_height >= tip_height) {
    return;
  }
  if (tip_height <= 0 || indexed_height + 1 != tip_height) {
    throw std::runtime_error(std::format(
        "recovered hog-devnode indexer is behind chain tip: indexed height {}, "
        "chain tip {}; only one-block tip replay is supported",
        indexed_height, tip_height));
  }

  std::filesystem::path event_path = EventBlockContext::final_events_filename(
      event_dir_.directory(), tip.value.proposal_header.events_hash);
  TxEventLogBatch events = read_event_log_file(event_path);
  // The on-disk event log file holds only event logs (transaction bodies are
  // forwarded out of band and are not part of the event trie), so this
  // recovery replay supplies no bodies.
  IndexerStatus<void> status =
      indexer_db_->consume_events(tip_height, events, {});
  if (!status.has_value()) {
    throw std::runtime_error(
        std::format("failed to replay recovered event log into indexer: {}",
                    status.error()));
  }
  info("replayed recovered event log '{}' into indexer at height {}",
       event_path.string(), tip_height);
}

void
HogDevnodeNode::register_indexer_with_event_service()
{
  auto client =
      create_client<EventIndexer1>(bind_host_, event_indexer_server_.port())();
  std::string indexer_event_address =
      format_advertised_address(bind_host_, indexer_event_server_->port());
  IndexerCard card{.protocol = version::kHogdProtocol(),
                   .address_for_event = indexer_event_address.c_str()};
  client.event_indexer_intro(card)();
}

BootstrapAccountConfig
HogDevnodeNode::validate_fresh_data_dir(
    BootstrapAccountConfig const &requested_config) const
{
  if (!data_dir_.has_value()) {
    return requested_config;
  }
  std::error_code ec;
  if (std::filesystem::exists(metadata_path(*data_dir_), ec)) {
    BootstrapAccountConfig persisted_config =
        parse_data_dir_metadata(read_metadata_file(metadata_path(*data_dir_)));
    require_matching_bootstrap_config(persisted_config, requested_config);
    return persisted_config;
  }
  if (ec) {
    throw std::runtime_error(
        std::format("could not inspect hog-devnode data dir metadata '{}': {}",
                    metadata_path(*data_dir_).string(), ec.message()));
  }
  return requested_config;
}

BootstrapAccountConfig
HogDevnodeNode::validate_recovered_data_dir(
    BootstrapAccountConfig const &requested_config)
{
  if (!data_dir_.has_value()) {
    return requested_config;
  }

  std::error_code ec;
  if (!std::filesystem::exists(metadata_path(*data_dir_), ec)) {
    if (ec) {
      throw std::runtime_error(std::format(
          "could not inspect hog-devnode data dir metadata '{}': {}",
          metadata_path(*data_dir_).string(), ec.message()));
    }
    recovered_data_dir_metadata_missing_ = true;
    warn("hog-devnode data dir '{}' has committed chain state but no metadata; "
         "reconstructing metadata from startup flags; ensure the flags match "
         "the original data dir",
         data_dir_->string());
    return requested_config;
  }

  BootstrapAccountConfig persisted_config =
      parse_data_dir_metadata(read_metadata_file(metadata_path(*data_dir_)));
  require_matching_bootstrap_config(persisted_config, requested_config);
  return persisted_config;
}

void
HogDevnodeNode::write_data_dir_metadata() const
{
  if (!data_dir_.has_value()) {
    return;
  }

  write_metadata_atomically(
      metadata_path(*data_dir_),
      build_data_dir_metadata_json(
          bootstrap_config_,
          build_genesis_configuration(bootstrap_config_,
                                      ctx_->get_cost_model())));
}

void
HogDevnodeNode::load_recovered_bootstrap_metadata(
    BootstrapAccountConfig const &config)
{
  if (config.count == 0) {
    genesis_configuration_ =
        build_genesis_configuration(config, ctx_->get_cost_model());
    return;
  }

  BootstrapAccountArtifact artifact = build_bootstrap_account_artifact(config);
  genesis_configuration_ = std::move(artifact.genesis_configuration);
  genesis_configuration_.storage_cost_model = ctx_->get_cost_model().to_xdr();
  bootstrap_accounts_ = std::move(artifact.accounts);
  if (ctx_->get_tip().value.proposal_header.block_height == 0) {
    throw std::runtime_error(
        "hog-devnode data dir has bootstrap genesis but no initialization "
        "block; reinitialize the data dir");
  }
  refresh_bootstrap_account_balances();
}

void
HogDevnodeNode::materialize_bootstrap_accounts(
    BootstrapAccountConfig const &config)
{
  auto client = create_client<SchedulerCoord1>(bind_host_,
                                               mempool_coord_server_.port())();
  if (config.count == 0) {
    GenesisConfigurationBuilder builder;
    genesis_configuration_ =
        builder.build(StorageCostModel::default_cost_model());
    CHECK_EQ(client.schedcoord_load_genesis_configuration(
                 GenesisConfiguration(genesis_configuration_))(),
             DbSchedResult::DbSchedResultOK);
    return;
  }

  BootstrapAccountArtifact artifact = build_bootstrap_account_artifact(config);
  genesis_configuration_ = artifact.genesis_configuration;
  CHECK_EQ(client.schedcoord_load_genesis_configuration(
               std::move(artifact.genesis_configuration))(),
           DbSchedResult::DbSchedResultOK);
  maybe_inject_post_genesis_failure();

  SignedTransactionBatch batch;
  batch.append_range(artifact.initialization_txs);
  commit_initialization_batch(batch);

  bootstrap_accounts_ = std::move(artifact.accounts);
  refresh_bootstrap_account_balances();
}

void
HogDevnodeNode::commit_initialization_batch(SignedTransactionBatch const &txs)
{
  if (txs.empty()) {
    return;
  }

  auto client = create_client<SchedulerCoord1>(bind_host_,
                                               mempool_coord_server_.port())();
  client.schedulercoord_submit_unvetted(t_to_parallel_batch(txs)())();

  SchedulerGetTipResult tip = client.schedcoord_get_tip()();
  CHECK(tip.outcome() == SchedulerGetTipOutcome::SchedulerGetTipOK);

  SchedulerProposalResult proposal =
      client.schedulercoord_propose_block(tip.db_tip().id, ProposalContext{})();
  CHECK(proposal.outcome() == SchedulerProposalOutcome::SchedulerProposalOK);
  ConsensusValue value{};
  value.proposal_header = proposal.proposed_block().proposal.header;
  CHECK(client.schedulercoord_commit_block(proposal.proposed_block().block_id,
                                           value)() ==
        SchedulerCommitOutcome::SchedulerCommitOK);
}

void
HogDevnodeNode::refresh_bootstrap_account_balances()
{
  for (BootstrapAccountMetadata &account : bootstrap_accounts_) {
    HogDBStatus<AddressAuthData> auth_data =
        ctx_->get_db().read_address_auth_data(account.address);
    if (!auth_data.has_value()) {
      throw std::runtime_error(
          "bootstrap account metadata does not match recovered chain: missing "
          "auth data");
    }
    validate_bootstrap_account_auth_data(account, *auth_data);

    HogDBStatus<int128_t> balance =
        ctx_->get_db().read_address_balance(account.address);
    if (!balance.has_value()) {
      throw std::runtime_error(
          "bootstrap account metadata does not match recovered chain: missing "
          "balance");
    }
    if (*balance < 0 ||
        std::cmp_greater(*balance, std::numeric_limits<std::uint64_t>::max())) {
      throw std::runtime_error(
          "bootstrap account metadata does not match recovered chain: invalid "
          "balance");
    }
    account.storage_tokens = static_cast<std::uint64_t>(*balance);
  }
}

void
HogDevnodeNode::init_db_backend(
    std::unique_ptr<sched_db_client_t> sched_db_client)
{
  DBStatus<std::uint64_t> lsn =
      Database::check(db_dir_.directory(), kRamDbBytes);
  CHECK(lsn.has_value());
  info("database '{}' passed integrity check; current LSN is {}",
       db_dir_.directory(), *lsn);

  DBStatus<std::unique_ptr<Database>> ramdb =
      Database::open_lsn(db_dir_.directory(), *lsn, kRamDbBytes);
  CHECK(ramdb.has_value());

  auto db = HogDBDAO(RAMDB(std::move(*ramdb)));
  ctx_ =
      std::make_unique<RPCContext>(std::move(db), std::move(sched_db_client));
  if (ctx_->has_committed_tip()) {
    HogDBStatus<StorageCostModel> cost_model = ctx_->get_db().read_cost_model();
    if (!cost_model) {
      panic("failed to read recovered cost model: {}", cost_model.error());
    }
    ctx_->initialize_cost_model(*cost_model);
    recovered_existing_chain_ = true;
    info("recovered hog-devnode database at committed height {}",
         ctx_->get_tip().value.proposal_header.block_height);
  }
  else {
    ctx_->initialize_cost_model(StorageCostModel::default_cost_model());
  }
}

void
HogDevnodeNode::register_db_with_mempool()
{
  auto client = create_client<SchedDb1>(bind_host_, sched_db_server_.port())();
  std::string exec_address =
      format_advertised_address(bind_host_, db_executor_server_->port());
  std::string sched_address =
      format_advertised_address(bind_host_, db_scheduler_server_->port());
  std::string auth_address =
      format_advertised_address(bind_host_, db_auth_server_->port());
  const BlockHeight block_height =
      std::max<BlockHeight>(ctx_->get_tip().value.proposal_header.block_height,
                            BlockHeight{0});
  client.scheddb_intro_shard({.protocol = version::kHogdProtocol(),
                              .block_height = block_height,
                              .address_for_exec = exec_address.c_str(),
                              .address_for_sched = sched_address.c_str(),
                              .address_for_auth = auth_address.c_str()})();
}

void
HogDevnodeNode::register_event_with_mempool()
{
  auto client =
      create_client<SchedEvent1>(bind_host_, sched_event_server_.port())();
  RPCError r = client.schedevent_intro_event(
      {.protocol = version::kHogdProtocol(),
       .upid = {},
       .address_for_exec =
           format_advertised_address(bind_host_, event_exec_server_.port())
               .c_str(),
       .address_for_sched =
           format_advertised_address(bind_host_, event_sched_server_.port())
               .c_str()})();
  CHECK_EQ(r, RPCError::kRPCNone);
}

void
HogDevnodeNode::register_auth_with_mempool()
{
  sched_auth_client_t client =
      create_client<MempoolAuth1>(bind_host_, mempool_auth_server_.port())();
  AuthCard auth_card = {
      .protocol = version::kHogdProtocol(),
      .upid = {},
      .address_for_db =
          format_advertised_address(bind_host_, auth_db_server_.port()).c_str(),
      .address_for_mempool =
          format_advertised_address(bind_host_, auth_mempool_server_.port())
              .c_str()};
  client.mempoolauth_intro_auth(auth_card)();
  auth_server_->set_mempool_client(std::move(client));
  auth_server_->subscribe_to_auth_data_changes(auth_card)();
}

void
HogDevnodeNode::register_executor_with_mempool()
{
  const sockaddr_storage coordinator_sockaddr =
      host_port_to_sockaddr(bind_host_, sched_exec_server_.port());
  executor_.start(bind_host_, /*self_port=*/0, coordinator_sockaddr);
}

x23::Async<void>
HogDevnodeNode::register_coordinator_with_mempool()
{
  auto client = create_client<SchedulerCoord1>(bind_host_,
                                               mempool_coord_server_.port())();
  std::string coord_address =
      format_advertised_address(bind_host_, coordinator_sched_server_.port());
  CoordCard card{.protocol = version::kHogdProtocol(),
                 .address_for_sched = coord_address.c_str()};
  CHECK_EQ(co_await client.schedulercoord_intro_coord(card),
           CoordinatorRegistrationOutcome::CoordinatorRegistrationOK);
}

} // namespace hogd
