# local_test_contracts

Test contracts checked directly into this repo, unlike the sibling
`integration_test_contracts/` directory, which is a git submodule. Use this
directory when a contract should evolve in lockstep with hogd (same PR, same
review, no submodule pointer dance).

Both directories share the cargo workspace rooted at `built_contracts/`
(`Cargo.toml` / `Cargo.lock`) and the same build pipeline: `build_risc.sh`
lowers each crate to a `<name>.rv32a` artifact via groundhog-cli, and
`generate_header.py` turns the emitted `<name>_spec.json` into a C++ header
(`build/built_contracts/<name>.h`). Outputs land flat in
`build/built_contracts/`, so crate names must be unique across both
directories.

## Adding a contract

1. Create `local_test_contracts/<name>/` with `Cargo.toml` and
   `src/{lib.rs, main.rs, interface.rs, implementation.rs}` — copy an
   existing crate here as a template.
2. Add `local_test_contracts/<name>` to the workspace `members` list in
   `built_contracts/Cargo.toml`, then refresh the lockfile:
   `cd built_contracts && cargo metadata --offline > /dev/null`.
3. Add `local_test_contracts/<name>` to the `add_riscv_builds(SPEC_CONTRACTS ...)`
   call in `built_contracts/CMakeLists.txt`, and add
   `add_contract_header(<name>)` if C++ tests consume it.
4. Build with the `build_all_contracts` target; consume from C++ tests via
   `#include "built_contracts/<name>.h"` and
   `ContractTraitMeta<<name>::<Trait>>::load_rv32()`.

## Current contracts

- `tx_context_test` — transaction-context syscalls: GetSourceTransactionHash,
  GetBlockHeight, and the valid-statistic side of GetTransactionStatistics.
  Driven by `hogd/hog_exec/runtime/tx_context_tests.cc`.
- `auth_weight_test` — auth-configuration syscalls from a guest frame:
  WriteAddressRequiredWeight (happy path + read-only-frame rejection) and
  GetAuthClaim. Driven by `hogd/hog_exec/runtime/auth_weight_tests.cc`.
- `airdrop_faucet` — a genesis ROOT contract for testnet-style chains: mints
  any requested amount to the caller (the native token's mint is gated on the
  genesis-root address). Its first claim also funds the token contract's own
  storage rows, the faucet analog of the stock ceremony's token_funding mint.
  Driven by `hogd/hog_exec/runtime/faucet_scenario_test.cc`.
- `drip_faucet` — a rate-limited genesis ROOT contract: at most 100 native
  tokens per 5-block window, globally, tracked with a CapacityConstraint so
  concurrent same-window claims compose and only aggregate overdraws are
  rejected (at block packing). Driven by
  `hogd/hog_exec/runtime/faucet_scenario_test.cc`.
- `faucet_caller` — a contract-shaped faucet user for the bootstrap-loop
  scenario: deployed through the default wallet by an airdrop-funded pubkey
  account, then claims from the faucet for itself. Driven by
  `hogd/hog_exec/runtime/faucet_scenario_test.cc`.
