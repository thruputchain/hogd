# Separate the local faucet from developer bootstrap accounts
**Date:** 2026-08-12 **Status:** Proposed
## Problem
Local `groundhog node` currently models every deterministic bootstrap wallet as one homogeneous account group:

- `bootstrapAccounts.count` controls the total number of wallets.

- Accounts are derived at indices `0..count-1` from one seed.

- Every account receives the same `initialStorageTokens` allocation.

- Every runtime-manifest account is labeled `local_user`.

- `dev-0` acts as the local faucet only because downstream CLI code falls back to the first account; genesis and runtime metadata do not identify it as infrastructure.


This makes faucet capacity inseparable from developer-account funding. Raising the default so local airdrops do not quickly exhaust `dev-0` also raises every developer account. Conversely, a user-facing option such as `--dev-account-hog` would accidentally configure the faucet as well.

The local faucet and developer accounts have different responsibilities and need separate configuration, allocation, and metadata.
## Goals
- Make `dev-0` an explicit local faucet with a fixed allocation of **1,000,000 HOG** (`10,000,000,000,000` raw units).

- Define `--dev-account-count` as the number of developer accounts, excluding the faucet.

- Derive developer accounts from index 1, preserving the existing deterministic identities of `dev-1`, `dev-2`, and later accounts.

- Configure developer-account funding independently, with a default of **100 HOG** (`1,000,000,000` raw units).

- Make bootstrap genesis, runtime metadata, persistence metadata, the supervisor, and faucet resolution agree on the faucet role.

- Keep hosted Devnet bootstrap and its dedicated faucet configuration unchanged.

## Non-goals
- No protocol-level minting or change to the fixed HOG supply.

- No change to hosted Devnet account or faucet allocations.

- No configurable local faucet amount in the initial design. Faucet capacity is infrastructure policy, not a developer-account option.

- No change to wallet-create prefunding, ordinary airdrop amounts, deploy funding policy, or insufficient-funds error decoding.

- No preservation of the old meaning of `--dev-account-count`; the new meaning is intentionally less surprising.

## User-facing semantics
The local node always materializes `dev-0` as the faucet. The configured count adds developer accounts after it.

For example:

```sh
groundhog node --dev-account-count 3 --dev-account-hog 50000
```

creates:

| Name | Derivation index | Role | Initial allocation |
|------|------------------|------|--------------------|
| `dev-0` | 0 | `faucet` | 1,000,000 HOG |
| `dev-1` | 1 | `local_user` | 50,000 HOG |
| `dev-2` | 2 | `local_user` | 50,000 HOG |
| `dev-3` | 3 | `local_user` | 50,000 HOG |

`--dev-account-count 0` creates only `dev-0`. Omitting `--dev-account-hog` gives each developer account 100 HOG. The supervisor converts the user-facing HOG value to raw units before writing the bootstrap manifest; `hog-devnode` and all JSON metadata continue to use raw integer units.

The existing direct `hog-devnode` shorthand retains its raw-unit naming:

- `--dev-accounts <count>` means developer accounts, excluding the faucet.

- `--dev-account-tokens <raw-units>` applies only to developer accounts.

- The local faucet amount is fixed and has no command-line override.

## Design
### Separate local configuration from the hosted homogeneous account group
Do not change `BootstrapAccountConfig` into a mixed local faucet/user model. Hosted bootstrap uses it for a homogeneous set of hosted developer accounts and already models its dedicated faucet separately through `HostedFaucetConfig`.

Introduce a local configuration type, conceptually:

```cpp
inline constexpr std::uint64_t kLocalFaucetStorageTokens =
    10'000'000'000'000;
inline constexpr std::uint64_t kDefaultLocalUserStorageTokens =
    1'000'000'000;

struct LocalBootstrapConfig {
  std::uint32_t user_account_count = 0;
  std::string seed = "groundhog-dev-accounts";
  std::uint64_t user_storage_tokens = kDefaultLocalUserStorageTokens;
  std::string dev_wallet_profile = std::string(kDefaultDevWalletProfile);
};
```

`HostedBootstrapGeneratorConfig::bootstrap_config` remains the existing `BootstrapAccountConfig` with its current count and 100-HOG default. This keeps the local semantic change out of hosted Devnet.

The maximum applies to developer accounts. If `kMaxBootstrapAccounts` remains 1000, local bootstrap may materialize 1001 total wallets: one faucet plus up to 1000 users. Rename the constant if needed so its meaning is unambiguous.
### Materialization and deterministic identity
Refactor account materialization so a caller can specify:

- derivation index,

- role,

- initial allocation.


Local bootstrap materializes:

1. index 0 with role `faucet` and `kLocalFaucetStorageTokens`;

2. `user_account_count` accounts at indices `1..user_account_count`, each with role `local_user` and `user_storage_tokens`.


The seed, key derivation, wallet nonce derivation, and deployed-address calculation stay unchanged. Starting users at index 1 preserves existing `dev-1`, `dev-2`, and later identities. The old `dev-0` identity also remains the faucet identity.

Add the role to `BootstrapAccountMetadata` as an enum rather than inferring it from position during JSON generation. Runtime serialization maps the enum to stable strings (`faucet`, `local_user`, and `hosted_dev_account`). The hosted materializer assigns `hosted_dev_account`, preserving the existing role emitted by hosted bootstrap metadata while making role assignment explicit at materialization time. Tests must verify that hosted metadata output remains unchanged.

Genesis supply validation must include the faucet plus all user allocations. The existing `GenesisConfigurationBuilder::build()` total-allocation check is the final guard; tests should also cover the maximum local configuration.
### Bootstrap manifest schema
The supervisor-owned bootstrap manifest must represent faucet and user funding separately. This changes the meaning of `bootstrapAccounts.count`, so use schema version 2 rather than silently reinterpreting version 1.

Proposed version-2 shape:

```json
{
  "schemaVersion": 2,
  "kind": "hogd-bootstrap",
  "bootstrapFaucet": {
    "index": 0,
    "initialStorageTokens": "10000000000000"
  },
  "bootstrapAccounts": {
    "count": 3,
    "firstIndex": 1,
    "seed": "groundhog-dev-accounts",
    "initialStorageTokens": "500000000000"
  }
}
```

The remaining existing fields (`networkHash`, `profile`, `protocol`, `devWallet`, and `storageCostModel`) are unchanged.

`bootstrapFaucet.index` must equal 0, `bootstrapFaucet.initialStorageTokens` must equal the fixed local faucet allocation, and `bootstrapAccounts.firstIndex` must equal 1. Keeping these values explicit makes the generated genesis intent auditable while preventing the supervisor and `hog-devnode` from drifting.

Version-1 manifests are rejected with an actionable message that tells the user to upgrade the supervisor/CLI that generated the manifest. Supporting both meanings would retain ambiguity and is unnecessary for generated, local-only bootstrap intent.
### Runtime manifest
Keep runtime-manifest schema version 2 if consumers already accept arbitrary role strings and an additive account entry; otherwise bump it in coordination with `groundhog-rpc`. The emitted `bootstrapAccounts` array contains the faucet first, followed by developer accounts:

```json
[
  {
    "index": 0,
    "role": "faucet",
    "publicKey": "...",
    "walletAddress": "...",
    "initialStorageTokens": "10000000000000"
  },
  {
    "index": 1,
    "role": "local_user",
    "publicKey": "...",
    "walletAddress": "...",
    "initialStorageTokens": "500000000000"
  }
]
```

The runtime manifest is the source of truth for role resolution. Update `groundhog-rpc` to select the account whose role is `faucet`; remove the positional `dev-0` fallback after the new producer and consumer ship together. Named developer-account resolution continues to use each entry's derivation index.
### Persistent data directories
Persistent metadata currently records one count and one token amount under schema version 1. Bump the metadata schema and record the local distinctions:

```json
{
  "schemaVersion": 2,
  "kind": "hog-devnode-data",
  "genesisHash": "...",
  "bootstrapConfig": {
    "devWalletProfile": "hog-devnode-default",
    "devAccountSeed": "groundhog-dev-accounts",
    "faucetTokens": "10000000000000",
    "devAccounts": 3,
    "devAccountTokens": "500000000000"
  }
}
```

All fields are genesis intent. On recovery, `hog-devnode` compares the requested faucet allocation, user count, user allocation, seed, and wallet profile with persisted metadata before opening the chain. Any mismatch fails with the existing style of actionable `data dir was initialized with different ...` error.

Version-1 data metadata is rejected. Existing persistent local chains require a fresh data directory because their genesis account set has the old semantics. The error should explicitly state that the bootstrap layout changed and a fresh data directory is required; no automatic genesis migration is attempted.
### Direct invocation and bootstrap manifests
Both local startup paths produce the same `LocalBootstrapConfig`:

- Direct `hog-devnode` shorthand builds it from defaults and raw-unit flags.

- `--bootstrap-manifest` loads it from the version-2 manifest.


The existing conflict rule remains: a bootstrap manifest conflicts with local account shorthand flags. Both paths always materialize the fixed faucet.
### Hosted bootstrap isolation
`hog-hosted-bootstrap` remains unchanged:

- Its `BootstrapAccountConfig.count` remains the total homogeneous hosted developer-account count.

- Its developer-account default remains 100 HOG.

- Its optional dedicated faucet still requires explicit `--faucet-public-key` and `--faucet-tokens` values.

- Hosted metadata continues to represent the dedicated faucet separately.


Tests must make this isolation explicit so future local-default changes cannot alter hosted allocations accidentally.
### Transition from the current `fix/faucet-bootstrap` branch
The current branch raises `BootstrapAccountConfig`'s shared default and the release smoke argument to `10,000,000,000,000` raw units. Those changes are an intermediate homogeneous-account implementation and must not survive the split:

- Restore `BootstrapAccountConfig::storage_tokens` to the 100-HOG homogeneous default used by hosted bootstrap.

- Move the 1,000,000-HOG constant to the local faucet configuration only.

- Change the release smoke invocation so `--dev-account-tokens` uses the 100-HOG local-user default, or omit it to exercise that default.

- Update release smoke expectations for one always-present faucet plus the requested number of users.

- Replace the existing shared-default assertion with role/allocation assertions for the separated local materialization.


This prevents `--dev-account-tokens 10000000000000` from being reinterpreted as 1,000,000 HOG for every developer account after the flag becomes user-only.
## Cross-repository coordination
### `groundhog-hogd`
- Add `LocalBootstrapConfig` and fixed local faucet constants.

- Materialize index 0 as `faucet` and users from index 1.

- Add roles to local account metadata.

- Parse bootstrap manifest schema version 2.

- Emit role-aware runtime metadata.

- Persist and validate metadata schema version 2.

- Preserve hosted bootstrap behavior.

### Supervisor / `groundhog node`
- Define `--dev-account-count` as user accounts excluding the faucet.

- Add or retain `--dev-account-hog` for per-user allocation, defaulting to 100 HOG.

- Convert HOG values with up to seven decimal places to raw units exactly.

- Generate bootstrap manifest version 2 with the fixed faucet and separate user allocation.

- Explain in help text that `dev-0` is always present and excluded from count.

### `groundhog-rpc`
- Parse and retain runtime account roles.

- Resolve the local airdrop sponsor by role `faucet`, not array position.

- Keep named account resolution aligned with indices starting at `dev-1`.

- Remove the legacy `dev-0` positional fallback once producer and consumer compatibility is coordinated.


Changes should land in dependency order: `groundhog-hogd`, then the consuming RPC/CLI and supervisor updates, followed by an integrated local smoke test.
## Testing
### `groundhog-hogd`
- Default local config materializes only `dev-0`, with role `faucet` and `10,000,000,000,000` raw units.

- User count 3 materializes four accounts at indices 0 through 3.

- Faucet uses the index-0 golden key/address; users retain existing index-1+ golden keys/addresses.

- Custom user funding changes indices 1+ but not the faucet allocation.

- Runtime metadata reports `faucet` for index 0 and `local_user` for indices 1+.

- Bootstrap manifest version 2 parses separate faucet/user allocations and rejects wrong fixed indices, wrong faucet funding, malformed raw amounts, and version 1.

- Direct shorthand and equivalent manifest input produce identical genesis and runtime metadata.

- Persistent recovery succeeds with matching schema-version-2 metadata and fails clearly for count, user funding, seed, profile, or faucet mismatches.

- Version-1 persistent metadata requests a fresh data directory.

- Maximum user count plus faucet stays within account and HOG-supply limits; excessive aggregate allocations fail cleanly.

- Hosted bootstrap tests continue to assert ten homogeneous accounts at 100 HOG by default and no implicit dedicated faucet.

### Cross-repository integration
- `groundhog node` with defaults exposes `dev-0` as faucet and the configured default number of users at 100 HOG each.

- `groundhog node --dev-account-count 3 --dev-account-hog 50000` exposes exactly the four-account table described above.

- Local wallet creation and repeated airdrops debit `dev-0`, not a user account.

- Named operations against `dev-1`, `dev-2`, and `dev-3` use their existing deterministic identities.

- A persistent node started with changed genesis funding fails before service readiness and instructs the user to choose a fresh data directory.

## Files expected to change
| Area | Files |
| --- | --- |
| Local config/materialization | `hogd/devnode/bootstrap_accounts.h`, `hogd/devnode/bootstrap_accounts.cc` |
| Bootstrap manifest | `hogd/devnode/bootstrap_manifest.h`, `hogd/devnode/bootstrap_manifest.cc` |
| Local CLI | `hogd/devnode/hog_devnode.cc` |
| Node/persistence | `hogd/devnode/hog_devnode_node.h`, `hogd/devnode/hog_devnode_node.cc` |
| Runtime manifest | `hogd/devnode/runtime_manifest.cc` |
| Tests | `hogd/devnode/bootstrap_manifest_test.cc`, `hogd/devnode/hog_devnode_test.cc`, `hogd/devnode/hosted_bootstrap_generator_test.cc` |
| Release smoke | `.github/workflows/release.yml` |

External supervisor and `groundhog-rpc` files are tracked in their respective repositories.
## Risks and mitigations
- **Count semantic break.** Count now excludes the faucet. Use bootstrap manifest schema version 2, update help text, and cover count 0/1/N in integration tests.

- **Persistent genesis incompatibility.** Old chains have a different account set and allocations. Reject version-1 metadata with a fresh-directory instruction instead of attempting migration.

- **Producer/consumer role skew.** Coordinate runtime-role parsing and faucet resolution before removing the positional fallback.

- **Hosted behavior regression.** Keep hosted configuration on the existing homogeneous type and lock its defaults in tests.

- **Off-by-one derivation errors.** Assert golden identities at indices 0, 1, and the configured upper boundary.

- **Supply over-allocation.** Build all local allocations through one genesis builder and retain aggregate supply validation.

## Rollout
1. Implement and test the local split in `groundhog-hogd`, including schema version 2 and hosted-isolation tests.

2. Update `groundhog-rpc` to resolve the explicit `faucet` role and preserve named user-account resolution.

3. Update the supervisor/CLI flags and bootstrap-manifest generator.

4. Release compatible component versions together.

5. Run a clean local-node smoke test covering account listing, wallet creation, airdrop, deployment, and persistent-state mismatch.

6. Remove any temporary positional faucet fallback after the compatible release boundary is established.
