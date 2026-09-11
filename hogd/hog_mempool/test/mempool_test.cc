#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <future>
#include <gtest/gtest.h>
#include <initializer_list>
#include <latch>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#include "address_auth.hh"
#include "auth_claim.hh"
#include "auth_rpc.hh"
#include "built_contracts/contract_loader.h"
#include "built_contracts/deploy_and_transfer.h"
#include "built_contracts/fibonacci.h"
#include "consensus.hh"
#include "contract.hh"
#include "db_sched.hh"
#include "exec_sched.hh"
#include "genesis.hh"
#include "header.hh"
#include "hogd/base/coropoll.h"
#include "hogd/base/log.h"
#include "hogd/base/test.h"
#include "hogd/base/types.h"
#include "hogd/base/xdr_util.h"
#include "hogd/crypto/crypto.h"
#include "hogd/crypto/domain_separators.h"
#include "hogd/genesis/genesis_configuration.h"
#include "hogd/genesis/genesis_constants.h"
#include "hogd/hog_db/status.h"
#include "hogd/hog_exec/common/limits.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/contract_trait_client.h"
#include "hogd/hog_mempool/config_hash.h"
#include "hogd/hog_mempool/registration.h"
#include "hogd/hog_mempool/rpc_from_auth.h"
#include "hogd/hog_mempool/rpc_from_coord.h"
#include "hogd/hog_mempool/rpc_from_event.h"
#include "hogd/hog_mempool/rpc_from_gateway.h"
#include "hogd/hog_mempool/scheduler.h"
#include "hogd/hog_mempool/shared.h"
#include "hogd/hog_mempool/test/infra.h"
#include "hogd/hog_mempool/test/txs.h"
#include "hogd/http/http.h"
#include "hogd/http/http_test.h"
#include "hogd/init.h"
#include "hogd/metrics/shared.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "hogd/version.h"
#include "hogd/xdr/protocol_token.h"
#include "integration.hh"
#include "parallel.hh"
#include "proposal.hh"
#include "proposal_hash.h"
#include "rpc_types.hh"
#include "sched_coord.hh"
#include "sched_event.hh"
#include "sched_gateway.hh"
#include "transaction.hh"
#include "types.hh"
#include "validator.hh"
#include "x23/async.h"
#include "x23/cleanup.h"
#include "x23/corosync.h"
#include "x23/rpc_common.h"
#include "x23/serialize.h"
#include "x23/vec.h"
#include "x23/x23.h"

namespace hogd {

namespace {

constexpr size_t kNumTxs = 100;

using deploy_and_transfer_client_t =
    ContractTraitClient<deploy_and_transfer::DeployAndTransfer>;

SHA256Hash
make_test_nonce(uint64_t value)
{
  return sha256(value);
}

std::tuple<uint64_t, uint64_t>
make_amount(uint128_t amount)
{
  return std::make_tuple(amount >> 64, amount & 0xffffffffffffffffULL);
}

ValidatorID
make_validator_id(uint8_t value)
{
  ValidatorID validator_id{};
  validator_id.fill(value);
  return validator_id;
}

SchedulerProposalResult
propose_block(MempoolTestInfra &server, const DbTip &parent,
              ProposalContext context = {})
{
  context.protocol = 0;
  return server.mempool_coord_rpc().schedulercoord_propose_block(
      parent.id, std::move(context))();
}

ConsensusValue
consensus_value(const DbTip &parent, const Proposal &proposal)
{
  return ConsensusValue{.protocol = 0,
                        .ext = {},
                        .prev_hash = sha256(parent.value),
                        .proposal_header = proposal.header,
                        .proposal_txs_hash = proposal_txs_compatibility_hash(
                            proposal.transactions),
                        .upgrades = {}};
}

SchedulerCommitOutcome
commit_block(MempoolTestInfra &server, const DbTip &parent,
             const SchedulerProposedBlock &proposed)
{
  return server.mempool_coord_rpc().schedulercoord_commit_block(
      proposed.block_id, consensus_value(parent, proposed.proposal))();
}

SchedulerCommitOutcome
commit_block(MempoolTestInfra &server, BlockID block_id, const DbTip &parent,
             const Proposal &proposal)
{
  return server.mempool_coord_rpc().schedulercoord_commit_block(
      block_id, consensus_value(parent, proposal))();
}

} // namespace

TEST(Mempool, ProposalTxsCompatibilityHashGoldenVector)
{
  ProposalTxs txs{};
  txs.hash.segment_tokens = {1, 2};
  txs.hash.segment_bytes = {4, 8};
  txs.hash.xdr = {0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 3};
  txs.nondeterministic_data.segment_tokens = {1};
  txs.nondeterministic_data.segment_bytes = {4};
  txs.nondeterministic_data.xdr = {0xde, 0xad, 0xbe, 0xef};

  const ProposalTxsHash expected = {
      0x3a, 0x3c, 0xe7, 0x82, 0x83, 0x94, 0x38, 0x26, 0xac, 0x43, 0x29,
      0x1f, 0xab, 0x79, 0xbd, 0x17, 0xc4, 0xad, 0x1d, 0x1e, 0xf9, 0xe2,
      0x2d, 0x8c, 0x72, 0x31, 0xa9, 0x7c, 0xb6, 0x68, 0xdc, 0xd9,
  };
  EXPECT_EQ(proposal_txs_compatibility_hash(txs), expected);
  EXPECT_NE(sha256(txs), expected);
}

TEST(Mempool, LookupBeforeReadyPreservesRequestCardinality)
{
  MempoolCoordinatorRpc coordinator(/*db_rpc=*/nullptr, /*auth_rpc=*/nullptr,
                                    NetworkHash{}, /*mempool_tx_size_limit=*/0);
  TransactionHashBatch hashes;
  hashes.resize(2);

  auto result = coordinator.schedcoord_lookup_transactions(hashes)();

  ASSERT_EQ(result.size(), hashes.size());
  EXPECT_FALSE(result[0]);
  EXPECT_FALSE(result[1]);
}

TEST(Mempool, SubmitUnvettedWithOutcomesBeforeReadyPreservesCardinality)
{
  MempoolCoordinatorRpc coordinator(/*db_rpc=*/nullptr, /*auth_rpc=*/nullptr,
                                    NetworkHash{}, /*mempool_tx_size_limit=*/0);
  MempoolGatewayRpc gateway(&coordinator);
  SignedTransactionBatch txs;
  txs.resize(2);

  const SchedGatewaySubmitOutcomeBatch outcomes =
      gateway.schedgateway_submit_tx_batch(t_to_parallel_batch(txs)())();

  ASSERT_EQ(outcomes.size(), txs.size());
  EXPECT_EQ(outcomes[0],
            SchedGatewaySubmitOutcome::SchedGatewaySubmitRetryable);
  EXPECT_EQ(outcomes[1],
            SchedGatewaySubmitOutcome::SchedGatewaySubmitRetryable);
}

TEST(Mempool, SubmitUnvettedWithOutcomesRejectsMalformedBatch)
{
  MempoolCoordinatorRpc coordinator(/*db_rpc=*/nullptr, /*auth_rpc=*/nullptr,
                                    NetworkHash{}, /*mempool_tx_size_limit=*/0);
  MempoolGatewayRpc gateway(&coordinator);
  SignedTransactionBatch txs;
  txs.resize(1);
  ParallelSignedTransactionBatch batch = t_to_parallel_batch(txs)();
  ASSERT_EQ(batch.segment_tokens.size(), 1UL);
  batch.segment_tokens[0]++;

  EXPECT_THROW(gateway.schedgateway_submit_tx_batch(std::move(batch))(),
               std::runtime_error);
}

TEST(Mempool, AuthenticatedPubkeyAccountMissingFromHogDbIsRecoverable)
{
  auto [raw_public_key, raw_secret_key] = ed25519_keygen();
  PublicKey public_key(CryptographicScheme::Ed25519);
  public_key.ed25519() = raw_public_key;
  SecretKey secret_key(CryptographicScheme::Ed25519);
  secret_key.ed25519() = raw_secret_key;
  PubkeyAccountPreimage preimage{.pubkey = public_key, .nonce = 42};
  const std::optional<ContractAddress> address = pubkey_auth_address(preimage);
  if (!address.has_value()) {
    ADD_FAILURE() << "failed to derive pubkey auth address";
    return;
  }

  fib_client_t fib_client(*address);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 1);
  AuthClaim claim(AuthClaimType::kPubkeySignature);
  claim.pubkey_preimage() = preimage;
  txs[0].tx.auth_claim = std::move(claim);
  txs[0].signatures.push_back(make_signature(secret_key, txs[0].tx,
                                             /*public_key_id=*/0,
                                             MempoolTestInfra::kNetworkHash));

  MempoolTestInfra server;
  MempoolGatewayRpc gateway(&server.mempool_coord_rpc());
  const ssize_t auth_failures_before =
      MempoolMetrics::instance().auth_failed_tx_count.value();

  const SchedGatewaySubmitOutcomeBatch outcomes =
      gateway.schedgateway_submit_tx_batch(
          t_to_parallel_batch(std::move(txs))())();

  ASSERT_EQ(outcomes.size(), 1UL);
  EXPECT_EQ(outcomes[0],
            SchedGatewaySubmitOutcome::SchedGatewaySubmitInternalError);
  EXPECT_EQ(MempoolMetrics::instance().auth_failed_tx_count.value(),
            auth_failures_before);
  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 0UL);
}

TEST(Mempool, EventStartupTimesOutWithoutCanonicalTip)
{
  Registration &registration = Registration::registration();
  registration.clear();
  x23::Defer cleanup([&registration] { registration.clear(); });

  ASSERT_DEATH(MempoolEventRpc::wait_for_registration_for(
                   std::chrono::milliseconds(20)),
               "event startup timed out waiting for canonical tip");
}

TEST(Mempool, EventStartupTimesOutWithoutActivation)
{
  Registration &registration = Registration::registration();
  registration.clear();
  x23::Defer cleanup([&registration] { registration.clear(); });
  ConsensusValue value{};
  value.proposal_header.block_height = 7;
  registration.publish_event_canonical_tip(
      EventCanonicalTip{.block_id = 70, .consensus_value = value});

  ASSERT_DEATH(MempoolEventRpc::wait_for_registration_for(
                   std::chrono::milliseconds(20)),
               "event startup timed out waiting for activation");
}

TEST(Mempool, EventPreparedCardIsUnavailableBeforeActivation)
{
  Registration &registration = Registration::registration();
  registration.clear();
  x23::Defer cleanup([&registration] { registration.clear(); });
  ConsensusValue value{};
  value.proposal_header.block_height = 7;
  registration.publish_event_canonical_tip(
      EventCanonicalTip{.block_id = 70, .consensus_value = value});

  EventCard card{.protocol = version::kHogdProtocol(),
                 .address_for_exec = "127.0.0.1:1",
                 .address_for_sched = "127.0.0.1:2"};
  auto prepared = MempoolEventRpc::schedevent_prepare_event(card);
  ASSERT_EQ(prepared.outcome(), EventPrepareOutcome::EventPrepareOK);
  EXPECT_FALSE(registration.event_registered(nullptr));
}

TEST(Mempool, EventSameCardPrepareRetryPreservesPendingSnapshot)
{
  Registration &registration = Registration::registration();
  registration.clear();
  x23::Defer cleanup([&registration] { registration.clear(); });
  ConsensusValue value{};
  value.proposal_header.block_height = 7;
  registration.publish_event_canonical_tip(
      EventCanonicalTip{.block_id = 70, .consensus_value = value});
  EventCard card{.protocol = version::kHogdProtocol(),
                 .address_for_exec = "127.0.0.1:1",
                 .address_for_sched = "127.0.0.1:2"};

  auto first = MempoolEventRpc::schedevent_prepare_event(card);
  ASSERT_EQ(first.outcome(), EventPrepareOutcome::EventPrepareOK);
  auto idempotent = MempoolEventRpc::schedevent_prepare_event(card);
  ASSERT_EQ(idempotent.outcome(), EventPrepareOutcome::EventPrepareOK);
  EXPECT_EQ(idempotent.snapshot().token, first.snapshot().token);
  EXPECT_EQ(idempotent.snapshot().tip, first.snapshot().tip);

  ConsensusValue changed_value = value;
  changed_value.proposal_header.block_height = 8;
  registration.publish_event_canonical_tip(
      EventCanonicalTip{.block_id = 80, .consensus_value = changed_value});
  auto lost_reply_retry = MempoolEventRpc::schedevent_prepare_event(card);
  ASSERT_EQ(lost_reply_retry.outcome(), EventPrepareOutcome::EventPrepareOK);
  EXPECT_EQ(lost_reply_retry.snapshot().token, first.snapshot().token);
  EXPECT_EQ(lost_reply_retry.snapshot().tip, first.snapshot().tip);
  auto activation =
      MempoolEventRpc::schedevent_activate_event(lost_reply_retry.snapshot());
  ASSERT_EQ(activation.outcome(), EventActivationOutcome::EventActivationRetry);
  EXPECT_EQ(activation.current_tip().block_id, 80);

  auto refreshed = MempoolEventRpc::schedevent_prepare_event(card);
  ASSERT_EQ(refreshed.outcome(), EventPrepareOutcome::EventPrepareOK);
  EXPECT_GT(refreshed.snapshot().token, first.snapshot().token);
  EXPECT_EQ(refreshed.snapshot().tip.block_id, 80);
  EXPECT_EQ(refreshed.snapshot().tip.consensus_value, changed_value);
  EXPECT_EQ(
      MempoolEventRpc::schedevent_activate_event(first.snapshot()).outcome(),
      EventActivationOutcome::EventActivationError);
  EXPECT_EQ(MempoolEventRpc::schedevent_activate_event(refreshed.snapshot())
                .outcome(),
            EventActivationOutcome::EventActivationOK);
}

TEST(Mempool, DifferentEventCardDoesNotReplacePendingRegistration)
{
  Registration &registration = Registration::registration();
  registration.clear();
  x23::Defer cleanup([&registration] { registration.clear(); });
  ConsensusValue value{};
  value.proposal_header.block_height = 7;
  registration.publish_event_canonical_tip(
      EventCanonicalTip{.block_id = 70, .consensus_value = value});
  EventCard first{.protocol = version::kHogdProtocol(),
                  .address_for_exec = "127.0.0.1:1",
                  .address_for_sched = "127.0.0.1:2"};
  EventCard contender{.protocol = version::kHogdProtocol(),
                      .address_for_exec = "127.0.0.1:3",
                      .address_for_sched = "127.0.0.1:4"};

  auto prepared = MempoolEventRpc::schedevent_prepare_event(first);
  ASSERT_EQ(prepared.outcome(), EventPrepareOutcome::EventPrepareOK);
  EXPECT_EQ(MempoolEventRpc::schedevent_prepare_event(contender).outcome(),
            EventPrepareOutcome::EventPrepareError);
  EXPECT_EQ(
      MempoolEventRpc::schedevent_activate_event(prepared.snapshot()).outcome(),
      EventActivationOutcome::EventActivationOK);
  EventCard registered;
  ASSERT_TRUE(registration.event_registered(&registered));
  EXPECT_EQ(registered, first);
}

TEST(Mempool, EventActivationRetryAfterLostSuccessReplyIsIdempotent)
{
  Registration &registration = Registration::registration();
  registration.clear();
  x23::Defer cleanup([&registration] { registration.clear(); });
  ConsensusValue value{};
  value.proposal_header.block_height = 7;
  registration.publish_event_canonical_tip(
      EventCanonicalTip{.block_id = 70, .consensus_value = value});
  EventCard card{.protocol = version::kHogdProtocol(),
                 .address_for_exec = "127.0.0.1:1",
                 .address_for_sched = "127.0.0.1:2"};
  auto first = MempoolEventRpc::schedevent_prepare_event(card);
  auto retry = MempoolEventRpc::schedevent_prepare_event(card);
  ASSERT_EQ(first.outcome(), EventPrepareOutcome::EventPrepareOK);
  ASSERT_EQ(retry.outcome(), EventPrepareOutcome::EventPrepareOK);

  EXPECT_EQ(
      MempoolEventRpc::schedevent_activate_event(retry.snapshot()).outcome(),
      EventActivationOutcome::EventActivationOK);
  EXPECT_EQ(
      MempoolEventRpc::schedevent_activate_event(first.snapshot()).outcome(),
      EventActivationOutcome::EventActivationOK);
  EventCard registered;
  ASSERT_TRUE(registration.event_registered(&registered));
  EXPECT_EQ(registered, card);
}

TEST(Mempool, EventPrepareUsesTypedPreGenesisAndInvalidTipOutcomes)
{
  Registration &registration = Registration::registration();
  registration.clear();
  x23::Defer cleanup([&registration] { registration.clear(); });
  EventCard card{.protocol = version::kHogdProtocol(),
                 .address_for_exec = "127.0.0.1:1",
                 .address_for_sched = "127.0.0.1:2"};

  EXPECT_EQ(MempoolEventRpc::schedevent_prepare_event(card).outcome(),
            EventPrepareOutcome::EventPrepareNotReady);
  ConsensusValue pregenesis{};
  pregenesis.proposal_header.block_height = -1;
  registration.publish_event_canonical_tip(
      EventCanonicalTip{.block_id = std::numeric_limits<BlockID>::max(),
                        .consensus_value = pregenesis});
  for (size_t retry = 0; retry < 3; ++retry) {
    EXPECT_EQ(MempoolEventRpc::schedevent_prepare_event(card).outcome(),
              EventPrepareOutcome::EventPrepareNotReady);
    EXPECT_FALSE(registration.event_registered(nullptr));
  }

  ConsensusValue invalid = pregenesis;
  invalid.proposal_header.block_height = -2;
  registration.publish_event_canonical_tip(
      EventCanonicalTip{.block_id = 0, .consensus_value = invalid});
  EXPECT_EQ(MempoolEventRpc::schedevent_prepare_event(card).outcome(),
            EventPrepareOutcome::EventPrepareError);
  EXPECT_FALSE(registration.event_registered(nullptr));
}

TEST(Mempool, EventPrepareRejectsMismatchedProtocol)
{
  Registration &registration = Registration::registration();
  registration.clear();
  x23::Defer cleanup([&registration] { registration.clear(); });
  ConsensusValue value{};
  value.proposal_header.block_height = 7;
  registration.publish_event_canonical_tip(
      EventCanonicalTip{.block_id = 70, .consensus_value = value});
  EventCard card{.protocol = protocol_from_text("hogp/not-this-binary"),
                 .address_for_exec = "127.0.0.1:1",
                 .address_for_sched = "127.0.0.1:2"};
  EXPECT_EQ(MempoolEventRpc::schedevent_prepare_event(card).outcome(),
            EventPrepareOutcome::EventPrepareError);
  EXPECT_FALSE(registration.event_registered(nullptr));
}

TEST(Mempool, ActivatedEventRejectsDifferentCardAndAcceptsSameSnapshot)
{
  Registration &registration = Registration::registration();
  registration.clear();
  x23::Defer cleanup([&registration] { registration.clear(); });
  ConsensusValue value{};
  value.proposal_header.block_height = 7;
  registration.publish_event_canonical_tip(
      EventCanonicalTip{.block_id = 70, .consensus_value = value});

  EventCard first{.protocol = version::kHogdProtocol(),
                  .address_for_exec = "127.0.0.1:1",
                  .address_for_sched = "127.0.0.1:2"};
  auto first_prepared = MempoolEventRpc::schedevent_prepare_event(first);
  ASSERT_EQ(first_prepared.outcome(), EventPrepareOutcome::EventPrepareOK);
  ASSERT_EQ(MempoolEventRpc::schedevent_activate_event(
                first_prepared.snapshot())
                .outcome(),
            EventActivationOutcome::EventActivationOK);
  ASSERT_TRUE(registration.event_registered(nullptr));

  EventCard replacement{.protocol = version::kHogdProtocol(),
                        .address_for_exec = "127.0.0.1:3",
                        .address_for_sched = "127.0.0.1:4"};
  auto replacement_prepared =
      MempoolEventRpc::schedevent_prepare_event(replacement);
  EXPECT_EQ(replacement_prepared.outcome(),
            EventPrepareOutcome::EventPrepareError);
  EXPECT_EQ(MempoolEventRpc::schedevent_activate_event(
                first_prepared.snapshot())
                .outcome(),
            EventActivationOutcome::EventActivationOK);
  EventCard registered;
  ASSERT_TRUE(registration.event_registered(&registered));
  EXPECT_EQ(registered, first);
}

TEST(Mempool, RegisteredEventSameCardRepreparesAtAdvancedCanonicalTip)
{
  Registration &registration = Registration::registration();
  registration.clear();
  x23::Defer cleanup([&registration] { registration.clear(); });
  ConsensusValue first_value{};
  first_value.proposal_header.block_height = 7;
  registration.publish_event_canonical_tip(
      EventCanonicalTip{.block_id = 70, .consensus_value = first_value});
  EventCard card{.protocol = version::kHogdProtocol(),
                 .address_for_exec = "127.0.0.1:1",
                 .address_for_sched = "127.0.0.1:2"};
  auto first = MempoolEventRpc::schedevent_prepare_event(card);
  ASSERT_EQ(first.outcome(), EventPrepareOutcome::EventPrepareOK);
  ASSERT_EQ(
      MempoolEventRpc::schedevent_activate_event(first.snapshot()).outcome(),
      EventActivationOutcome::EventActivationOK);

  ConsensusValue next_value = first_value;
  next_value.proposal_header.block_height = 8;
  registration.publish_event_canonical_tip(
      EventCanonicalTip{.block_id = 80, .consensus_value = next_value});
  auto refreshed = MempoolEventRpc::schedevent_prepare_event(card);

  ASSERT_EQ(refreshed.outcome(), EventPrepareOutcome::EventPrepareOK);
  EXPECT_GT(refreshed.snapshot().token, first.snapshot().token);
  EXPECT_EQ(refreshed.snapshot().tip.block_id, 80);
  EXPECT_EQ(MempoolEventRpc::schedevent_activate_event(refreshed.snapshot())
                .outcome(),
            EventActivationOutcome::EventActivationOK);
}

TEST(Mempool, EventActivationRejectsChangedCanonicalTip)
{
  Registration &registration = Registration::registration();
  registration.clear();
  x23::Defer cleanup([&registration] { registration.clear(); });
  ConsensusValue first_value{};
  first_value.proposal_header.block_height = 7;
  registration.publish_event_canonical_tip(
      EventCanonicalTip{.block_id = 70, .consensus_value = first_value});
  EventCard card{.protocol = version::kHogdProtocol(),
                 .address_for_exec = "127.0.0.1:1",
                 .address_for_sched = "127.0.0.1:2"};
  auto prepared = MempoolEventRpc::schedevent_prepare_event(card);
  ASSERT_EQ(prepared.outcome(), EventPrepareOutcome::EventPrepareOK);

  ConsensusValue changed_value = first_value;
  changed_value.proposal_header.block_height = 8;
  registration.publish_event_canonical_tip(
      EventCanonicalTip{.block_id = 80, .consensus_value = changed_value});
  auto activated =
      MempoolEventRpc::schedevent_activate_event(prepared.snapshot());
  ASSERT_EQ(activated.outcome(), EventActivationOutcome::EventActivationRetry);
  EXPECT_EQ(activated.current_tip().block_id, 80);
  EXPECT_FALSE(registration.event_registered(nullptr));
}

TEST(Mempool, EventActivationRejectsModifiedPreparedSnapshot)
{
  Registration &registration = Registration::registration();
  registration.clear();
  x23::Defer cleanup([&registration] { registration.clear(); });
  ConsensusValue value{};
  value.proposal_header.block_height = 7;
  registration.publish_event_canonical_tip(
      EventCanonicalTip{.block_id = 70, .consensus_value = value});
  EventCard card{.protocol = version::kHogdProtocol(),
                 .address_for_exec = "127.0.0.1:1",
                 .address_for_sched = "127.0.0.1:2"};
  auto prepared = MempoolEventRpc::schedevent_prepare_event(card);
  ASSERT_EQ(prepared.outcome(), EventPrepareOutcome::EventPrepareOK);

  EventRegistrationSnapshot modified = prepared.snapshot();
  modified.tip.block_id = 71;
  auto activated = MempoolEventRpc::schedevent_activate_event(modified);
  EXPECT_EQ(activated.outcome(), EventActivationOutcome::EventActivationError);
  EXPECT_FALSE(registration.event_registered(nullptr));
}

TEST(Mempool, ExactTipActivationPublishesPreparedCard)
{
  Registration &registration = Registration::registration();
  registration.clear();
  x23::Defer cleanup([&registration] { registration.clear(); });
  ConsensusValue value{};
  value.proposal_header.block_height = 7;
  registration.publish_event_canonical_tip(
      EventCanonicalTip{.block_id = 70, .consensus_value = value});
  EventCard card{.protocol = version::kHogdProtocol(),
                 .address_for_exec = "127.0.0.1:1",
                 .address_for_sched = "127.0.0.1:2"};
  auto prepared = MempoolEventRpc::schedevent_prepare_event(card);
  ASSERT_EQ(prepared.outcome(), EventPrepareOutcome::EventPrepareOK);
  auto activated =
      MempoolEventRpc::schedevent_activate_event(prepared.snapshot());
  ASSERT_EQ(activated.outcome(), EventActivationOutcome::EventActivationOK);
  EventCard registered;
  ASSERT_TRUE(registration.event_registered(&registered));
  EXPECT_EQ(registered, card);
}

TEST(Mempool, ShutdownBeforeReadyIsNoop)
{
  MempoolCoordinatorRpc coordinator(/*db_rpc=*/nullptr, /*auth_rpc=*/nullptr,
                                    NetworkHash{}, /*mempool_tx_size_limit=*/0);

  coordinator.schedulercoord_shutdown()();
}

TEST(MempoolAuthRpc, AuthCallbackBlocksUntilRegistration)
{
  MempoolAuthRpc auth_rpc;
  constexpr BlockHeight kDbTip = 4;
  std::vector<BlockHeight> callback_height;
  std::latch update_finished(1);

  // We run [auth_rpc.mempoolauth_auth_data_changed()] in a separate thread as
  // it blocks until a callback is registered.
  [](MempoolAuthRpc &auth_rpc, std::latch &update_finished) -> x23::Detached {
    co_await default_threadpool().bg();
    co_await auth_rpc.mempoolauth_auth_data_changed(kDbTip, KeyChangeBatch{});
    update_finished.count_down();
  }(auth_rpc, update_finished);
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_TRUE(callback_height.empty());

  auth_rpc.register_callback(
      [&callback_height](BlockHeight height,
                         KeyChangeBatch /*key_changes*/) -> x23::Async<void> {
        callback_height.push_back(height);
        co_return;
      },
      [] {})();

  update_finished.wait();
  auth_rpc.mempoolauth_auth_data_changed(kDbTip + 1, KeyChangeBatch{})();
  EXPECT_EQ(callback_height.size(), 2);
  EXPECT_EQ(callback_height[0], kDbTip);
  EXPECT_EQ(callback_height[1], kDbTip + 1);
}

TEST(MempoolAuthRpc, StartupUpdatesDoNotCoalesce)
{
  MempoolAuthRpc auth_rpc;
  constexpr BlockHeight kDbTip = 4;
  std::unordered_set<BlockHeight> callback_height;
  x23::CoroLock callback_height_lock;
  std::latch updates_finished(2);

  // We run [auth_rpc.mempoolauth_auth_data_changed()] in separate threads as
  // they block until a callback is registered.
  [](MempoolAuthRpc &auth_rpc, std::latch &updates_finished) -> x23::Detached {
    co_await default_threadpool().bg();
    co_await auth_rpc.mempoolauth_auth_data_changed(kDbTip, KeyChangeBatch{});
    updates_finished.count_down();
  }(auth_rpc, updates_finished);
  [](MempoolAuthRpc &auth_rpc, std::latch &updates_finished) -> x23::Detached {
    co_await default_threadpool().bg();
    co_await auth_rpc.mempoolauth_auth_data_changed(kDbTip + 1,
                                                    KeyChangeBatch{});
    updates_finished.count_down();
  }(auth_rpc, updates_finished);
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_TRUE(callback_height.empty());

  auth_rpc.register_callback(
      [&callback_height, &callback_height_lock](BlockHeight height,
                                                KeyChangeBatch /*key_changes*/)
          -> x23::Async<void> {
        x23::CoroGuard cg;
        co_await cg(callback_height_lock);

        EXPECT_TRUE(callback_height.insert(height).second);
        co_return;
      },
      [] {})();

  updates_finished.wait();
  EXPECT_EQ(callback_height.size(), 2);
  EXPECT_TRUE(callback_height.contains(kDbTip));
  EXPECT_TRUE(callback_height.contains(kDbTip + 1));
}

TEST(MempoolAuthRpc, UnregisterDrainsAdmittedCallbacks)
{
  MempoolAuthRpc auth_rpc;
  constexpr BlockHeight kDbTip = 4;
  std::latch callback_started(1);
  std::latch allow_callback_finish(1);
  std::latch unregister_started(1);
  std::atomic<size_t> callback_count = 0;

  auth_rpc.register_callback(
      [&](BlockHeight, KeyChangeBatch) -> x23::Async<void> {
        callback_count.fetch_add(1, std::memory_order_relaxed);
        callback_started.count_down();
        allow_callback_finish.wait();
        co_return;
      },
      [] {})();

  std::thread callback_thread([&] {
    auth_rpc.mempoolauth_auth_data_changed(kDbTip + 1, KeyChangeBatch{})();
  });
  callback_started.wait();

  std::promise<void> unregister_result;
  std::future<void> unregistered = unregister_result.get_future();
  std::thread unregister_thread([&] {
    unregister_started.count_down();
    auth_rpc.unregister_callback()();
    unregister_result.set_value();
  });
  unregister_started.wait();
  EXPECT_EQ(unregistered.wait_for(std::chrono::milliseconds(100)),
            std::future_status::timeout);

  allow_callback_finish.count_down();
  callback_thread.join();
  EXPECT_EQ(unregistered.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);
  unregister_thread.join();
}

TEST(Mempool, ExecutorCleanupFanoutDrainsBeforeRethrowingFirstException)
{
  MempoolTestInfra server;
  constexpr BlockID kBlock = std::numeric_limits<BlockID>::max() - 6;
  std::latch failure_started(1);
  std::latch blocked_started(1);
  std::latch allow_blocked_cleanup(1);
  std::atomic_size_t started{0};
  server.mempool_coord_rpc().set_finish_executor_started_test_hook(
      [&](BlockID block_id, size_t executor_idx) {
        EXPECT_EQ(block_id, kBlock);
        started.fetch_add(1, std::memory_order_relaxed);
        if (executor_idx == 0) {
          failure_started.count_down();
          throw std::runtime_error("first executor cleanup failure");
        }
        if (executor_idx == 1) {
          blocked_started.count_down();
          allow_blocked_cleanup.wait();
          throw std::runtime_error("blocked executor cleanup failure");
        }
        throw std::runtime_error("later executor cleanup failure");
      });

  // Run the cleanup on an explicit thread rather than std::async: libc++
  // detaches the std::async worker, so future::get() can return while the
  // worker is still tearing down coroutine frames that hold references to the
  // rethrown exception. That teardown frees the exception's what() buffer via
  // libc++abi's (TSAN-uninstrumented) refcounting, which TSAN reports as a
  // race against the EXPECT_STREQ below. Joining the thread before inspecting
  // the exception makes the ordering explicit.
  std::promise<void> cleanup_result;
  std::future<void> cleanup = cleanup_result.get_future();
  std::thread cleanup_thread([&] {
    try {
      server.mempool_coord_rpc().test_finish_all_executors(kBlock)();
      cleanup_result.set_value();
    } catch (...) {
      cleanup_result.set_exception(std::current_exception());
    }
  });
  failure_started.wait();
  blocked_started.wait();
  EXPECT_EQ(cleanup.wait_for(std::chrono::milliseconds(50)),
            std::future_status::timeout);

  allow_blocked_cleanup.count_down();
  cleanup_thread.join();
  bool caught_first_exception = false;
  try {
    cleanup.get();
  } catch (const std::runtime_error &e) {
    caught_first_exception = true;
    EXPECT_STREQ(e.what(), "first executor cleanup failure");
  }
  EXPECT_TRUE(caught_first_exception);
  EXPECT_EQ(started.load(std::memory_order_relaxed),
            MempoolCoordinatorRpc::kNumClientsPerExecutor);
  server.mempool_coord_rpc().set_finish_executor_started_test_hook({});
}

// TODO(jhumphri): Update this test once we implement authentication and
// vetting.
TEST(Mempool, SubmitUnvetted)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  server.mempool_coord_rpc().schedulercoord_submit_unvetted(
      t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 1UL);
}

// This submits duplicate txs at the same block height, which tests that the
// mempool scheduler can detect and discard duplicate txs. This does *not* test
// that hog_db detects duplicate uniquifiers.
TEST(Mempool, SubmitUnvettedDuplicateRejectedByMempool)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  server.mempool_coord_rpc().schedulercoord_submit_unvetted(
      t_to_parallel_batch(SignedTransactionBatch{txs[0], txs[0]})())();
  server.mempool_coord_rpc().schedulercoord_submit_unvetted(
      t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 1UL);
}

// This submits duplicate txs each at a *different* block height, which tests
// that hog_db can detect and discard duplicate txs. This does *not* test that
// the mempool scheduler detects duplicate txs.
TEST(Mempool, SubmitUnvettedDuplicateRejectedByHogDb)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  constexpr size_t kNumBlocks = 5;
  for (size_t i = 0; i < kNumBlocks; i++) {
    // We submit each duplicate tx at a different block height so that hog_db is
    // forced to reject duplicate uniquifiers rather than the mempool scheduler
    // identifying duplicate txs.
    server.mempool_coord_rpc().schedulercoord_submit_unvetted(
        t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
    EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 1UL);

    SchedulerGetTipResult get_tip =
        server.mempool_coord_rpc().schedcoord_get_tip()();
    ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    DbTip db_tip = get_tip.db_tip();
    EXPECT_EQ(db_tip.value.proposal_header.block_height, i + 1);

    auto res_nominate = propose_block(server, db_tip);
    ASSERT_EQ(res_nominate.outcome(),
              SchedulerProposalOutcome::SchedulerProposalOK);

    const Proposal &proposal = res_nominate.proposed_block().proposal;
    EXPECT_EQ(proposal.header.block_height, i + 2);

    SignedTransactionBatch parsed_txs;
    EXPECT_TRUE(parallel_batch_to_t(res_nominate.proposed_block().txs,
                                    parsed_txs, kNoBatchSizeLimit)());

    TransactionHashBatch tx_hashes;
    EXPECT_TRUE(parallel_batch_to_t(proposal.transactions.hash, tx_hashes,
                                    kNoBatchSizeLimit)());

    x23::XdrVec<NondeterministicProposalData> tx_nondeterministic_data;
    EXPECT_TRUE(parallel_batch_to_t(proposal.transactions.nondeterministic_data,
                                    tx_nondeterministic_data,
                                    kNoBatchSizeLimit)());

    if (i == 0) {
      // The tx should be included in the nominated block on the first iteration
      // as the tx has never been executed before.
      EXPECT_EQ(parsed_txs.size(), 1UL);
      EXPECT_EQ(parsed_txs[0], txs[0]);

      EXPECT_EQ(tx_hashes.size(), 1UL);
      EXPECT_EQ(tx_hashes[0], sha256(parsed_txs[0].tx));

      EXPECT_EQ(tx_nondeterministic_data.size(), 1UL);
    }
    else {
      // The tx should not be included in the nominated block on all iterations
      // after the first because it is a duplicate tx. Thus, hog_db should
      // reject the tx's uniquifier as a duplicate, so hog_exec should fail the
      // tx with error code [ExecResultTxNonDeterministicError]. The mempool
      // should then discard the tx and not include it in the nominated block.
      EXPECT_TRUE(parsed_txs.empty());
      EXPECT_TRUE(tx_hashes.empty());
      EXPECT_TRUE(tx_nondeterministic_data.empty());
    }

    auto res_commit =
        commit_block(server, db_tip, res_nominate.proposed_block());
    ASSERT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);
  }

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  EXPECT_EQ(get_tip.db_tip().value.proposal_header.block_height,
            kNumBlocks + 1);
}

TEST(Mempool, SubmitUnvettedMempoolMaxCapacityLimit)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);

  size_t mempool_tx_size_limit = 1'000;
  SignedTransactionBatch txs =
      generate_unsigned_txs(fib_client, mempool_tx_size_limit + 1);

  MempoolTestInfra server(/*skip_genesis_block=*/false, mempool_tx_size_limit);

  ParallelSignedTransactionBatch batch =
      t_to_parallel_batch(SignedTransactionBatch{txs})();
  MempoolGatewayRpc gateway(&server.mempool_coord_rpc());
  const SchedGatewaySubmitOutcomeBatch outcomes =
      gateway.schedgateway_submit_tx_batch(std::move(batch))();
  ASSERT_EQ(outcomes.size(), txs.size());
  for (size_t i = 0; i < mempool_tx_size_limit; i++) {
    EXPECT_EQ(outcomes[i],
              SchedGatewaySubmitOutcome::SchedGatewaySubmitAdmitted);
  }
  EXPECT_EQ(outcomes.back(),
            SchedGatewaySubmitOutcome::SchedGatewaySubmitRetryable);
  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(),
            mempool_tx_size_limit);

  // The legacy endpoint preserves its saturated fast path: it must reject
  // before attempting to decode even a malformed batch.
  ParallelSignedTransactionBatch malformed =
      t_to_parallel_batch(SignedTransactionBatch{txs[0]})();
  ASSERT_EQ(malformed.segment_tokens.size(), 1UL);
  malformed.segment_tokens[0]++;
  const ssize_t malformed_before =
      MempoolMetrics::instance().rejected_malformed_unvetted_rpc_count.value();
  const ssize_t capacity_before =
      MempoolMetrics::instance().rejected_capacity_rpc_count.value();
  server.mempool_coord_rpc().schedulercoord_submit_unvetted(
      std::move(malformed))();
  EXPECT_EQ(
      MempoolMetrics::instance().rejected_malformed_unvetted_rpc_count.value(),
      malformed_before);
  EXPECT_EQ(MempoolMetrics::instance().rejected_capacity_rpc_count.value(),
            capacity_before + 1);

  // A valid duplicate is zero-growth and remains idempotently admitted even
  // when no capacity slots remain.
  const SchedGatewaySubmitOutcomeBatch duplicate =
      gateway.schedgateway_submit_tx_batch(
          t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  ASSERT_EQ(duplicate.size(), 1UL);
  EXPECT_EQ(duplicate[0],
            SchedGatewaySubmitOutcome::SchedGatewaySubmitAdmitted);
  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(),
            mempool_tx_size_limit);
}

TEST(Mempool, SubmitUnvettedCapacityCountsOnlyDistinctNewBodies)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 3);
  MempoolTestInfra server(/*skip_genesis_block=*/false,
                          /*mempool_tx_size_limit=*/2);
  MempoolGatewayRpc gateway(&server.mempool_coord_rpc());

  const SchedGatewaySubmitOutcomeBatch first =
      gateway.schedgateway_submit_tx_batch(
          t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  ASSERT_EQ(first.size(), 1UL);
  EXPECT_EQ(first[0], SchedGatewaySubmitOutcome::SchedGatewaySubmitAdmitted);

  const SchedGatewaySubmitOutcomeBatch outcomes =
      gateway.schedgateway_submit_tx_batch(t_to_parallel_batch(
          SignedTransactionBatch{txs[0], txs[1], txs[2]})())();
  ASSERT_EQ(outcomes.size(), 3UL);
  EXPECT_EQ(outcomes[0], SchedGatewaySubmitOutcome::SchedGatewaySubmitAdmitted);
  EXPECT_EQ(outcomes[1], SchedGatewaySubmitOutcome::SchedGatewaySubmitAdmitted);
  EXPECT_EQ(outcomes[2],
            SchedGatewaySubmitOutcome::SchedGatewaySubmitRetryable);
  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 2UL);
}

TEST(Mempool, SubmitUnvettedCapacityCountsSameRequestDuplicateOnce)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 1);
  MempoolTestInfra server(/*skip_genesis_block=*/false,
                          /*mempool_tx_size_limit=*/1);
  MempoolGatewayRpc gateway(&server.mempool_coord_rpc());

  const SchedGatewaySubmitOutcomeBatch outcomes =
      gateway.schedgateway_submit_tx_batch(
          t_to_parallel_batch(SignedTransactionBatch{txs[0], txs[0]})())();
  ASSERT_EQ(outcomes.size(), 2UL);
  EXPECT_EQ(outcomes[0], SchedGatewaySubmitOutcome::SchedGatewaySubmitAdmitted);
  EXPECT_EQ(outcomes[1], SchedGatewaySubmitOutcome::SchedGatewaySubmitAdmitted);
  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 1UL);
}

TEST(Mempool, SubmitVetted)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 1UL);
}

// This submits duplicate txs at the same block height, which tests that the
// mempool scheduler can detect and discard duplicate txs. This does *not* test
// that hog_db detects duplicate uniquifiers.
TEST(Mempool, SubmitVettedDuplicateRejectedByMempool)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id,
      t_to_parallel_batch(SignedTransactionBatch{txs[0], txs[0]})())();
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 1UL);
}

// This submits duplicate txs each at a *different* block height, which tests
// that hog_db can detect and discard duplicate txs. This does *not* test that
// the mempool scheduler detects duplicate txs.
TEST(Mempool, SubmitVettedDuplicateRejectedByHogDb)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;
  ValidatorID validator_id{};

  constexpr size_t kNumBlocks = 5;
  for (size_t i = 0; i < kNumBlocks; i++) {
    // We submit each duplicate tx at a different block height so that hog_db is
    // forced to reject duplicate uniquifiers rather than the mempool scheduler
    // identifying duplicate txs.
    server.mempool_coord_rpc().schedulercoord_submit_vetted(
        validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
    EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 1UL);

    SchedulerGetTipResult get_tip =
        server.mempool_coord_rpc().schedcoord_get_tip()();
    ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    DbTip db_tip = get_tip.db_tip();
    EXPECT_EQ(db_tip.value.proposal_header.block_height, i + 1);

    auto res_nominate = propose_block(server, db_tip);
    ASSERT_EQ(res_nominate.outcome(),
              SchedulerProposalOutcome::SchedulerProposalOK);

    const Proposal &proposal = res_nominate.proposed_block().proposal;
    EXPECT_EQ(proposal.header.block_height, i + 2);

    SignedTransactionBatch parsed_txs;
    EXPECT_TRUE(parallel_batch_to_t(res_nominate.proposed_block().txs,
                                    parsed_txs, kNoBatchSizeLimit)());

    TransactionHashBatch tx_hashes;
    EXPECT_TRUE(parallel_batch_to_t(proposal.transactions.hash, tx_hashes,
                                    kNoBatchSizeLimit)());

    x23::XdrVec<NondeterministicProposalData> tx_nondeterministic_data;
    EXPECT_TRUE(parallel_batch_to_t(proposal.transactions.nondeterministic_data,
                                    tx_nondeterministic_data,
                                    kNoBatchSizeLimit)());

    if (i == 0) {
      // The tx should be included in the nominated block on the first iteration
      // as the tx has never been executed before.
      EXPECT_EQ(parsed_txs.size(), 1UL);
      EXPECT_EQ(parsed_txs[0], txs[0]);

      EXPECT_EQ(tx_hashes.size(), 1UL);
      EXPECT_EQ(tx_hashes[0], sha256(parsed_txs[0].tx));

      EXPECT_EQ(tx_nondeterministic_data.size(), 1UL);
    }
    else {
      // The tx should not be included in the nominated block on all iterations
      // after the first because it is a duplicate tx. Thus, hog_db should
      // reject the tx's uniquifier as a duplicate, so hog_exec should fail the
      // tx with error code [ExecResultTxNonDeterministicError]. The mempool
      // should then discard the tx and not include it in the nominated block.
      EXPECT_TRUE(parsed_txs.empty());
      EXPECT_TRUE(tx_hashes.empty());
      EXPECT_TRUE(tx_nondeterministic_data.empty());
    }

    auto res_commit =
        commit_block(server, db_tip, res_nominate.proposed_block());
    ASSERT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);
  }

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  EXPECT_EQ(get_tip.db_tip().value.proposal_header.block_height,
            kNumBlocks + 1);
}

TEST(Mempool, SubmitUnvettedMalformedBatchDoesNotCrash)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 1);

  MempoolTestInfra server;

  ParallelSignedTransactionBatch batch =
      t_to_parallel_batch(SignedTransactionBatch{txs[0]})();

  ASSERT_EQ(batch.segment_tokens.size(), 1UL);
  batch.segment_tokens[0]++;

  server.mempool_coord_rpc().schedulercoord_submit_unvetted(batch)();
  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 0UL);
}

TEST(Mempool, SubmitVettedMalformedBatchDoesNotCrash)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 1);

  MempoolTestInfra server;

  ParallelSignedTransactionBatch batch =
      t_to_parallel_batch(SignedTransactionBatch{txs[0]})();

  ASSERT_EQ(batch.segment_tokens.size(), 1UL);
  batch.segment_tokens[0]++;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                          batch)();
  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 0UL);
}

TEST(Mempool, LookupBorrowsTheScheduledTransactionInsteadOfCopyingIt)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  server.mempool_coord_rpc().schedulercoord_submit_unvetted(
      t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  ASSERT_EQ(server.mempool_coord_rpc().scheduler()->size(), 1UL);

  const TransactionHash present = sha256(txs[0].tx);
  const TransactionHash absent = sha256(txs[1].tx);
  ASSERT_NE(present, absent);

  // A repeated hash and a miss, so both the sharing and the absent-slot paths
  // are covered and the reply is checked to stay positional.
  TransactionHashBatch hashes;
  hashes.push_back(present);
  hashes.push_back(absent);
  hashes.push_back(present);

  auto result =
      server.mempool_coord_rpc().schedcoord_lookup_transactions(hashes)();

  ASSERT_EQ(result.size(), hashes.size());
  EXPECT_FALSE(result[1]);
  ASSERT_TRUE(result[0]);
  ASSERT_TRUE(result[2]);
  EXPECT_EQ(*result[0], txs[0]);
  EXPECT_EQ(*result[2], txs[0]);

  // Both slots point at the transaction the scheduler holds rather than at
  // copies of it.  This is the regression guard: reintroducing the deep copy
  // makes these addresses differ.
  const SignedTransaction *scheduled =
      server.mempool_coord_rpc().scheduler()->find_tx(present).get();
  ASSERT_NE(scheduled, nullptr);
  EXPECT_EQ(result[0].get(), scheduled);
  EXPECT_EQ(result[2].get(), scheduled);
}

TEST(Mempool, Propose)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip db_tip = get_tip.db_tip();
  auto res = propose_block(server, db_tip);
  EXPECT_EQ(res.outcome(), SchedulerProposalOutcome::SchedulerProposalOK);

  {
    SignedTransactionBatch parsed_txs;
    EXPECT_TRUE(parallel_batch_to_t(res.proposed_block().txs, parsed_txs,
                                    kNoBatchSizeLimit)());
    EXPECT_EQ(parsed_txs.size(), 1UL);
    EXPECT_EQ(parsed_txs[0], txs[0]);

    TransactionHashBatch tx_hashes;
    EXPECT_TRUE(
        parallel_batch_to_t(res.proposed_block().proposal.transactions.hash,
                            tx_hashes, kNoBatchSizeLimit)());
    EXPECT_EQ(tx_hashes.size(), 1UL);
    EXPECT_EQ(tx_hashes[0], sha256(parsed_txs[0].tx));
  }
}

TEST(Mempool, NominateBatch)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                          t_to_parallel_batch(
                                                              txs)())();

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip db_tip = get_tip.db_tip();
  auto res = propose_block(server, db_tip);
  EXPECT_EQ(res.outcome(), SchedulerProposalOutcome::SchedulerProposalOK);

  {
    SignedTransactionBatch parsed_txs;
    EXPECT_TRUE(parallel_batch_to_t(res.proposed_block().txs, parsed_txs,
                                    kNoBatchSizeLimit)());
    EXPECT_EQ(parsed_txs.size(), txs.size());
    std::set<SignedTransaction> parsed_txs_set(parsed_txs.begin(),
                                               parsed_txs.end());
    std::set<SignedTransaction> txs_set(txs.begin(), txs.end());
    EXPECT_EQ(parsed_txs_set, txs_set);

    TransactionHashBatch tx_hashes;
    EXPECT_TRUE(
        parallel_batch_to_t(res.proposed_block().proposal.transactions.hash,
                            tx_hashes, kNoBatchSizeLimit)());
    EXPECT_EQ(tx_hashes.size(), txs.size());
    std::set<TransactionHash> parsed_tx_hashes_set(tx_hashes.begin(),
                                                   tx_hashes.end());
    std::set<TransactionHash> tx_hashes_set;
    for (const SignedTransaction &tx : txs) {
      EXPECT_TRUE(tx_hashes_set.insert(sha256(tx.tx)).second);
    }
    EXPECT_EQ(parsed_tx_hashes_set, tx_hashes_set);
  }
}

TEST(Mempool, ProposalExceptionRollsBackPendingNomination)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 1);
  MempoolTestInfra server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                          t_to_parallel_batch(
                                                              txs)())();
  const DbTip db_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()().db_tip();

  server.mempool_coord_rpc().set_propose_batch_result_test_hook(
      [](ExecProposeBatchResult &) {
        throw std::runtime_error("injected proposal failure");
      });
  EXPECT_THROW(propose_block(server, db_tip), std::runtime_error);
  server.mempool_coord_rpc().set_propose_batch_result_test_hook({});

  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), txs.size());
  const SchedulerProposalResult retry = propose_block(server, db_tip);
  ASSERT_EQ(retry.outcome(), SchedulerProposalOutcome::SchedulerProposalOK);
  SignedTransactionBatch proposed_txs;
  ASSERT_TRUE(parallel_batch_to_t(retry.proposed_block().txs, proposed_txs,
                                  kNoBatchSizeLimit)());
  EXPECT_EQ(proposed_txs, txs);
}

TEST(Mempool, ProposalRecoversAfterInitializeBlockFailure)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 1);
  MempoolTestInfra server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                          t_to_parallel_batch(
                                                              txs)())();
  const DbTip db_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()().db_tip();

  server.event_aggregator().fail_next_open_block();
  const SchedulerProposalResult failed = propose_block(server, db_tip);
  EXPECT_EQ(failed.outcome(), SchedulerProposalOutcome::SchedulerProposalError);

  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), txs.size());
  const SchedulerProposalResult retry = propose_block(server, db_tip);
  ASSERT_EQ(retry.outcome(), SchedulerProposalOutcome::SchedulerProposalOK);
  SignedTransactionBatch proposed_txs;
  ASSERT_TRUE(parallel_batch_to_t(retry.proposed_block().txs, proposed_txs,
                                  kNoBatchSizeLimit)());
  EXPECT_EQ(proposed_txs, txs);
}

TEST(Mempool, NominateEmpty)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);

  MempoolTestInfra server;

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip db_tip = get_tip.db_tip();
  auto res = propose_block(server, db_tip);
  EXPECT_EQ(res.outcome(), SchedulerProposalOutcome::SchedulerProposalOK);

  {
    SignedTransactionBatch parsed_txs;
    EXPECT_TRUE(parallel_batch_to_t(res.proposed_block().txs, parsed_txs,
                                    kNoBatchSizeLimit)());
    EXPECT_TRUE(parsed_txs.empty());
  }
}

TEST(Mempool, NominateTxWithError)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  constexpr size_t kNumBlocks = 2;
  for (size_t i = 0; i < kNumBlocks; i++) {
    // We submit each duplicate tx at a different block height so that hog_db is
    // forced to reject duplicate uniquifiers rather than the mempool scheduler
    // identifying duplicate txs. This is necessary to cause the tx to fail with
    // [ExecResultTxNonDeterministicError].
    server.mempool_coord_rpc().schedulercoord_submit_unvetted(
        t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
    EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 1UL);

    SchedulerGetTipResult get_tip =
        server.mempool_coord_rpc().schedcoord_get_tip()();
    ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    DbTip db_tip = get_tip.db_tip();
    EXPECT_EQ(db_tip.value.proposal_header.block_height, i + 1);

    auto res_nominate = propose_block(server, db_tip);
    ASSERT_EQ(res_nominate.outcome(),
              SchedulerProposalOutcome::SchedulerProposalOK);

    const Proposal &proposal = res_nominate.proposed_block().proposal;
    EXPECT_EQ(proposal.header.block_height, i + 2);

    SignedTransactionBatch parsed_txs;
    EXPECT_TRUE(parallel_batch_to_t(res_nominate.proposed_block().txs,
                                    parsed_txs, kNoBatchSizeLimit)());

    TransactionHashBatch tx_hashes;
    EXPECT_TRUE(parallel_batch_to_t(proposal.transactions.hash, tx_hashes,
                                    kNoBatchSizeLimit)());

    x23::XdrVec<NondeterministicProposalData> tx_nondeterministic_data;
    EXPECT_TRUE(parallel_batch_to_t(proposal.transactions.nondeterministic_data,
                                    tx_nondeterministic_data,
                                    kNoBatchSizeLimit)());

    if (i == 0) {
      // The tx should be included in the nominated block on the first iteration
      // as the tx has never been executed before.
      EXPECT_EQ(parsed_txs.size(), 1UL);
      EXPECT_EQ(parsed_txs[0], txs[0]);

      EXPECT_EQ(tx_hashes.size(), 1UL);
      EXPECT_EQ(tx_hashes[0], sha256(parsed_txs[0].tx));

      EXPECT_EQ(tx_nondeterministic_data.size(), 1UL);
    }
    else {
      // The tx should not be included in the nominated block on all iterations
      // after the first because it is a duplicate tx. Thus, hog_db should
      // reject the tx's uniquifier as a duplicate, so hog_exec should fail the
      // tx with error code [ExecResultTxNonDeterministicError]. The mempool
      // should then discard the tx and not include it in the nominated block.
      EXPECT_TRUE(parsed_txs.empty());
      EXPECT_TRUE(tx_hashes.empty());
      EXPECT_TRUE(tx_nondeterministic_data.empty());
    }

    auto res_commit =
        commit_block(server, db_tip, res_nominate.proposed_block());
    ASSERT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);
  }

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  EXPECT_EQ(get_tip.db_tip().value.proposal_header.block_height,
            kNumBlocks + 1);
}

// This test does the following to exercise a failed CHECK that was later fixed:
//   1. Submit an affordable tx (txs[0]) and one whose gas_limit exceeds the
//      account's balance (txs[1]).
//   2. Nominate: txs[0] is included in the block; txs[1] is over budget, so the
//      account is removed from [ready_accounts_] while the nomination is
//      pending.
//   3. The client resubmits txs[0]. It is added back to the scheduler runqueue;
//      the account is *not* added back to [ready_accounts_].
//   4. Commit the nominated block. [remove_from_scheduler()] removes the
//      account's txs from the runqueue. In that function, [account_is_ready()]
//      returns false and [ready_accounts_.erase()] returns 0. Before the fix,
//      [ready_accounts_.erase()] would trigger a CHECK.
TEST(Mempool, CommitOfExcludedAccountTxSucceeds)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 2);
  txs[0].tx.uniquifier.block_expiry_height = 10; // affordable, scheduled first
  txs[1].tx.uniquifier.block_expiry_height = 11;
  // Over budget: 20x the default tx gas is 2x the account's funded balance.
  txs[1].tx.invocation.gas_limit = txs[0].tx.invocation.gas_limit * 20;

  MempoolTestInfra server;

  // 1. Submit both txs through the coordinator's honest ingress.
  server.mempool_coord_rpc().schedulercoord_submit_unvetted(
      t_to_parallel_batch(SignedTransactionBatch{txs[0], txs[1]})())();
  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 2UL);

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip db_tip = get_tip.db_tip();

  // 2. Nominate: txs[0] is included; txs[1] excludes the account from the
  // block.
  auto res_nominate = propose_block(server, db_tip);
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);

  // 3. Submit txs[0] (the affordable tx) again.
  server.mempool_coord_rpc().schedulercoord_submit_unvetted(
      t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();

  // 4. Commit the nominated block. [remove_from_scheduler()] removes txs[0]
  // from the runqueue and attempts to remove the account from
  // [ready_accounts_].
  auto res_commit = commit_block(server, db_tip, res_nominate.proposed_block());
  ASSERT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);
}

TEST(Mempool, BlockHeightsIncrement)
{
  MempoolTestInfra server;

  constexpr size_t kNumBlocks = 5;
  for (size_t i = 0; i < kNumBlocks; i++) {
    SchedulerGetTipResult get_tip =
        server.mempool_coord_rpc().schedcoord_get_tip()();
    ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    DbTip db_tip = get_tip.db_tip();
    EXPECT_EQ(db_tip.value.proposal_header.block_height, i + 1);

    auto res_nominate = propose_block(server, db_tip);
    ASSERT_EQ(res_nominate.outcome(),
              SchedulerProposalOutcome::SchedulerProposalOK);

    const Proposal &proposal = res_nominate.proposed_block().proposal;
    EXPECT_EQ(proposal.header.block_height, i + 2);

    auto res_commit =
        commit_block(server, db_tip, res_nominate.proposed_block());
    ASSERT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);
  }

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  EXPECT_EQ(get_tip.db_tip().value.proposal_header.block_height,
            kNumBlocks + 1);
}

TEST(Mempool, NominateCommit)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip db_tip = get_tip.db_tip();
  auto res_nominate = propose_block(server, db_tip);
  EXPECT_EQ(res_nominate.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);

  {
    SignedTransactionBatch parsed_txs;
    EXPECT_TRUE(parallel_batch_to_t(res_nominate.proposed_block().txs,
                                    parsed_txs, kNoBatchSizeLimit)());
    EXPECT_EQ(parsed_txs.size(), 1UL);
    EXPECT_EQ(parsed_txs[0], txs[0]);

    TransactionHashBatch tx_hashes;
    EXPECT_TRUE(parallel_batch_to_t(
        res_nominate.proposed_block().proposal.transactions.hash, tx_hashes,
        kNoBatchSizeLimit)());
    EXPECT_EQ(tx_hashes.size(), 1UL);
    EXPECT_EQ(tx_hashes[0], sha256(parsed_txs[0].tx));
  }

  auto res_commit =
      commit_block(server, get_tip.db_tip(), res_nominate.proposed_block());
  EXPECT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);
}

TEST(Mempool, NewlyDeployedContractHogCount)
{
  constexpr ContractAddress kDeployAndTransferAddress = make_account_address(7);
  constexpr uint128_t kTransferAmount = 100'000'000;
  const SHA256Hash deploy_nonce = make_test_nonce(1);
  std::shared_ptr<Contract> fibonacci_contract =
      load_rv32_contract("fibonacci.rv32a");
  ASSERT_TRUE(fibonacci_contract);
  const SHA256Hash fibonacci_hash = sha256(*fibonacci_contract);
  const ContractAddress deployed_address =
      sha256(ContractDeploymentHashPreimage{.deployer_address =
                                                kDeployAndTransferAddress,
                                            .nonce = deploy_nonce,
                                            .contract_hash = fibonacci_hash});

  deploy_and_transfer_client_t deploy_and_transfer_client(
      kDeployAndTransferAddress);
  SignedTransaction tx{
      .tx = Transaction{
          .invocation = deploy_and_transfer_client
                            .make_invocation<u8"deploy_and_transfer">(
                                /*gas_amount=*/5'000'000'000, deploy_nonce,
                                fibonacci_hash, make_amount(kTransferAmount)),
          .uniquifier = {.block_expiry_height =
                             std::numeric_limits<BlockHeight>::max(),
                         .nonce = 1},
          .contracts_to_deploy = {*fibonacci_contract},
      }};

  MempoolTestInfra server;
  server.deploy_built_contract_to_address(kDeployAndTransferAddress,
                                          "deploy_and_transfer.rv32a");

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{tx})())();

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  auto res_nominate = propose_block(server, get_tip.db_tip());
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);

  SignedTransactionBatch parsed_txs;
  ASSERT_TRUE(parallel_batch_to_t(res_nominate.proposed_block().txs, parsed_txs,
                                  kNoBatchSizeLimit)());
  ASSERT_EQ(parsed_txs.size(), 1UL);
  EXPECT_EQ(parsed_txs[0], tx);

  TransactionHashBatch tx_hashes;
  ASSERT_TRUE(parallel_batch_to_t(
      res_nominate.proposed_block().proposal.transactions.hash, tx_hashes,
      kNoBatchSizeLimit)());
  ASSERT_EQ(tx_hashes.size(), 1UL);
  EXPECT_EQ(tx_hashes[0], sha256(tx.tx));

  auto res_commit =
      commit_block(server, get_tip.db_tip(), res_nominate.proposed_block());
  ASSERT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);

  HogDbStatus<DeployedContractHash> deployed_metadata =
      server.read_address_metadata(deployed_address);
  ASSERT_TRUE(deployed_metadata.has_value());
  EXPECT_EQ(deployed_metadata->hash, fibonacci_hash);

  HogDbStatus<int128_t> deployed_balance =
      server.read_address_balance(deployed_address);
  ASSERT_TRUE(deployed_balance.has_value());
  EXPECT_GT(*deployed_balance, 0);

  server.mempool_coord_rpc().wait_for_transfer_data(
      res_nominate.proposed_block().proposal.header.block_height)();
  auto [deployed_hog_age, deployed_hog_age_lock] =
      server.mempool_coord_rpc().scheduler()->get_cached_hog_age(
          deployed_address);
  ASSERT_EQ(deployed_hog_age->address, deployed_address);
  EXPECT_EQ(from_xdr(deployed_hog_age->num_hogs[kHogAgeGroupToSchedule]), 0UL);
  EXPECT_EQ(from_xdr(deployed_hog_age->num_hogs[kHogAgeGroupAlreadyScheduled]),
            0UL);
  // The fibonacci contract needs to pay for storage, so the storage cost is
  // deducted from the live hogs used for scheduling. Thus, the number of hogs
  // in the [kHogAgeGroupReceived] scheduling group will be less than
  // [kTransferAmount].
  EXPECT_GT(from_xdr(deployed_hog_age->num_hogs[kHogAgeGroupReceived]), 0UL);
  EXPECT_LT(from_xdr(deployed_hog_age->num_hogs[kHogAgeGroupReceived]),
            kTransferAmount);
  HogDbStatus<int128_t> deployed_address_balance =
      server.read_address_balance(deployed_address);
  ASSERT_TRUE(deployed_address_balance.has_value());
  EXPECT_EQ(from_xdr(deployed_hog_age->num_hogs[kHogAgeGroupReceived]),
            static_cast<uint128_t>(*deployed_address_balance));
}

TEST(Mempool, CommitBlockNotFound)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  ConsensusValue value{};
  value.proposal_header.block_height = 2;
  auto res_commit = server.mempool_coord_rpc().schedulercoord_commit_block(
      /*block_id=*/1, std::move(value))();
  EXPECT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitBlockNotFound);
}

TEST(Mempool, Validate)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  // We propose a block first and then reuse the returned [Proposal] struct
  // for the validation below. The [Proposal] struct contains some data, such
  // as the nondeterministic data, that I would prefer not to have to construct
  // myself in this test.
  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip db_tip = get_tip.db_tip();
  auto res_nominate = propose_block(server, db_tip);
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);

  // We reuse the [Proposal] struct.
  Proposal proposal = res_nominate.proposed_block().proposal;
  {
    get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
    EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    db_tip = get_tip.db_tip();
    EXPECT_EQ(db_tip.value.proposal_header.block_height, 1);
  }
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate_block(db_tip.id,
                                                               proposal)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationOK);
}

TEST(Mempool, RejectsMismatchedConfigHash)
{
  MempoolTestInfra server;
  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);

  SchedulerProposalResult nominated = propose_block(server, get_tip.db_tip());
  ASSERT_EQ(nominated.outcome(), SchedulerProposalOutcome::SchedulerProposalOK);
  Proposal proposal = nominated.proposed_block().proposal;
  const ProposalHeader original_header = proposal.header;
  proposal.header.config_hash[0] ^= 1;
  EXPECT_NE(sha256(original_header), sha256(proposal.header));

  SchedulerValidationResult validated =
      server.mempool_coord_rpc().schedulercoord_validate_block(
          get_tip.db_tip().id, std::move(proposal))();
  EXPECT_EQ(validated.outcome(),
            SchedulerValidationOutcome::SchedulerValidationInvalid);
}

TEST(Mempool, ValidateMalformedProposalDecodeReturnsInvalid)
{
  MempoolTestInfra server;

  TransactionHashBatch tx_hashes{TransactionHash{}};
  x23::XdrVec<NondeterministicProposalData> tx_nondeterministic_data{
      NondeterministicProposalData{}};

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip db_tip = get_tip.db_tip();
  Proposal proposal{
      .header = {.ext = {},
                 .network_hash = MempoolTestInfra::kNetworkHash,
                 .block_height = db_tip.value.proposal_header.block_height + 1},
      .transactions = {.ext = {},
                       .hash = t_to_parallel_batch(tx_hashes)(),
                       .nondeterministic_data =
                           t_to_parallel_batch(tx_nondeterministic_data)()}};

  ASSERT_EQ(proposal.transactions.hash.segment_tokens.size(), 1UL);
  ASSERT_EQ(proposal.transactions.nondeterministic_data.segment_tokens.size(),
            1UL);
  proposal.transactions.hash.segment_tokens[0]++;
  proposal.transactions.nondeterministic_data.segment_tokens[0]++;

  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate_block(db_tip.id,
                                                               proposal)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationMalformed);
}

TEST(Mempool, ValidateMismatchedProposalCountsReturnsInvalid)
{
  MempoolTestInfra server;

  TransactionHashBatch tx_hashes{TransactionHash{}, TransactionHash{}};
  x23::XdrVec<NondeterministicProposalData> tx_nondeterministic_data{
      NondeterministicProposalData{}};

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip db_tip = get_tip.db_tip();
  Proposal proposal{
      .header = {.ext = {},
                 .network_hash = MempoolTestInfra::kNetworkHash,
                 .block_height = db_tip.value.proposal_header.block_height + 1},
      .transactions = {.ext = {},
                       .hash = t_to_parallel_batch(tx_hashes)(),
                       .nondeterministic_data =
                           t_to_parallel_batch(tx_nondeterministic_data)()}};

  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate_block(db_tip.id,
                                                               proposal)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationMalformed);
}

namespace {

// Repeats [one_element_batch]'s sole element [count] times into one segment.
// The result is self-consistent, so only an explicit size bound rejects it.
template<typename Batch>
ParallelBatch
repeated_parallel_batch(const Batch &one_element_batch, size_t count)
{
  const ParallelBatch one = t_to_parallel_batch(one_element_batch)();
  EXPECT_EQ(one.segment_tokens.size(), 1UL);
  EXPECT_EQ(one.segment_tokens[0], 1U);
  const size_t element_size = one.xdr.size();
  const size_t total_bytes = element_size * count;

  ParallelBatch batch;
  batch.segment_tokens.push_back(static_cast<unsigned int>(count));
  batch.segment_bytes.push_back(static_cast<unsigned int>(total_bytes));
  batch.xdr.resize(total_bytes, 0);
  for (size_t i = 0; i < count; i++) {
    std::memcpy(&batch.xdr[i * element_size], one.xdr.data(), element_size);
  }
  return batch;
}

} // namespace

// Both batches decode cleanly, so without the bound validation would fan
// [kMaxBlockSize] + 1 transactions out across the executors.
TEST(Mempool, ValidateOversizedProposalIsRejected)
{
  MempoolTestInfra server;

  constexpr size_t kOverCapCount = MempoolCoordinatorRpc::kMaxBlockSize + 1;

  const TransactionHashBatch one_hash{TransactionHash{}};
  const x23::XdrVec<NondeterministicProposalData> one_nomination{
      NondeterministicProposalData{}};
  ParallelBatch hashes = repeated_parallel_batch(one_hash, kOverCapCount);
  ParallelBatch nominations =
      repeated_parallel_batch(one_nomination, kOverCapCount);

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip db_tip = get_tip.db_tip();
  Proposal proposal{
      .header = {.ext = {},
                 .network_hash = MempoolTestInfra::kNetworkHash,
                 .block_height = db_tip.value.proposal_header.block_height + 1},
      .transactions = {.ext = {},
                       .hash = std::move(hashes),
                       .nondeterministic_data = std::move(nominations)}};

  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate_block(db_tip.id,
                                                               proposal)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationMalformed);
}

TEST(Mempool, ValidateBatch)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                          t_to_parallel_batch(
                                                              txs)())();

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip db_tip = get_tip.db_tip();
  auto res_nominate = propose_block(server, db_tip);
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);

  // We reuse the [Proposal] struct.
  Proposal proposal = res_nominate.proposed_block().proposal;
  {
    get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
    EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    db_tip = get_tip.db_tip();
    EXPECT_EQ(db_tip.value.proposal_header.block_height, 1);
  }

  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                          t_to_parallel_batch(
                                                              txs)())();
  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate_block(db_tip.id,
                                                               proposal)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationOK);
}

TEST(Mempool, ValidateRejectsBlockWithFailedTx)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip db_tip = get_tip.db_tip();
  SchedulerProposalResult res_nominate = propose_block(server, db_tip);
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);

  SchedulerCommitOutcome res_commit =
      commit_block(server, db_tip, res_nominate.proposed_block());
  ASSERT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);

  Proposal proposal = res_nominate.proposed_block().proposal;
  proposal.header.block_height++;
  {
    get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
    ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    db_tip = get_tip.db_tip();
    ASSERT_EQ(db_tip.value.proposal_header.block_height, 2);
  }

  // Submit the same tx again so that hog_db detects the duplicate tx and
  // hog_exec fails the tx.
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();

  SchedulerValidationResult res_validate =
      server.mempool_coord_rpc().schedulercoord_validate_block(db_tip.id,
                                                               proposal)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationInvalid);
}

TEST(Mempool, ValidateEmpty)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);

  MempoolTestInfra server;

  const SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);

  Proposal proposal{};
  proposal.header.block_height =
      get_tip.db_tip().value.proposal_header.block_height + 1;
  auto res_validate = server.mempool_coord_rpc().schedulercoord_validate_block(
      get_tip.db_tip().id, std::move(proposal))();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationInvalid);
}

TEST(Mempool, ValidateWrongTxsHash)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip db_tip = get_tip.db_tip();
  auto res_nominate = propose_block(server, db_tip);
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);

  auto res_commit = commit_block(server, db_tip, res_nominate.proposed_block());
  EXPECT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);

  Proposal proposal = res_nominate.proposed_block().proposal;
  proposal.header.block_height++;
  {
    get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
    EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    db_tip = get_tip.db_tip();
    EXPECT_EQ(db_tip.value.proposal_header.block_height, 2);

    proposal.header.txs_hash[0] ^= 0xff;
  }
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate_block(db_tip.id,
                                                               proposal)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationInvalid);
}

TEST(Mempool, ValidateBatchRemoveTxFromProposal)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                          t_to_parallel_batch(
                                                              txs)())();

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip db_tip = get_tip.db_tip();
  auto res_nominate = propose_block(server, db_tip);
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);

  auto res_commit = commit_block(server, db_tip, res_nominate.proposed_block());
  EXPECT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);

  // We reuse the [Proposal] struct and make some adjustments for the
  // validation.
  Proposal proposal = res_nominate.proposed_block().proposal;
  proposal.header.block_height++;
  {
    TransactionHashBatch tx_hashes;
    EXPECT_TRUE(parallel_batch_to_t(proposal.transactions.hash, tx_hashes,
                                    kNoBatchSizeLimit)());

    get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
    EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    db_tip = get_tip.db_tip();
    EXPECT_EQ(db_tip.value.proposal_header.block_height, 2);

    // Remove a transaction from the proposal.
    tx_hashes.pop_back();
    x23::XdrVec<NondeterministicProposalData> tx_nondeterministic_data;
    EXPECT_TRUE(parallel_batch_to_t(proposal.transactions.nondeterministic_data,
                                    tx_nondeterministic_data,
                                    kNoBatchSizeLimit)());
    tx_nondeterministic_data.pop_back();

    proposal.transactions.hash = t_to_parallel_batch(tx_hashes)();
    proposal.transactions.nondeterministic_data =
        t_to_parallel_batch(tx_nondeterministic_data)();
  }

  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                          t_to_parallel_batch(
                                                              txs)())();
  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate_block(db_tip.id,
                                                               proposal)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationInvalid);
}

TEST(Mempool, ValidateBatchMissingTx)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, /*num_txs=*/5);

  MempoolTestInfra server;

  ValidatorID validator_id{};
  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip db_tip = get_tip.db_tip();
  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                          t_to_parallel_batch(
                                                              txs)())();

  SchedulerProposalResult res_nominate = propose_block(server, db_tip);
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);

  auto res_commit = commit_block(server, db_tip, res_nominate.proposed_block());
  EXPECT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);
  Proposal proposal = res_nominate.proposed_block().proposal;
  proposal.header.block_height++;
  {
    TransactionHashBatch tx_hashes;
    EXPECT_TRUE(parallel_batch_to_t(proposal.transactions.hash, tx_hashes,
                                    kNoBatchSizeLimit)());

    get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
    EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    db_tip = get_tip.db_tip();
    EXPECT_EQ(db_tip.value.proposal_header.block_height, 2);
  }

  // Remove a transaction.
  txs.pop_back();

  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                          t_to_parallel_batch(
                                                              txs)())();

  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate_block(db_tip.id,
                                                               proposal)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationMissingTxs);
}

TEST(Mempool, ValidateCommit)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  // We propose a block first and then reuse the returned [Proposal] struct
  // for the validation below. The [Proposal] struct contains some data,
  // such as the nondeterministic data, that I would prefer not to have to
  // construct myself in this test.
  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip db_tip = get_tip.db_tip();
  auto res_nominate = propose_block(server, db_tip);
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);

  // We reuse the [Proposal] struct.
  Proposal proposal = res_nominate.proposed_block().proposal;
  {
    get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
    EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    db_tip = get_tip.db_tip();
    EXPECT_EQ(db_tip.value.proposal_header.block_height, 1);
  }
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate_block(db_tip.id,
                                                               proposal)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationOK);

  auto res_commit =
      commit_block(server, res_validate.block().block_id, db_tip, proposal);
  EXPECT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);
}

// Regression test for a validation-gate leak: the
// [SchedulerValidationUnknownParentBlock] early return in
// [schedulercoord_validate_block()] used to skip
// [decrement_num_validations_in_progress()], leaking the gate increment.
// [close_validation_gate()] then waits forever for the count to reach zero,
// so every subsequent [schedulercoord_commit_block()] wedges. (The bench
// harness maps a timeout to success, so a wedged validator is silently
// masked in CI.) Without the fix this test HANGS at the commit below; with
// it, it completes in well under a second.
TEST(Mempool, UnknownParentBlockReleasesValidationGate)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 32);

  MempoolTestInfra server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(validator_id,
                                                          t_to_parallel_batch(
                                                              txs)())();
  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  const DbTip db_tip = get_tip.db_tip();
  auto res_nominate = propose_block(server, db_tip);
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);
  const Proposal proposal = res_nominate.proposed_block().proposal;

  // Validate against a parent id that is not the last committed block.
  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate_block(db_tip.id + 1,
                                                               proposal)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationUnknownParentBlock);

  // The commit must not wedge on a leaked validation-gate increment.
  auto res_commit = commit_block(server, res_nominate.proposed_block().block_id,
                                 db_tip, proposal);
  EXPECT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);
}

TEST(Mempool, ProposalUsesTransactionRoot)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  server.mempool_coord_rpc().schedulercoord_submit_unvetted(
      t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  const DbTip parent_tip = get_tip.db_tip();

  SchedulerProposalResult res_nominate = propose_block(server, parent_tip);
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);
  const Proposal proposal = res_nominate.proposed_block().proposal;

  SchedulerCommitOutcome res_commit =
      commit_block(server, parent_tip, res_nominate.proposed_block());
  ASSERT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);

  get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  TransactionHashBatch tx_hashes;
  ASSERT_TRUE(parallel_batch_to_t(proposal.transactions.hash, tx_hashes,
                                  kNoBatchSizeLimit)());
  EXPECT_EQ(proposal.header.txs_hash,
            MempoolCoordinatorRpc::get_txset_hash(tx_hashes));
}

TEST(Mempool, RejectsABlockWhoseTimepointPrecedesTheTip)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;
  server.mempool_coord_rpc().schedulercoord_submit_unvetted(
      t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  const DbTip parent_tip = get_tip.db_tip();

  // Commit a block carrying a timepoint, so the tip has one to be behind.
  constexpr TimePoint kTipTimepoint = 1'000'000;
  SchedulerProposalResult res_propose =
      propose_block(server, parent_tip,
                    ProposalContext{.block_timepoint = kTipTimepoint});
  ASSERT_EQ(res_propose.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);
  const Proposal committed = res_propose.proposed_block().proposal;
  ASSERT_EQ(committed.header.block_timepoint, kTipTimepoint);
  ASSERT_EQ(commit_block(server, parent_tip, res_propose.proposed_block()),
            SchedulerCommitOutcome::SchedulerCommitOK);

  get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip tip = get_tip.db_tip();
  ASSERT_EQ(tip.value.proposal_header.block_timepoint, kTipTimepoint);

  // Nominating behind the tip is refused.
  EXPECT_EQ(propose_block(server, tip,
                          ProposalContext{.block_timepoint = kTipTimepoint - 1})
                .outcome(),
            SchedulerProposalOutcome::SchedulerProposalError);

  // Equal is allowed: two blocks sharing a timepoint is implausible, not
  // incoherent. This also leaves the mempool able to nominate afterwards,
  // showing the refusal above did not wedge it.
  SchedulerProposalResult equal_timepoint =
      propose_block(server, tip,
                    ProposalContext{.block_timepoint = kTipTimepoint});
  ASSERT_EQ(equal_timepoint.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);
  // Commit the nomination.
  ASSERT_EQ(commit_block(server, tip, equal_timepoint.proposed_block()),
            SchedulerCommitOutcome::SchedulerCommitOK);

  get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  tip = get_tip.db_tip();
  ASSERT_EQ(tip.value.proposal_header.block_timepoint, kTipTimepoint);

  // Greater than is allowed.
  SchedulerProposalResult greater =
      propose_block(server, tip,
                    ProposalContext{.block_timepoint = kTipTimepoint + 1});
  ASSERT_EQ(greater.outcome(), SchedulerProposalOutcome::SchedulerProposalOK);

  // Proposing and validating do not advance the committed tip.
  // A peer's proposal behind the tip is invalid.
  Proposal backwards = greater.proposed_block().proposal;
  backwards.header.block_timepoint = kTipTimepoint - 1;
  EXPECT_EQ(server.mempool_coord_rpc()
                .schedulercoord_validate_block(tip.id, std::move(backwards))()
                .outcome(),
            SchedulerValidationOutcome::SchedulerValidationInvalid);

  // A peer's proposal equal to the tip is allowed.
  Proposal equal = greater.proposed_block().proposal;
  equal.header.block_timepoint = kTipTimepoint;
  EXPECT_EQ(server.mempool_coord_rpc()
                .schedulercoord_validate_block(tip.id, std::move(equal))()
                .outcome(),
            SchedulerValidationOutcome::SchedulerValidationOK);

  // A peer's proposal greater than the tip is allowed.
  Proposal greater2 = greater.proposed_block().proposal;
  greater2.header.block_timepoint = kTipTimepoint + 2;
  EXPECT_EQ(server.mempool_coord_rpc()
                .schedulercoord_validate_block(tip.id, std::move(greater2))()
                .outcome(),
            SchedulerValidationOutcome::SchedulerValidationOK);
}

TEST(Mempool, ProposeBlockReturnsClosedRootsAndContext)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  server.mempool_coord_rpc().schedulercoord_submit_unvetted(
      t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  const DbTip parent_tip = get_tip.db_tip();

  ProposalContext unsupported_context{.protocol = 1};
  EXPECT_EQ(server.mempool_coord_rpc()
                .schedulercoord_propose_block(parent_tip.id,
                                              unsupported_context)()
                .outcome(),
            SchedulerProposalOutcome::SchedulerProposalError);

  ProposalContext context{.proposer = make_validator_id(0x42),
                          .block_timepoint = 123456789};
  SchedulerProposalResult res_propose =
      server.mempool_coord_rpc().schedulercoord_propose_block(parent_tip.id,
                                                              context)();
  ASSERT_EQ(res_propose.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);
  const SchedulerProposedBlock &proposed = res_propose.proposed_block();

  EXPECT_EQ(proposed.proposal.header.proposer, context.proposer);
  EXPECT_EQ(proposed.proposal.header.block_timepoint, context.block_timepoint);
  EXPECT_NE(proposed.proposal.header.storage_hash, StorageShortHash{});

  ConsensusValue value = consensus_value(parent_tip, proposed.proposal);
  SchedulerCommitOutcome res_commit =
      server.mempool_coord_rpc().schedulercoord_commit_block(proposed.block_id,
                                                             value)();
  ASSERT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);
  auto forwarded_consensus =
      server.event_sched_rpc().get_committed_consensus_value(
          proposed.block_id)();
  if (!forwarded_consensus.has_value()) {
    ADD_FAILURE() << "Consensus value was not forwarded";
    return;
  }
  EXPECT_EQ(x23::serialize(forwarded_consensus.value()), x23::serialize(value));

  get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  EXPECT_EQ(get_tip.db_tip().value.proposal_header, proposed.proposal.header);
}

TEST(Mempool, EmptyProposalForwardsConsensusValue)
{
  MempoolTestInfra server;
  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  const DbTip parent_tip = get_tip.db_tip();

  SchedulerProposalResult res_propose =
      server.mempool_coord_rpc().schedulercoord_propose_block(
          parent_tip.id, ProposalContext{})();
  ASSERT_EQ(res_propose.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);
  const SchedulerProposedBlock &proposed = res_propose.proposed_block();
  EXPECT_TRUE(proposed.proposal.transactions.hash.xdr.empty());
  EXPECT_TRUE(proposed.proposal.transactions.nondeterministic_data.xdr.empty());

  ConsensusValue value = consensus_value(parent_tip, proposed.proposal);
  ASSERT_EQ(server.mempool_coord_rpc().schedulercoord_commit_block(
                proposed.block_id, value)(),
            SchedulerCommitOutcome::SchedulerCommitOK);
  auto forwarded_consensus =
      server.event_sched_rpc().get_committed_consensus_value(
          proposed.block_id)();
  if (!forwarded_consensus.has_value()) {
    ADD_FAILURE() << "Consensus value was not forwarded";
    return;
  }
  EXPECT_EQ(x23::serialize(forwarded_consensus.value()), x23::serialize(value));
}

TEST(Mempool, CommitBlockRejectsMismatchedConsensusValue)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  server.mempool_coord_rpc().schedulercoord_submit_unvetted(
      t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  const DbTip parent_tip = get_tip.db_tip();

  SchedulerProposalResult res_propose =
      server.mempool_coord_rpc().schedulercoord_propose_block(
          parent_tip.id, ProposalContext{})();
  ASSERT_EQ(res_propose.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);
  const SchedulerProposedBlock &proposed = res_propose.proposed_block();

  const ConsensusValue expected =
      consensus_value(parent_tip, proposed.proposal);
  auto expect_rejected = [&](ConsensusValue value) {
    EXPECT_EQ(server.mempool_coord_rpc().schedulercoord_commit_block(
                  proposed.block_id, std::move(value))(),
              SchedulerCommitOutcome::SchedulerCommitInvalidConsensusValue);
  };

  ConsensusValue wrong_header = expected;
  wrong_header.proposal_header.events_hash[0] ^= 0xff;
  expect_rejected(std::move(wrong_header));

  ConsensusValue wrong_protocol = expected;
  wrong_protocol.protocol = 1;
  expect_rejected(std::move(wrong_protocol));

  ConsensusValue unexpected_upgrade = expected;
  unexpected_upgrade.upgrades.push_back(Upgrade{1});
  expect_rejected(std::move(unexpected_upgrade));

  EXPECT_EQ(server.mempool_coord_rpc().schedulercoord_commit_block(
                proposed.block_id, expected)(),
            SchedulerCommitOutcome::SchedulerCommitOK);

  get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  EXPECT_EQ(get_tip.db_tip().id, proposed.block_id);
  EXPECT_EQ(get_tip.db_tip().value.proposal_header.block_height,
            proposed.proposal.header.block_height);
}

TEST(Mempool, ValidateBlockRejectsTamperedHeaderRoots)
{
  auto expect_tampered_root_rejected = [](auto tamper) {
    fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
    const SignedTransactionBatch txs =
        generate_unsigned_txs(fib_client, kNumTxs);

    MempoolTestInfra server;

    server.mempool_coord_rpc().schedulercoord_submit_unvetted(
        t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();

    SchedulerGetTipResult get_tip =
        server.mempool_coord_rpc().schedcoord_get_tip()();
    ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    const DbTip parent_tip = get_tip.db_tip();

    SchedulerProposalResult res_propose =
        server.mempool_coord_rpc().schedulercoord_propose_block(
            parent_tip.id, ProposalContext{})();
    ASSERT_EQ(res_propose.outcome(),
              SchedulerProposalOutcome::SchedulerProposalOK);
    Proposal proposal = res_propose.proposed_block().proposal;
    tamper(proposal.header);

    server.mempool_coord_rpc().schedulercoord_submit_unvetted(
        t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();

    SchedulerValidationResult res_validate =
        server.mempool_coord_rpc().schedulercoord_validate_block(
            parent_tip.id, std::move(proposal))();
    EXPECT_EQ(res_validate.outcome(),
              SchedulerValidationResult::SchedulerValidationInvalid);
  };

  expect_tampered_root_rejected(
      [](ProposalHeader &header) { header.storage_hash[0] ^= 0xff; });
  expect_tampered_root_rejected(
      [](ProposalHeader &header) { header.events_hash[0] ^= 0xff; });
}

TEST(Mempool, ValidateTwo)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  // We propose a block first and then reuse the returned [Proposal] struct
  // for the validation below. The [Proposal] struct contains some data, such
  // as the nondeterministic data, that I would prefer not to have to construct
  // myself in this test.
  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip db_tip = get_tip.db_tip();
  auto res_nominate = propose_block(server, db_tip);
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);

  // We reuse the [Proposal] struct.
  Proposal proposal = res_nominate.proposed_block().proposal;
  {
    get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
    EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    db_tip = get_tip.db_tip();
    EXPECT_EQ(db_tip.value.proposal_header.block_height, 1);
  }
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate_block(db_tip.id,
                                                               proposal)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationOK);

  auto res_commit =
      commit_block(server, res_validate.block().block_id, db_tip, proposal);
  EXPECT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);

  // Now propose another block.
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();

  get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  db_tip = get_tip.db_tip();
  EXPECT_EQ(db_tip.value.proposal_header.block_height, 2);

  res_nominate = propose_block(server, db_tip);
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);

  // Now validate another block.
  proposal = res_nominate.proposed_block().proposal;
  {
    get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
    EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    db_tip = get_tip.db_tip();
    EXPECT_EQ(db_tip.value.proposal_header.block_height, 2);
  }
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  res_validate =
      server.mempool_coord_rpc().schedulercoord_validate_block(db_tip.id,
                                                               proposal)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationOK);
  res_commit =
      commit_block(server, res_validate.block().block_id, db_tip, proposal);
  EXPECT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);

  get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  db_tip = get_tip.db_tip();
  EXPECT_EQ(db_tip.id, res_validate.block().block_id);
  EXPECT_EQ(db_tip.value.proposal_header.block_height, 3);
}

TEST(Mempool, GetTip)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip db_tip = get_tip.db_tip();
  EXPECT_EQ(db_tip.value.proposal_header.block_height, 1);
  auto event_tip = Registration::registration().event_canonical_tip();
  if (!event_tip.has_value()) {
    ADD_FAILURE() << "Event canonical tip was not published";
    return;
  }
  const EventCanonicalTip &initial_event_tip = event_tip.value();
  EXPECT_EQ(initial_event_tip.block_id, db_tip.id);
  EXPECT_EQ(x23::serialize(initial_event_tip.consensus_value),
            x23::serialize(db_tip.value));

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  // We propose a block first and then reuse the returned [Proposal] struct
  // for the validation below. The [Proposal] struct contains some data,
  // such as the nondeterministic data, that I would prefer not to have to
  // construct myself in this test.
  auto res_nominate = propose_block(server, db_tip);
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);

  // We reuse the [Proposal] struct.
  Proposal proposal = res_nominate.proposed_block().proposal;
  {
    get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
    EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    db_tip = get_tip.db_tip();
    EXPECT_EQ(db_tip.value.proposal_header.block_height, 1);
  }
  // The nominated block above also contains [txs[0]], we did not commit the
  // block, so [txs[0]] should not be detected as a duplicate.
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  auto res_validate =
      server.mempool_coord_rpc().schedulercoord_validate_block(db_tip.id,
                                                               proposal)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationResult::SchedulerValidationOK);

  get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  db_tip = get_tip.db_tip();
  EXPECT_EQ(db_tip.value.proposal_header.block_height, 1);

  auto res_commit =
      commit_block(server, res_validate.block().block_id, db_tip, proposal);
  EXPECT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);

  get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
  EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  db_tip = get_tip.db_tip();
  EXPECT_EQ(db_tip.id, res_validate.block().block_id);
  EXPECT_EQ(db_tip.value.proposal_header.block_height, 2);
  event_tip = Registration::registration().event_canonical_tip();
  if (!event_tip.has_value()) {
    ADD_FAILURE() << "Event canonical tip was not updated";
    return;
  }
  const EventCanonicalTip &committed_event_tip = event_tip.value();
  EXPECT_EQ(committed_event_tip.block_id, db_tip.id);
  EXPECT_EQ(x23::serialize(committed_event_tip.consensus_value),
            x23::serialize(db_tip.value));
}

TEST(Mempool, GenerateBlockId)
{
  uint64_t first = MempoolCoordinatorRpc::generate_block_id();
  EXPECT_NE(first, 0UL);

  uint64_t second = MempoolCoordinatorRpc::generate_block_id();
  EXPECT_NE(second, 0UL);
  EXPECT_NE(first, second);
}

TEST(Mempool, AuthChangedEvictsInvalidTxs)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 1);

  MempoolTestInfra server;

  AddressAuthData auth_data = get_default_account();
  const Ed25519SecretKey private_key =
      create_key_pair(auth_data, /*idx=*/0, /*weight=*/1);
  auth_data.last_written_height = 1;
  auth_data.required_weight = 1;

  server.set_address_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);
  Transaction &tx = txs[0].tx;
  tx.auth_claim = make_sign_claim(0);

  SecretKey sk(CryptographicScheme::Ed25519);
  sk.ed25519() = private_key;
  txs[0].signatures.push_back(make_signature(sk, tx, /*public_key_id=*/0,
                                             MempoolTestInfra::kNetworkHash));

  // Submit the tx — should pass auth (weight 1 >= required 1)
  server.mempool_coord_rpc().schedulercoord_submit_unvetted(
      t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 1UL);

  // Now raise the required_weight to 10 — the tx (weight 1) no longer qualifies
  auth_data.required_weight = 10;
  auth_data.last_written_height = 2;
  server.set_address_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);
  KeyChangeBatch key_changes_update = {
      {.address = MempoolTestInfra::kAccountAddresses[0],
       .auth_data = x23::Ptr<AddressAuthData>(auth_data)}};

  server.register_auth_data_updates(2, std::move(key_changes_update))();

  // The auth_data_changed callback should re-verify and evict the tx
  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 0UL);
}

TEST(Mempool, FailedAuthenticationDoesNotInvalidateBlocks)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 1);

  MempoolTestInfra server;

  AddressAuthData auth_data = get_default_account();
  create_key_pair(auth_data, /*idx=*/0, /*weight=*/1);
  auth_data.last_written_height = 0;
  auth_data.required_weight = 1;
  server.set_address_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);

  txs[0].tx.auth_claim = make_sign_claim(0);
  const TransactionHash tx_hash = sha256(txs[0].tx);

  server.mempool_coord_rpc().schedulercoord_submit_unvetted(
      t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();
  ASSERT_EQ(server.mempool_coord_rpc().scheduler()->size(), 0UL);

  auto expect_validation_outcome = [&](auto expected_outcome) {
    SchedulerGetTipResult get_tip =
        server.mempool_coord_rpc().schedcoord_get_tip()();
    EXPECT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);

    TransactionHashBatch tx_hashes{tx_hash};
    x23::XdrVec<NondeterministicProposalData> tx_nondeterministic_data{
        NondeterministicProposalData{}};
    Proposal proposal{
        .header = {.ext = {},
                   .network_hash = MempoolTestInfra::kNetworkHash,
                   .block_height =
                       get_tip.db_tip().value.proposal_header.block_height + 1},
        .transactions = {.ext = {},
                         .hash = t_to_parallel_batch(tx_hashes)(),
                         .nondeterministic_data =
                             t_to_parallel_batch(tx_nondeterministic_data)()}};

    SchedulerValidationResult res_validate =
        server.mempool_coord_rpc().schedulercoord_validate_block(
            get_tip.db_tip().id, proposal)();
    EXPECT_EQ(res_validate.outcome(), expected_outcome);
    return res_validate;
  };

  // A local auth failure says nothing about the signatures a proposer holds.
  // [TransactionHash] covers [Transaction] and deliberately excludes
  // signatures, so a transaction we could not authenticate may be correctly
  // signed elsewhere on the network. Validation must ask the coordinator for
  // the authoritative [SignedTransaction] instead of voting the block invalid,
  // and must keep doing so however many blocks elapse.
  for (size_t blocks_elapsed = 0; blocks_elapsed < 5; blocks_elapsed++) {
    SchedulerValidationResult res_missing = expect_validation_outcome(
        SchedulerValidationResult::SchedulerValidationMissingTxs);
    ASSERT_EQ(res_missing.missing_tx_hashes().size(), 1UL);
    EXPECT_EQ(res_missing.missing_tx_hashes()[0], tx_hash);

    // Now commit an empty block.
    SchedulerGetTipResult get_tip =
        server.mempool_coord_rpc().schedcoord_get_tip()();
    ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);

    SchedulerProposalResult res_nominate =
        propose_block(server, get_tip.db_tip());
    ASSERT_EQ(res_nominate.outcome(),
              SchedulerProposalOutcome::SchedulerProposalOK);

    SchedulerCommitOutcome res_commit =
        commit_block(server, get_tip.db_tip(), res_nominate.proposed_block());
    ASSERT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);
  }

  SchedulerValidationResult res_validate = expect_validation_outcome(
      SchedulerValidationResult::SchedulerValidationMissingTxs);
  ASSERT_EQ(res_validate.missing_tx_hashes().size(), 1UL);
  EXPECT_EQ(res_validate.missing_tx_hashes()[0], tx_hash);
}

// [TransactionHash] hashes [Transaction] and deliberately excludes signatures
// so that a third party cannot change an in-flight transaction's identity. A
// failed authentication is therefore a verdict about particular signature
// bytes, while the hash naming it covers every signing of that transaction.
// An attacker who takes a validly signed transaction, corrupts its signatures
// and submits the result must not be able to move this node's verdict on a
// block carrying the genuine transaction.
TEST(Mempool, CorruptedSignatureSubmitCannotPoisonValidation)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 1);

  MempoolTestInfra server;

  AddressAuthData auth_data = get_default_account();
  const Ed25519SecretKey private_key =
      create_key_pair(auth_data, /*idx=*/0, /*weight=*/1);
  auth_data.last_written_height = 1;
  auth_data.required_weight = 1;
  server.set_address_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);

  Transaction &tx = txs[0].tx;
  tx.auth_claim = make_sign_claim(0);

  SecretKey sk(CryptographicScheme::Ed25519);
  sk.ed25519() = private_key;

  // The genuine transaction, correctly signed.
  SignedTransaction good_tx = txs[0];
  good_tx.signatures.push_back(make_signature(sk, tx, /*public_key_id=*/0,
                                              MempoolTestInfra::kNetworkHash));

  // The attacker's copy: identical [Transaction], corrupted signature bytes.
  // The two share a [TransactionHash], which is what makes the attack possible.
  SignedTransaction poisoned_tx = txs[0];
  DecoratedSignature bad_signature =
      make_signature(sk, tx, /*public_key_id=*/0,
                     MempoolTestInfra::kNetworkHash);
  bad_signature.signature.ed25519()[0] ^= 0xff;
  poisoned_tx.signatures.push_back(bad_signature);

  const TransactionHash tx_hash = sha256(tx);
  ASSERT_EQ(sha256(poisoned_tx.tx), tx_hash);

  // The attacker submits the corrupted copy over the unauthenticated path. It
  // must fail authentication and must not be admitted.
  server.mempool_coord_rpc().schedulercoord_submit_unvetted(
      t_to_parallel_batch(SignedTransactionBatch{poisoned_tx})())();
  ASSERT_EQ(server.mempool_coord_rpc().scheduler()->size(), 0UL);

  // A proposal carrying the genuine transaction must come back as MissingTxs,
  // so the coordinator is asked for the authoritative [SignedTransaction].
  // Voting Invalid here would reject a perfectly good block on the strength of
  // signature bytes this node was handed by an anonymous submitter.
  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);

  TransactionHashBatch tx_hashes{tx_hash};
  x23::XdrVec<NondeterministicProposalData> tx_nondeterministic_data{
      NondeterministicProposalData{}};
  Proposal poisoned_proposal{
      .header = {.ext = {},
                 .network_hash = MempoolTestInfra::kNetworkHash,
                 .block_height =
                     get_tip.db_tip().value.proposal_header.block_height + 1},
      .transactions = {.ext = {},
                       .hash = t_to_parallel_batch(tx_hashes)(),
                       .nondeterministic_data =
                           t_to_parallel_batch(tx_nondeterministic_data)()}};

  SchedulerValidationResult res_poisoned =
      server.mempool_coord_rpc().schedulercoord_validate_block(
          get_tip.db_tip().id, poisoned_proposal)();
  ASSERT_EQ(res_poisoned.outcome(),
            SchedulerValidationResult::SchedulerValidationMissingTxs);
  ASSERT_EQ(res_poisoned.missing_tx_hashes().size(), 1UL);
  EXPECT_EQ(res_poisoned.missing_tx_hashes()[0], tx_hash);

  // Once the genuine transaction arrives the same hash validates normally: the
  // earlier authentication failure left no residue behind.
  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{good_tx})())();
  ASSERT_EQ(server.mempool_coord_rpc().scheduler()->size(), 1UL);

  SchedulerProposalResult res_nominate =
      propose_block(server, get_tip.db_tip());
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);
  Proposal good_proposal = res_nominate.proposed_block().proposal;

  get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{good_tx})())();

  SchedulerValidationResult res_good =
      server.mempool_coord_rpc().schedulercoord_validate_block(
          get_tip.db_tip().id, good_proposal)();
  EXPECT_EQ(res_good.outcome(),
            SchedulerValidationResult::SchedulerValidationOK);
}

namespace {

struct NamedSigning {
  const char *name;
  SignedTransaction signed_tx;
};

// Signings that share [base_tx] -- and therefore its [TransactionHash] -- but
// that authentication must reject. Between them they cover what someone who
// does not hold the signing key can produce (corrupt, absent, borrowed from
// another key, another transaction, or another network) and the orderings the
// auth-claim rules turn down as malformed.
//
// [key_id_0_sk] signs as registered key 0. [unrelated_sk] is a key the account
// has never registered.
inline std::vector<NamedSigning>
rejectable_signings(const Transaction &base_tx, const SecretKey &key_id_0_sk,
                    const SecretKey &unrelated_sk,
                    const NetworkHash &network_hash)
{
  const DecoratedSignature good =
      make_signature(key_id_0_sk, base_tx, /*public_key_id=*/0, network_hash);

  auto signing = [&base_tx](std::initializer_list<DecoratedSignature> sigs) {
    SignedTransaction out;
    out.tx = base_tx;
    for (const DecoratedSignature &sig : sigs) {
      out.signatures.push_back(sig);
    }
    return out;
  };

  DecoratedSignature flipped = good;
  flipped.signature.ed25519()[0] ^= 0xff;

  DecoratedSignature zeroed = good;
  for (auto &byte : zeroed.signature.ed25519()) {
    byte = 0;
  }

  // A real signature by a key the account does not list.
  const DecoratedSignature unrelated_key =
      make_signature(unrelated_sk, base_tx, /*public_key_id=*/0, network_hash);

  // A real signature by the right key over a different transaction.
  Transaction other_tx = base_tx;
  other_tx.uniquifier.nonce = base_tx.uniquifier.nonce + 1;
  const DecoratedSignature other_message =
      make_signature(key_id_0_sk, other_tx, /*public_key_id=*/0, network_hash);

  // A real signature by the right key over this transaction, but bound to a
  // different network by the signature preimage.
  NetworkHash other_network = network_hash;
  other_network[0] ^= 0xff;
  const DecoratedSignature other_chain =
      make_signature(key_id_0_sk, base_tx, /*public_key_id=*/0, other_network);

  // Valid bytes attributed to a key slot the account has not registered.
  DecoratedSignature unregistered_slot = good;
  unregistered_slot.public_key_id = 1;

  return {
      {"no signatures", signing({})},
      {"bit-flipped signature", signing({flipped})},
      {"zeroed signature", signing({zeroed})},
      {"signature by an unregistered key", signing({unrelated_key})},
      {"signature over a different transaction", signing({other_message})},
      {"signature bound to a different network", signing({other_chain})},
      {"signature attributed to an unregistered slot",
       signing({unregistered_slot})},
      // Signer ids must be strictly increasing, so a repeated id is malformed
      // even though each copy verifies on its own.
      {"duplicated signer id", signing({good, good})},
      {"valid signature followed by a corrupt one", signing({good, flipped})},
  };
}

// Registers [key_id_0_sk] as key 0 with weight 1 and requires weight 1.
inline SecretKey
register_single_signer(MempoolTestInfra &server, Transaction &tx)
{
  AddressAuthData auth_data = get_default_account();
  const Ed25519SecretKey private_key =
      create_key_pair(auth_data, /*idx=*/0, /*weight=*/1);
  auth_data.last_written_height = 1;
  auth_data.required_weight = 1;
  server.set_address_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);
  tx.auth_claim = make_sign_claim(0);

  SecretKey sk(CryptographicScheme::Ed25519);
  sk.ed25519() = private_key;
  return sk;
}

inline SecretKey
unregistered_signer()
{
  AddressAuthData scratch = get_default_account();
  SecretKey sk(CryptographicScheme::Ed25519);
  sk.ed25519() = create_key_pair(scratch, /*idx=*/0, /*weight=*/1);
  return sk;
}

} // namespace

// The property the whole design rests on: [TransactionHash] covers
// [Transaction] and nothing else, so no manipulation of the signatures can
// change it. That is what makes a signature-keyed verdict stored under a
// [TransactionHash] unsound.
TEST(Mempool, TransactionHashIsIndependentOfSignatureManipulation)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 1);

  MempoolTestInfra server;
  Transaction &tx = txs[0].tx;
  const SecretKey sk = register_single_signer(server, tx);
  const SecretKey unrelated = unregistered_signer();

  SignedTransaction good_tx = txs[0];
  good_tx.signatures.push_back(make_signature(sk, tx, /*public_key_id=*/0,
                                              MempoolTestInfra::kNetworkHash));

  const TransactionHash tx_hash = sha256(tx);
  const SHA256Hash good_signing_hash = sha256(good_tx);

  for (const NamedSigning &attack :
       rejectable_signings(tx, sk, unrelated, MempoolTestInfra::kNetworkHash)) {
    SCOPED_TRACE(attack.name);
    // The manipulated signing is indistinguishable from the genuine one by
    // transaction hash ...
    EXPECT_EQ(sha256(attack.signed_tx.tx), tx_hash);
    // ... and distinguishable by the hash that covers the signatures.
    EXPECT_NE(sha256(attack.signed_tx), good_signing_hash);
  }
}

// No manipulation of a transaction's signatures may let a third party move this
// node's verdict on a block carrying the genuine transaction. Every rejected
// signing must leave validation answering MissingTxs, and the genuine
// transaction must still be admitted and validate afterwards.
TEST(Mempool, NoSignatureManipulationCanInvalidateABlock)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 1);

  MempoolTestInfra server;
  Transaction &tx = txs[0].tx;
  const SecretKey sk = register_single_signer(server, tx);
  const SecretKey unrelated = unregistered_signer();

  SignedTransaction good_tx = txs[0];
  good_tx.signatures.push_back(make_signature(sk, tx, /*public_key_id=*/0,
                                              MempoolTestInfra::kNetworkHash));
  const TransactionHash tx_hash = sha256(tx);

  // Submit every rejectable signing over the unauthenticated gateway path,
  // twice each so a resubmission of already-rejected bytes is covered too.
  // None may be admitted.
  for (const NamedSigning &attack :
       rejectable_signings(tx, sk, unrelated, MempoolTestInfra::kNetworkHash)) {
    SCOPED_TRACE(attack.name);
    for (int attempt = 0; attempt < 2; attempt++) {
      server.mempool_coord_rpc().schedulercoord_submit_unvetted(
          t_to_parallel_batch(SignedTransactionBatch{attack.signed_tx})())();
      ASSERT_EQ(server.mempool_coord_rpc().scheduler()->size(), 0UL);
    }
  }

  // After all of that, a proposal carrying the genuine transaction is still
  // only missing data -- never invalid.
  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);

  TransactionHashBatch tx_hashes{tx_hash};
  x23::XdrVec<NondeterministicProposalData> tx_nondeterministic_data{
      NondeterministicProposalData{}};
  Proposal proposal{
      .header = {.ext = {},
                 .network_hash = MempoolTestInfra::kNetworkHash,
                 .block_height =
                     get_tip.db_tip().value.proposal_header.block_height + 1},
      .transactions = {.ext = {},
                       .hash = t_to_parallel_batch(tx_hashes)(),
                       .nondeterministic_data =
                           t_to_parallel_batch(tx_nondeterministic_data)()}};

  SchedulerValidationResult res_missing =
      server.mempool_coord_rpc().schedulercoord_validate_block(
          get_tip.db_tip().id, proposal)();
  ASSERT_EQ(res_missing.outcome(),
            SchedulerValidationResult::SchedulerValidationMissingTxs);
  ASSERT_EQ(res_missing.missing_tx_hashes().size(), 1UL);
  EXPECT_EQ(res_missing.missing_tx_hashes()[0], tx_hash);

  // And the genuine signing of that same transaction is still accepted.
  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{good_tx})())();
  ASSERT_EQ(server.mempool_coord_rpc().scheduler()->size(), 1UL);

  SchedulerProposalResult res_nominate =
      propose_block(server, get_tip.db_tip());
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);
  Proposal good_proposal = res_nominate.proposed_block().proposal;

  get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{good_tx})())();

  SchedulerValidationResult res_good =
      server.mempool_coord_rpc().schedulercoord_validate_block(
          get_tip.db_tip().id, good_proposal)();
  EXPECT_EQ(res_good.outcome(),
            SchedulerValidationResult::SchedulerValidationOK);
}

// With k-of-N signers a transaction legitimately has many valid signings, so
// the distinction between "this transaction" and "these signature bytes"
// matters in normal operation and not only under attack.
TEST(Mempool, MultiSignerSigningsShareATransactionHash)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 1);

  MempoolTestInfra server;

  AddressAuthData auth_data = get_default_account();
  const Ed25519SecretKey key_0 =
      create_key_pair(auth_data, /*idx=*/0, /*weight=*/1);
  const Ed25519SecretKey key_1 =
      create_key_pair(auth_data, /*idx=*/1, /*weight=*/1);
  auth_data.last_written_height = 1;
  auth_data.required_weight = 2;
  server.set_address_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);

  Transaction &tx = txs[0].tx;
  AuthClaim claim(AuthClaimType::kSourceAddressAuthData);
  claim.source_address_claim().allowed_keys = 0b11;
  claim.source_address_claim().required_weight = 2;
  tx.auth_claim = claim;

  SecretKey sk_0(CryptographicScheme::Ed25519);
  sk_0.ed25519() = key_0;
  SecretKey sk_1(CryptographicScheme::Ed25519);
  sk_1.ed25519() = key_1;

  const DecoratedSignature sig_0 =
      make_signature(sk_0, tx, /*public_key_id=*/0,
                     MempoolTestInfra::kNetworkHash);
  const DecoratedSignature sig_1 =
      make_signature(sk_1, tx, /*public_key_id=*/1,
                     MempoolTestInfra::kNetworkHash);

  // Signer ids must be strictly increasing, so this is the one accepted
  // ordering and the reverse is malformed.
  SignedTransaction ordered = txs[0];
  ordered.signatures.push_back(sig_0);
  ordered.signatures.push_back(sig_1);

  SignedTransaction reversed = txs[0];
  reversed.signatures.push_back(sig_1);
  reversed.signatures.push_back(sig_0);

  // Only one signer: below the required weight.
  SignedTransaction below_threshold = txs[0];
  below_threshold.signatures.push_back(sig_0);

  const TransactionHash tx_hash = sha256(tx);
  EXPECT_EQ(sha256(reversed.tx), tx_hash);
  EXPECT_EQ(sha256(below_threshold.tx), tx_hash);
  EXPECT_NE(sha256(reversed), sha256(ordered));
  EXPECT_NE(sha256(below_threshold), sha256(ordered));

  // Neither rejected signing is admitted, and neither may keep the properly
  // ordered, fully weighted signing out afterwards.
  server.mempool_coord_rpc().schedulercoord_submit_unvetted(
      t_to_parallel_batch(SignedTransactionBatch{reversed})())();
  ASSERT_EQ(server.mempool_coord_rpc().scheduler()->size(), 0UL);
  server.mempool_coord_rpc().schedulercoord_submit_unvetted(
      t_to_parallel_batch(SignedTransactionBatch{below_threshold})())();
  ASSERT_EQ(server.mempool_coord_rpc().scheduler()->size(), 0UL);

  server.mempool_coord_rpc().schedulercoord_submit_unvetted(
      t_to_parallel_batch(SignedTransactionBatch{ordered})())();
  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 1UL);
}

// Driven entirely through the gateway and coordinator RPCs, with the auth
// change written by ordinary execution rather than injected: a transaction is
// refused for want of signing weight, a committed block lowers the account's
// threshold, and the same bytes are then accepted.
TEST(Mempool, RejectedTransactionAcceptedAfterCommittedAuthChange)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);

  MempoolTestInfra server;
  MempoolGatewayRpc gateway(&server.mempool_coord_rpc());

  // Two keys of weight 1 apiece, against a threshold of 2.
  AddressAuthData auth_data = get_default_account();
  const Ed25519SecretKey key_a =
      create_key_pair(auth_data, /*idx=*/0, /*weight=*/1);
  const Ed25519SecretKey key_b =
      create_key_pair(auth_data, /*idx=*/1, /*weight=*/1);
  auth_data.last_written_height = 1;
  auth_data.required_weight = 2;
  server.set_address_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);

  SecretKey sk_a(CryptographicScheme::Ed25519);
  sk_a.ed25519() = key_a;
  SecretKey sk_b(CryptographicScheme::Ed25519);
  sk_b.ed25519() = key_b;

  AuthClaim claim(AuthClaimType::kSourceAddressAuthData);
  claim.source_address_claim().allowed_keys = 0b11;
  claim.source_address_claim().required_weight = 1;

  auto submit_over_gateway = [&](const SignedTransaction &tx) {
    gateway.schedgateway_submit_tx_batch(
        t_to_parallel_batch(SignedTransactionBatch{tx})())();
  };

  // Signed by one key, so weight 1 against a threshold of 2.
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 1);
  txs[0].tx.auth_claim = claim;
  SignedTransaction underweight = txs[0];
  underweight.signatures.push_back(
      make_signature(sk_a, txs[0].tx, /*public_key_id=*/0,
                     MempoolTestInfra::kNetworkHash));

  submit_over_gateway(underweight);
  ASSERT_EQ(server.mempool_coord_rpc().scheduler()->size(), 0UL);
  // Again, rejected the same way.
  submit_over_gateway(underweight);
  ASSERT_EQ(server.mempool_coord_rpc().scheduler()->size(), 0UL);

  // A transaction that does meet the threshold, lowering it to 1.
  SignedTransactionBatch lower_txs = generate_unsigned_txs(fib_client, 1);
  lower_txs[0].tx.invocation =
      fib_client.make_invocation<u8"change_nonce_config">(
          /*gas_amount=*/5'000'000'000, uint32_t{1});
  lower_txs[0].tx.uniquifier.nonce = 99;
  lower_txs[0].tx.auth_claim = claim;
  SignedTransaction lower = lower_txs[0];
  lower.signatures.push_back(make_signature(sk_a, lower_txs[0].tx,
                                            /*public_key_id=*/0,
                                            MempoolTestInfra::kNetworkHash));
  lower.signatures.push_back(make_signature(sk_b, lower_txs[0].tx,
                                            /*public_key_id=*/1,
                                            MempoolTestInfra::kNetworkHash));

  submit_over_gateway(lower);
  ASSERT_EQ(server.mempool_coord_rpc().scheduler()->size(), 1UL);

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  SchedulerProposalResult res_nominate =
      propose_block(server, get_tip.db_tip());
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);
  ASSERT_EQ(commit_block(server, get_tip.db_tip(),
                         res_nominate.proposed_block()),
            SchedulerCommitOutcome::SchedulerCommitOK);

  // The same bytes now carry enough weight, and the earlier rejections must
  // not stand in the way.
  submit_over_gateway(underweight);
  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 1UL);
}

// A rejection is never remembered: every resubmission of a turned-down
// signing reaches authentication and is judged against the account's current
// auth data, so a signing this node turned down is re-verified -- and can be
// admitted -- once a committed block changes the account's keys.
TEST(Mempool, RejectedSigningIsReverifiedOnEveryResubmission)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 1);

  MempoolTestInfra server;

  // The account lists one key now and a different one after the change, and
  // the transaction is signed by neither. Authentication turns it down either
  // way; the auth-failure counter shows every submission reaching it.
  AddressAuthData initial_auth_data = get_default_account();
  create_key_pair(initial_auth_data, /*idx=*/0, /*weight=*/1);
  initial_auth_data.last_written_height = 1;
  initial_auth_data.required_weight = 1;
  server.set_address_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               initial_auth_data);

  AddressAuthData later_auth_data = get_default_account();
  create_key_pair(later_auth_data, /*idx=*/0, /*weight=*/1);
  later_auth_data.required_weight = 1;

  Transaction &tx = txs[0].tx;
  tx.auth_claim = make_sign_claim(0);

  const SecretKey sk = unregistered_signer();
  SignedTransaction signed_tx = txs[0];
  signed_tx.signatures.push_back(
      make_signature(sk, tx, /*public_key_id=*/0,
                     MempoolTestInfra::kNetworkHash));

  auto submit_over_gateway = [&] {
    server.mempool_coord_rpc().schedulercoord_submit_unvetted(
        t_to_parallel_batch(SignedTransactionBatch{signed_tx})())();
  };
  auto auth_failures = [] {
    return MempoolMetrics::instance().auth_failed_tx_count.value();
  };

  // Rejected against the initial auth data, and again on resubmission: each
  // attempt reaches authentication.
  const ssize_t before = auth_failures();
  submit_over_gateway();
  ASSERT_EQ(server.mempool_coord_rpc().scheduler()->size(), 0UL);
  ASSERT_EQ(auth_failures(), before + 1);
  submit_over_gateway();
  ASSERT_EQ(server.mempool_coord_rpc().scheduler()->size(), 0UL);
  ASSERT_EQ(auth_failures(), before + 2);

  // The account's keys change.
  later_auth_data.last_written_height = 2;
  server.set_address_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               later_auth_data);
  KeyChangeBatch key_changes = {
      {.address = MempoolTestInfra::kAccountAddresses[0],
       .auth_data = x23::Ptr<AddressAuthData>(later_auth_data)}};
  server.register_auth_data_updates(2, std::move(key_changes))();

  // The same bytes reach authentication again, judged against the new auth
  // data. Had this account's new keys made the signature valid, this is what
  // would let the transaction in.
  submit_over_gateway();
  EXPECT_EQ(auth_failures(), before + 3);
  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 0UL);
}

// A body supplied for a nomination is always authenticated, whatever this
// node concluded about the same bytes over the gateway: dropping one
// unexamined would keep this node from validating the block that referenced
// it. The auth-failure counter shows every path reaching authentication.
TEST(Mempool, VettedSubmissionsAreAlwaysAuthenticated)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 1);

  MempoolTestInfra server;
  Transaction &tx = txs[0].tx;
  const SecretKey sk = register_single_signer(server, tx);

  SignedTransaction poisoned_tx = txs[0];
  DecoratedSignature bad_signature =
      make_signature(sk, tx, /*public_key_id=*/0,
                     MempoolTestInfra::kNetworkHash);
  bad_signature.signature.ed25519()[0] ^= 0xff;
  poisoned_tx.signatures.push_back(bad_signature);

  auto submit_over_gateway = [&] {
    server.mempool_coord_rpc().schedulercoord_submit_unvetted(
        t_to_parallel_batch(SignedTransactionBatch{poisoned_tx})())();
  };
  auto auth_failures = [] {
    return MempoolMetrics::instance().auth_failed_tx_count.value();
  };

  // The first gateway submission reaches authentication and is turned down.
  const ssize_t before_first = auth_failures();
  submit_over_gateway();
  ASSERT_EQ(server.mempool_coord_rpc().scheduler()->size(), 0UL);
  const ssize_t after_first = auth_failures();
  ASSERT_EQ(after_first, before_first + 1);

  // So does the second: rejections are not remembered.
  submit_over_gateway();
  ASSERT_EQ(server.mempool_coord_rpc().scheduler()->size(), 0UL);
  EXPECT_EQ(auth_failures(), after_first + 1);

  // The same bytes arriving for a nomination are authenticated regardless of
  // the gateway rejections.
  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id,
      t_to_parallel_batch(SignedTransactionBatch{poisoned_tx})())();
  EXPECT_EQ(auth_failures(), after_first + 2);
}

// A rejected signing is a verdict about those bytes alone. The genuine
// signing shares its [TransactionHash] -- signatures are deliberately outside
// the hash -- so one bad copy from an anonymous submitter must never lock out
// the genuine one.
TEST(Mempool, RejectedSigningDoesNotBlockValidSigning)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 1);

  // One free slot makes this a regression for capacity-before-auth prefix
  // truncation: the invalid first signing must not starve the valid tail.
  MempoolTestInfra server(/*skip_genesis_block=*/false,
                          /*mempool_tx_size_limit=*/1);

  AddressAuthData auth_data = get_default_account();
  const Ed25519SecretKey private_key =
      create_key_pair(auth_data, /*idx=*/0, /*weight=*/1);
  auth_data.last_written_height = 1;
  auth_data.required_weight = 1;
  server.set_address_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               auth_data);

  Transaction &tx = txs[0].tx;
  tx.auth_claim = make_sign_claim(0);

  SecretKey sk(CryptographicScheme::Ed25519);
  sk.ed25519() = private_key;

  SignedTransaction good_tx = txs[0];
  good_tx.signatures.push_back(make_signature(sk, tx, /*public_key_id=*/0,
                                              MempoolTestInfra::kNetworkHash));

  SignedTransaction poisoned_tx = txs[0];
  DecoratedSignature bad_signature =
      make_signature(sk, tx, /*public_key_id=*/0,
                     MempoolTestInfra::kNetworkHash);
  bad_signature.signature.ed25519()[0] ^= 0xff;
  poisoned_tx.signatures.push_back(bad_signature);

  // Same transaction, so the two share a [TransactionHash] ...
  ASSERT_EQ(sha256(poisoned_tx.tx), sha256(good_tx.tx));
  // ... but different signings, distinguished by their signature bytes.
  ASSERT_NE(sha256(poisoned_tx), sha256(good_tx));

  // One request may contain both signings. Its results stay aligned to the
  // exact signed encodings even though both entries share a transaction hash.
  MempoolGatewayRpc gateway(&server.mempool_coord_rpc());
  const SchedGatewaySubmitOutcomeBatch mixed_outcomes =
      gateway.schedgateway_submit_tx_batch(t_to_parallel_batch(
          SignedTransactionBatch{poisoned_tx, good_tx})())();
  ASSERT_EQ(mixed_outcomes.size(), 2UL);
  EXPECT_EQ(
      mixed_outcomes[0],
      SchedGatewaySubmitOutcome::SchedGatewaySubmitAuthenticationRejected);
  EXPECT_EQ(mixed_outcomes[1],
            SchedGatewaySubmitOutcome::SchedGatewaySubmitAdmitted);
  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 1UL);

  // Rejection is request-scoped rather than retained under the shared hash.
  const SchedGatewaySubmitOutcomeBatch rejected_again =
      gateway.schedgateway_submit_tx_batch(
          t_to_parallel_batch(SignedTransactionBatch{poisoned_tx})())();
  ASSERT_EQ(rejected_again.size(), 1UL);
  EXPECT_EQ(
      rejected_again[0],
      SchedGatewaySubmitOutcome::SchedGatewaySubmitAuthenticationRejected);
  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 1UL);
}

// A stack restarted over a database that another network produced must fail
// at startup, the way a shard given the wrong --network-hash would, instead of
// publishing that network's tip to every downstream and then failing every
// propose and validate on the parent-id gate.
TEST(Mempool, RestartRejectsDatabaseFromAnotherNetwork)
{
  TmpDir db_dir;
  NetworkHash other_network = MempoolTestInfra::kNetworkHash;
  other_network[0] ^= 0x01;

  // Bring the stack up as one network and let it commit genesis, so the
  // database carries a tip stamped with that network's hash.
  {
    MempoolTestInfra server(
        MempoolTestInfra::Options{.network_hash =
                                      MempoolTestInfra::kNetworkHash,
                                  .db_directory = db_dir.directory()});
    const SchedulerGetTipResult tip =
        server.mempool_coord_rpc().schedcoord_get_tip()();
    ASSERT_EQ(tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    EXPECT_EQ(tip.db_tip().value.proposal_header.block_height, 1);
    EXPECT_EQ(tip.db_tip().value.proposal_header.network_hash,
              MempoolTestInfra::kNetworkHash);
  }

  // Restart over the same database as another network. The mempool rejects
  // the coordinator intro that recovers the tip, which fails startup.
  const MempoolTestInfra::Options other_network_options{
      .skip_genesis_block = true,
      .network_hash = other_network,
      .db_directory = db_dir.directory()};
  EXPECT_THROW(
      { MempoolTestInfra rejected(other_network_options); },
      x23::XdrCallFailed);

  // Restarting as the network that produced the database recovers its tip.
  MempoolTestInfra restarted(
      MempoolTestInfra::Options{.skip_genesis_block = true,
                                .network_hash = MempoolTestInfra::kNetworkHash,
                                .db_directory = db_dir.directory()});
  const SchedulerGetTipResult tip =
      restarted.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  EXPECT_EQ(tip.db_tip().value.proposal_header.block_height, 1);
  EXPECT_EQ(tip.db_tip().value.proposal_header.network_hash,
            MempoolTestInfra::kNetworkHash);
}

TEST(Mempool, GenesisLoadGatewayGetTipDataRace)
{
  MempoolTestInfra server(/*skip_genesis_block=*/true);
  MempoolGatewayRpc gateway(&server.mempool_coord_rpc());
  Registration &registration = Registration::registration();
  EXPECT_FALSE(registration.event_registered(nullptr));
  EventCard event_card{.protocol = version::kHogdProtocol(),
                       .address_for_exec = "127.0.0.1:1",
                       .address_for_sched = "127.0.0.1:2"};
  EXPECT_EQ(MempoolEventRpc::schedevent_prepare_event(event_card).outcome(),
            EventPrepareOutcome::EventPrepareNotReady);

  GenesisConfigurationBuilder builder;
  auto root_contract = load_stock_genesis_root_contract();
  GenesisConfiguration genesis_configuration =
      builder.build(MempoolTestInfra::kNetworkHash,
                    StorageCostModel::default_cost_model(), *root_contract,
                    stock_network_config());

  constexpr size_t kWarmupReads = 100;
  std::atomic<bool> load_done = false;
  std::atomic<bool> failure = false;
  std::atomic<size_t> public_tip_reads = 0;
  std::latch start_latch(2);

  std::thread tip_reader([&]() {
    start_latch.arrive_and_wait();
    do {
      ExpectedDBTip tip = gateway.schedgateway_get_tip()();
      const RPCError error = tip.error();
      if (error != RPCError::kRPCNotFound && error != RPCError::kRPCNone) {
        failure.store(true, std::memory_order_release);
        break;
      }
      if (error == RPCError::kRPCNone &&
          tip.db_tip().value.proposal_header.block_height != 0) {
        failure.store(true, std::memory_order_release);
        break;
      }
      public_tip_reads.fetch_add(1, std::memory_order_relaxed);
    } while (!load_done.load(std::memory_order_acquire));
  });

  std::thread genesis_loader([&]() {
    start_latch.arrive_and_wait();
    while (public_tip_reads.load(std::memory_order_acquire) < kWarmupReads &&
           !failure.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    DbSchedResult result =
        server.mempool_coord_rpc().schedcoord_load_genesis_configuration(
            std::move(genesis_configuration))();
    if (result != DbSchedResult::DbSchedResultOK) {
      failure.store(true, std::memory_order_release);
    }
    load_done.store(true, std::memory_order_release);
  });

  genesis_loader.join();
  tip_reader.join();

  EXPECT_FALSE(failure.load(std::memory_order_acquire));
  EXPECT_GE(public_tip_reads.load(std::memory_order_relaxed), kWarmupReads);

  ExpectedDBTip final_tip = gateway.schedgateway_get_tip()();
  EXPECT_EQ(final_tip.error(), RPCError::kRPCNone);
  EXPECT_EQ(final_tip.db_tip().value.proposal_header.block_height, 0);
  server.register_runtime_after_genesis();
  EXPECT_TRUE(registration.event_registered(nullptr));
}

TEST(Mempool, RuntimeReadinessWaitsForEventAndAllExecutors)
{
  MempoolTestInfra server(/*skip_genesis_block=*/true);
  GenesisConfigurationBuilder builder;
  auto root_contract = load_stock_genesis_root_contract();
  ASSERT_EQ(server.mempool_coord_rpc().schedcoord_load_genesis_configuration(
                builder.build(MempoolTestInfra::kNetworkHash,
                              StorageCostModel::default_cost_model(),
                              *root_contract, stock_network_config()))(),
            DbSchedResult::DbSchedResultOK);

  HttpServer health_server;
  add_health_metrics_routes(health_server);
  const uint16_t health_port =
      health_server.start_server(x23::HostPort{"127.0.0.1", 0});
  auto health_status = [&] {
    return request(health_port,
                   "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n");
  };

  auto runtime_init = std::async(std::launch::async, [&] {
    server.mempool_coord_rpc().finish_runtime_init()();
  });
  EXPECT_EQ(runtime_init.wait_for(std::chrono::milliseconds(100)),
            std::future_status::timeout);
  EXPECT_FALSE(server.mempool_coord_rpc().ready());
  EXPECT_NE(health_status().find("\"status\":\"starting\""), std::string::npos);
  std::promise<void> proposal_suspended;
  auto proposal_suspended_future = proposal_suspended.get_future();
  auto pending_proposal = std::async(std::launch::async, [&] {
    auto proposal =
        server.mempool_coord_rpc().schedulercoord_propose_block(0, {});
    proposal_suspended.set_value();
    return proposal();
  });
  EXPECT_EQ(proposal_suspended_future.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);
  EXPECT_EQ(pending_proposal.wait_for(std::chrono::milliseconds(100)),
            std::future_status::timeout);
  EXPECT_EQ(server.mempool_coord_rpc()
                .schedulercoord_validate_block(0, {})()
                .outcome(),
            SchedulerValidationOutcome::SchedulerValidationTooLateToValidate);

  server.register_event_only_after_genesis();
  EXPECT_EQ(runtime_init.wait_for(std::chrono::milliseconds(100)),
            std::future_status::timeout);
  EXPECT_FALSE(server.mempool_coord_rpc().ready());
  EXPECT_NE(health_status().find("\"status\":\"starting\""), std::string::npos);

  server.register_executor_only_after_event();
  ASSERT_EQ(runtime_init.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);
  runtime_init.get();
  EXPECT_TRUE(server.mempool_coord_rpc().ready());
  EXPECT_NE(health_status().find("\"status\":\"ok\""), std::string::npos);
  ASSERT_EQ(pending_proposal.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);
  EXPECT_EQ(pending_proposal.get().outcome(),
            SchedulerProposalOutcome::SchedulerProposalError);
}

// starve-exec: event is up, executor never registers. Readiness must stay
// starting and finish_runtime_init must not return. Register the executor at
// the end so test teardown does not hang.
TEST(Mempool, RuntimeStaysUnreadyWhenExecutorNeverRegisters)
{
  MempoolTestInfra server(/*skip_genesis_block=*/true);
  GenesisConfigurationBuilder builder;
  auto root_contract = load_stock_genesis_root_contract();
  ASSERT_EQ(server.mempool_coord_rpc().schedcoord_load_genesis_configuration(
                builder.build(MempoolTestInfra::kNetworkHash,
                              StorageCostModel::default_cost_model(),
                              *root_contract, stock_network_config()))(),
            DbSchedResult::DbSchedResultOK);

  HttpServer health_server;
  add_health_metrics_routes(health_server);
  const uint16_t health_port =
      health_server.start_server(x23::HostPort{"127.0.0.1", 0});
  auto health_status = [&] {
    return request(health_port,
                   "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n");
  };

  auto runtime_init = std::async(std::launch::async, [&] {
    server.mempool_coord_rpc().finish_runtime_init()();
  });
  server.register_event_only_after_genesis();
  EXPECT_EQ(runtime_init.wait_for(std::chrono::milliseconds(200)),
            std::future_status::timeout);
  EXPECT_FALSE(server.mempool_coord_rpc().ready());
  EXPECT_NE(health_status().find("\"status\":\"starting\""), std::string::npos);

  server.register_executor_only_after_event();
  ASSERT_EQ(runtime_init.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);
  runtime_init.get();
  EXPECT_TRUE(server.mempool_coord_rpc().ready());
}

TEST(Mempool, GenesisCeremonyMintsDeploysAndRejectsRetry)
{
  MempoolTestInfra server(/*skip_genesis_block=*/true);
  const StorageCostModel cost_model = StorageCostModel::default_cost_model();

  GenesisConfigurationBuilder builder;
  auto fibonacci_contract = load_rv32_contract("fibonacci.rv32a");
  const SHA256Hash deployment_nonce = make_test_nonce(9001);
  const SHA256Hash second_deployment_nonce = make_test_nonce(9002);
  const int128_t allocation = 1'000'000'000;
  using fibonacci_meta = ContractTraitMeta<fibonacci::Fibonacci>;
  builder.add_distribution(fibonacci_contract, deployment_nonce,
                           fibonacci_meta::entrypoint<u8"fibonacci">(),
                           fibonacci_meta::make_calldata<u8"fibonacci">(
                               uint32_t{0}),
                           allocation);
  builder.add_distribution(fibonacci_contract, second_deployment_nonce,
                           fibonacci_meta::entrypoint<u8"fibonacci">(),
                           fibonacci_meta::make_calldata<u8"fibonacci">(
                               uint32_t{1}),
                           allocation);

  auto root_contract = load_stock_genesis_root_contract();
  GenesisConfiguration genesis =
      builder.build(MempoolTestInfra::kNetworkHash, cost_model, *root_contract,
                    stock_network_config());
  ASSERT_EQ(server.mempool_coord_rpc().schedcoord_load_genesis_configuration(
                std::move(genesis))(),
            DbSchedResult::DbSchedResultOK);

  auto root_balance = server.read_address_balance(kGenesisRootContractAddress);
  ASSERT_FALSE(root_balance.has_value());
  EXPECT_EQ(root_balance.error(), HogDbCode::kNotFound);
  auto replay_meta =
      server.read_address_replay_cache_meta(kGenesisRootContractAddress);
  ASSERT_FALSE(replay_meta.has_value());
  EXPECT_EQ(replay_meta.error(), HogDbCode::kNotFound);
  auto total_supply = server.read_nnint128(
      ItemKey(kNativeTokenContractAddress, kNativeTokenTotalSupplyKey));
  ASSERT_FALSE(total_supply.has_value());
  EXPECT_EQ(total_supply.error(), HogDbCode::kNotFound);

  const SchedulerGetTipResult parent =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(parent.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  ASSERT_EQ(parent.db_tip().value.proposal_header.block_height, 0);
  const ConfigHash expected_config_hash = compute_config_hash(
      make_config_hash_preimage(cost_model, stock_network_config(),
                                native_default_wallet_hash()));
  EXPECT_EQ(parent.db_tip().value.proposal_header.config_hash,
            expected_config_hash);

  SignedTransactionBatch ceremony_txs;
  ceremony_txs.append_range(
      builder.make_genesis_ceremony_transactions(cost_model));
  ASSERT_EQ(ceremony_txs.size(), 1);
  ASSERT_EQ(ceremony_txs[0].tx.contracts_to_deploy.size(), 1);

  SignedTransactionBatch unauthorized_txs = ceremony_txs;
  unauthorized_txs[0].tx.auth_claim.source_address_claim().required_weight = 1;
  GenesisCeremonyRequest unauthorized_request{
      .expected_parent_height = 0,
      .expected_parent_value_hash = sha256(parent.db_tip().value),
      .context = ProposalContext{.protocol = 0},
      .transactions = t_to_parallel_batch(unauthorized_txs)(),
  };
  auto coordinator_client = server.mempool_coord_client();
  std::promise<void> ceremony_started;
  auto ceremony_started_future = ceremony_started.get_future();
  auto pending_unauthorized = std::async(std::launch::async, [&] {
    auto ceremony = coordinator_client.schedulercoord_commit_genesis_ceremony(
        std::move(unauthorized_request));
    ceremony_started.set_value();
    return ceremony();
  });
  ASSERT_EQ(ceremony_started_future.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);
  EXPECT_EQ(pending_unauthorized.wait_for(std::chrono::milliseconds(100)),
            std::future_status::timeout);
  EXPECT_FALSE(server.mempool_coord_rpc().ready());

  server.register_runtime_after_genesis();

  ASSERT_EQ(pending_unauthorized.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);
  EXPECT_EQ(pending_unauthorized.get().outcome(),
            GenesisCeremonyOutcome::GenesisCeremonyInvalidTransaction);

  ParallelSignedTransactionBatch parallel = t_to_parallel_batch(ceremony_txs)();
  GenesisCeremonyRequest request{
      .expected_parent_height = 0,
      .expected_parent_value_hash = sha256(parent.db_tip().value),
      .context = ProposalContext{.protocol = 0},
      .transactions = parallel,
  };

  // Exercise the RPC encoding and coroutine return path.  Directly invoking
  // MempoolCoordinatorRpc does not cover ownership of the nested
  // ParallelBatch vectors in GenesisCeremonyResult.
  GenesisCeremonyResult result =
      coordinator_client.schedulercoord_commit_genesis_ceremony(request)();
  ASSERT_EQ(result.outcome(), GenesisCeremonyOutcome::GenesisCeremonyCommitted);
  EXPECT_EQ(result.commit().db_tip.value.proposal_header.block_height, 1);
  EXPECT_EQ(result.commit().db_tip.value.proposal_header.config_hash,
            expected_config_hash);

  const ContractAddress deployed =
      calculate_genesis_deployment_address(sha256(*fibonacci_contract),
                                           deployment_nonce);
  ASSERT_TRUE(server.read_address_metadata(deployed).has_value());
  ASSERT_TRUE(server.read_address_balance(deployed).has_value());
  EXPECT_GT(*server.read_address_balance(deployed), 0);
  const ContractAddress second_deployed =
      calculate_genesis_deployment_address(sha256(*fibonacci_contract),
                                           second_deployment_nonce);
  ASSERT_TRUE(server.read_address_metadata(second_deployed).has_value());
  ASSERT_TRUE(server.read_address_balance(second_deployed).has_value());
  EXPECT_GT(*server.read_address_balance(second_deployed), 0);
  ASSERT_TRUE(
      server.read_address_balance(kGenesisRootContractAddress).has_value());
  EXPECT_EQ(*server.read_address_balance(kGenesisRootContractAddress), 0);

  GenesisCeremonyResult retry =
      coordinator_client.schedulercoord_commit_genesis_ceremony(
          std::move(request))();
  EXPECT_EQ(retry.outcome(),
            GenesisCeremonyOutcome::GenesisCeremonyParentMismatch);
}

TEST(Mempool, RejectsOrdinaryBlocksAtGenesisHeights)
{
  MempoolTestInfra server(/*skip_genesis_block=*/true);
  GenesisConfigurationBuilder builder;
  auto root_contract = load_stock_genesis_root_contract();
  ASSERT_EQ(server.mempool_coord_rpc().schedcoord_load_genesis_configuration(
                builder.build(MempoolTestInfra::kNetworkHash,
                              StorageCostModel::default_cost_model(),
                              *root_contract, stock_network_config()))(),
            DbSchedResult::DbSchedResultOK);
  server.register_runtime_after_genesis();

  const SchedulerGetTipResult parent =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(parent.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  ASSERT_EQ(parent.db_tip().value.proposal_header.block_height, 0);

  SchedulerProposalResult normal_proposal =
      server.mempool_coord_rpc().schedulercoord_propose_block(
          parent.db_tip().id, ProposalContext{.protocol = 0})();
  EXPECT_EQ(normal_proposal.outcome(),
            SchedulerProposalOutcome::SchedulerProposalError);

  for (const BlockHeight height : {BlockHeight{0}, BlockHeight{1}}) {
    Proposal proposal{};
    proposal.header.network_hash = MempoolTestInfra::kNetworkHash;
    proposal.header.block_height = height;
    EXPECT_EQ(server.mempool_coord_rpc()
                  .schedulercoord_validate_block(parent.db_tip().id,
                                                 std::move(proposal))()
                  .outcome(),
              SchedulerValidationOutcome::SchedulerValidationInvalid);

    ConsensusValue value{};
    value.proposal_header.block_height = height;
    EXPECT_EQ(server.mempool_coord_rpc().schedulercoord_commit_block(
                  /*block_id=*/0, std::move(value))(),
              SchedulerCommitOutcome::SchedulerCommitInvalidConsensusValue);
  }
}

TEST(Mempool, SubmitUnvettedDuplicateTxDropped)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 2);
  txs[1].tx.invocation = fib_client.make_invocation<u8"slow_fibonacci">(
      /*gas_amount=*/5'000'000'000, uint32_t{11});
  txs[1].tx.uniquifier = txs[0].tx.uniquifier;

  ASSERT_EQ(txs[0].tx.invocation.invoked_address,
            txs[1].tx.invocation.invoked_address);
  ASSERT_EQ(txs[0].tx.uniquifier.block_expiry_height,
            txs[1].tx.uniquifier.block_expiry_height);
  ASSERT_EQ(txs[0].tx.uniquifier.nonce, txs[1].tx.uniquifier.nonce);
  ASSERT_NE(sha256(txs[0].tx), sha256(txs[1].tx));

  MempoolTestInfra server;

  server.mempool_coord_rpc().schedulercoord_submit_unvetted(
      t_to_parallel_batch(txs)())();
  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 2UL);
  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->tx_cache_size(), 2UL);

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);

  SchedulerProposalResult res_nominate =
      propose_block(server, get_tip.db_tip());
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);

  SignedTransactionBatch parsed_txs;
  ASSERT_TRUE(parallel_batch_to_t(res_nominate.proposed_block().txs, parsed_txs,
                                  kNoBatchSizeLimit)());
  ASSERT_EQ(parsed_txs.size(), 1UL);
  ASSERT_TRUE(parsed_txs[0] == txs[0] || parsed_txs[0] == txs[1]);

  const TransactionHash tx0_hash = sha256(txs[0].tx);
  const TransactionHash tx1_hash = sha256(txs[1].tx);

  TransactionHashBatch tx_hashes;
  ASSERT_TRUE(parallel_batch_to_t(
      res_nominate.proposed_block().proposal.transactions.hash, tx_hashes,
      kNoBatchSizeLimit)());
  ASSERT_EQ(tx_hashes.size(), 1UL);
  ASSERT_TRUE(tx_hashes[0] == tx0_hash || tx_hashes[0] == tx1_hash);

  x23::XdrVec<NondeterministicProposalData> tx_nondeterministic_data;
  ASSERT_TRUE(parallel_batch_to_t(
      res_nominate.proposed_block().proposal.transactions.nondeterministic_data,
      tx_nondeterministic_data, kNoBatchSizeLimit)());
  EXPECT_EQ(tx_nondeterministic_data.size(), 1UL);

  const TransactionHash included_hash = tx_hashes[0];
  const TransactionHash sibling_hash =
      included_hash == tx0_hash ? tx1_hash : tx0_hash;

  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 0UL);
  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->tx_cache_size(), 2UL);
  EXPECT_TRUE(server.mempool_coord_rpc().scheduler()->find_tx(included_hash));
  EXPECT_TRUE(server.mempool_coord_rpc().scheduler()->find_tx(sibling_hash));

  SchedulerCommitOutcome res_commit =
      commit_block(server, get_tip.db_tip(), res_nominate.proposed_block());
  ASSERT_EQ(res_commit, SchedulerCommitOutcome::SchedulerCommitOK);

  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->size(), 0UL);
  EXPECT_EQ(server.mempool_coord_rpc().scheduler()->tx_cache_size(), 0UL);
  EXPECT_FALSE(server.mempool_coord_rpc().scheduler()->find_tx(included_hash));
  EXPECT_FALSE(server.mempool_coord_rpc().scheduler()->find_tx(sibling_hash));
}

namespace {

// Two authenticated wrappers over one [body_bytes]-byte Transaction: [thin]
// carries one signature, [fat] all kMaxSigners. Same TransactionHash.
struct SignatureVariants {
  AddressAuthData auth_data;
  SignedTransaction thin;
  SignedTransaction fat;
  TransactionHash tx_hash;
};

SignatureVariants
make_signature_variants(size_t body_bytes)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  SignedTransactionBatch txs = generate_unsigned_txs(fib_client, 1);

  SignatureVariants variants{.auth_data = get_default_account()};
  std::vector<SecretKey> signers;
  signers.reserve(kMaxSigners);
  for (size_t i = 0; i < kMaxSigners; i++) {
    SecretKey signer(CryptographicScheme::Ed25519);
    signer.ed25519() = create_key_pair(variants.auth_data, i, /*weight=*/1);
    signers.push_back(std::move(signer));
  }
  variants.auth_data.last_written_height = 1;
  variants.auth_data.required_weight = 1;

  Transaction &tx = txs[0].tx;
  AuthClaim claim(AuthClaimType::kSourceAddressAuthData);
  claim.source_address_claim().allowed_keys = (uint32_t{1} << kMaxSigners) - 1;
  claim.source_address_claim().required_weight = 1;
  tx.auth_claim = claim;
  tx.invocation.calldata.clear();
  const size_t base = x23::xdr_size(tx);
  CHECK_LE(base, body_bytes);
  tx.invocation.calldata.resize(body_bytes - base, 0);
  CHECK_EQ(x23::xdr_size(tx), body_bytes);

  variants.thin = txs[0];
  variants.thin.signatures.push_back(
      make_signature(signers[0], tx, /*public_key_id=*/0,
                     MempoolTestInfra::kNetworkHash));
  variants.fat = txs[0];
  for (size_t i = 0; i < kMaxSigners; i++) {
    variants.fat.signatures.push_back(
        make_signature(signers[i], tx, /*public_key_id=*/i,
                       MempoolTestInfra::kNetworkHash));
  }
  variants.tx_hash = sha256(tx);
  CHECK_EQ(sha256(variants.thin.tx), variants.tx_hash);
  CHECK_EQ(sha256(variants.fat.tx), variants.tx_hash);
  return variants;
}

// Submits [stx] through the gateway (the authenticated admission path).
SchedGatewaySubmitOutcome
submit_through_gateway(MempoolTestInfra &server, const SignedTransaction &stx)
{
  MempoolGatewayRpc gateway(&server.mempool_coord_rpc());
  const SchedGatewaySubmitOutcomeBatch outcomes =
      gateway.schedgateway_submit_tx_batch(
          t_to_parallel_batch(SignedTransactionBatch{stx})())();
  CHECK_EQ(outcomes.size(), 1UL);
  return outcomes[0];
}

TransactionHashBatch
nominated_hashes(const Proposal &proposal)
{
  TransactionHashBatch hashes;
  CHECK(parallel_batch_to_t(proposal.transactions.hash, hashes,
                            kNoBatchSizeLimit)());
  return hashes;
}

} // namespace

// #3844: proposer and validator may hold different-sized wrappers for one hash.
TEST(Mempool, SignatureVariantsOfOneHashValidateIdentically)
{
  const SignatureVariants variants =
      make_signature_variants(kMaxBytesTransaction);
  ASSERT_EQ(x23::xdr_size(variants.thin.tx), kMaxBytesTransaction);
  ASSERT_LT(x23::xdr_size(variants.thin), kMaxBytesSignedTransaction);
  ASSERT_LE(x23::xdr_size(variants.fat), kMaxBytesSignedTransaction);
  ASSERT_NE(sha256(variants.thin), sha256(variants.fat));

  Proposal proposal;
  {
    MempoolTestInfra proposer;
    proposer.set_address_auth_data(MempoolTestInfra::kAccountAddresses[0],
                                   variants.auth_data);
    std::shared_ptr<Scheduler> scheduler =
        proposer.mempool_coord_rpc().scheduler();

    EXPECT_EQ(submit_through_gateway(proposer, variants.thin),
              SchedGatewaySubmitOutcome::SchedGatewaySubmitAdmitted);
    submit_through_gateway(proposer, variants.fat);
    ASSERT_EQ(scheduler->size(), 1UL);
    const std::shared_ptr<const SignedTransaction> retained =
        scheduler->find_tx(variants.tx_hash);
    ASSERT_NE(retained, nullptr);
    EXPECT_EQ(x23::xdr_size(*retained), x23::xdr_size(variants.thin));

    const SchedulerGetTipResult tip =
        proposer.mempool_coord_rpc().schedcoord_get_tip()();
    ASSERT_EQ(tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    const SchedulerProposalResult proposed =
        propose_block(proposer, tip.db_tip());
    ASSERT_EQ(proposed.outcome(),
              SchedulerProposalOutcome::SchedulerProposalOK);
    proposal = proposed.proposed_block().proposal;
    const TransactionHashBatch nominated = nominated_hashes(proposal);
    ASSERT_EQ(nominated.size(), 1UL);
    EXPECT_EQ(nominated[0], variants.tx_hash);
  }

  {
    MempoolTestInfra validator;
    validator.set_address_auth_data(MempoolTestInfra::kAccountAddresses[0],
                                    variants.auth_data);
    std::shared_ptr<Scheduler> scheduler =
        validator.mempool_coord_rpc().scheduler();

    // The fat wrapper arrives first and is what this node retains.
    EXPECT_EQ(submit_through_gateway(validator, variants.fat),
              SchedGatewaySubmitOutcome::SchedGatewaySubmitAdmitted);
    submit_through_gateway(validator, variants.thin);
    ASSERT_EQ(scheduler->size(), 1UL);
    const std::shared_ptr<const SignedTransaction> retained =
        scheduler->find_tx(variants.tx_hash);
    ASSERT_NE(retained, nullptr);
    EXPECT_EQ(x23::xdr_size(*retained), x23::xdr_size(variants.fat));

    const SchedulerGetTipResult tip =
        validator.mempool_coord_rpc().schedcoord_get_tip()();
    ASSERT_EQ(tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
    const SchedulerValidationResult validated =
        validator.mempool_coord_rpc().schedulercoord_validate_block(
            tip.db_tip().id, proposal)();
    EXPECT_EQ(validated.outcome(),
              SchedulerValidationOutcome::SchedulerValidationOK);
  }
}

// An authenticated wrapper whose body is over kMaxBytesTransaction is dropped
// from the proposal by the executor.
TEST(Mempool, OverCapBodyIsDroppedFromProposal)
{
  const SignatureVariants variants =
      make_signature_variants(kMaxBytesTransaction + 4);
  ASSERT_GT(x23::xdr_size(variants.thin.tx), kMaxBytesTransaction);
  ASSERT_LT(x23::xdr_size(variants.thin), kMaxBytesSignedTransaction);

  MempoolTestInfra server;
  server.set_address_auth_data(MempoolTestInfra::kAccountAddresses[0],
                               variants.auth_data);
  EXPECT_EQ(submit_through_gateway(server, variants.thin),
            SchedGatewaySubmitOutcome::SchedGatewaySubmitAdmitted);
  ASSERT_EQ(server.mempool_coord_rpc().scheduler()->size(), 1UL);

  const SchedulerGetTipResult tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  const SchedulerProposalResult proposed = propose_block(server, tip.db_tip());
  ASSERT_EQ(proposed.outcome(), SchedulerProposalOutcome::SchedulerProposalOK);
  EXPECT_TRUE(nominated_hashes(proposed.proposed_block().proposal).empty());
}

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

// A nomination that repeats a transaction hash decodes cleanly and keeps the
// hash and nondeterministic-data counts equal, so it reaches txset hashing.
// Building the txset is a set insertion, and the repeat is the one input that
// fails it -- which used to abort the validator instead of voting the block
// down. See https://github.com/groundhogchain/hogd/issues/2661.
TEST(Mempool, TxsetHashRejectsDuplicateHashes)
{
  TransactionHashBatch tx_hashes{sha256(uint64_t{1}), sha256(uint64_t{1})};

  EXPECT_FALSE(
      MempoolCoordinatorRpc::try_get_txset_hash(tx_hashes).has_value());
}

TEST(Mempool, TxsetHashAcceptsDistinctHashes)
{
  TransactionHashBatch tx_hashes{sha256(uint64_t{1}), sha256(uint64_t{2})};

  std::optional<SHA256Hash> txset_hash =
      MempoolCoordinatorRpc::try_get_txset_hash(tx_hashes);
  ASSERT_TRUE(txset_hash.has_value());
  EXPECT_EQ(txset_hash, MempoolCoordinatorRpc::get_txset_hash(tx_hashes));
}

TEST(Mempool, ValidateDuplicateProposalHashesIsRejected)
{
  fib_client_t fib_client(MempoolTestInfra::kAccountAddresses[0]);
  const SignedTransactionBatch txs = generate_unsigned_txs(fib_client, kNumTxs);

  MempoolTestInfra server;

  ValidatorID validator_id{};
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();

  SchedulerGetTipResult get_tip =
      server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  DbTip db_tip = get_tip.db_tip();
  SchedulerProposalResult res_nominate = propose_block(server, db_tip);
  ASSERT_EQ(res_nominate.outcome(),
            SchedulerProposalOutcome::SchedulerProposalOK);

  // We reuse the proposed [Proposal] so the nomination names a transaction
  // this node actually holds -- a nomination of unknown hashes is rejected as
  // [SchedulerValidationMissingTxs] before the txset hash is ever compared.
  Proposal proposal = res_nominate.proposed_block().proposal;
  TransactionHashBatch tx_hashes;
  ASSERT_TRUE(parallel_batch_to_t(proposal.transactions.hash, tx_hashes,
                                  kNoBatchSizeLimit)());
  ASSERT_EQ(tx_hashes.size(), 1UL);
  x23::XdrVec<NondeterministicProposalData> tx_nominations;
  ASSERT_TRUE(parallel_batch_to_t(proposal.transactions.nondeterministic_data,
                                  tx_nominations, kNoBatchSizeLimit)());
  ASSERT_EQ(tx_nominations.size(), 1UL);

  // Repeating the sole transaction keeps both batches decodable and keeps
  // their element counts equal, which is every structural property validation
  // checks before it builds the txset.
  proposal.transactions.hash = repeated_parallel_batch(tx_hashes, 2);
  proposal.transactions.nondeterministic_data =
      repeated_parallel_batch(tx_nominations, 2);

  get_tip = server.mempool_coord_rpc().schedcoord_get_tip()();
  ASSERT_EQ(get_tip.outcome(), SchedulerGetTipOutcome::SchedulerGetTipOK);
  db_tip = get_tip.db_tip();
  server.mempool_coord_rpc().schedulercoord_submit_vetted(
      validator_id, t_to_parallel_batch(SignedTransactionBatch{txs[0]})())();

  SchedulerValidationResult res_validate =
      server.mempool_coord_rpc().schedulercoord_validate_block(db_tip.id,
                                                               proposal)();
  EXPECT_EQ(res_validate.outcome(),
            SchedulerValidationOutcome::SchedulerValidationInvalid);
}
