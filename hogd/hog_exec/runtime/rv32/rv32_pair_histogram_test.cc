#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "built_contracts/contract_loader.h"
#include "built_contracts/fibonacci.h"
#include "built_contracts/itc_erc20.h"
#include "contract.hh"
#include "ffi_enums.hh"
#include "hogd/base/types.h"
#include "hogd/hog_exec/common/status.h"
#include "hogd/hog_exec/runtime/contract_trait_client.h"
#include "hogd/hog_exec/runtime/item/deployed_contract.h"
#include "hogd/hog_exec/runtime/item/storage_snapshot_cache.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/runtime_exports.h"
#include "hogd/hog_exec/runtime/rv32/artifact.h"
#include "hogd/hog_exec/runtime/rv32/host.h"
#include "hogd/hog_exec/runtime/rv32/machine.h"
#include "hogd/hog_exec/runtime/rv32/program.h"
#include "hogd/hog_exec/runtime/rv32/rv32_contract.h"
#include "hogd/hog_exec/runtime/syscall_entry.h"
#include "hogd/hog_exec/runtime/syscall_handler.h"
#include "rpc_types.hh"
#include "syscall_error.hh"
#include "transaction.hh"
#include "types.hh"

// Offline analysis tool (NOT part of the main build): traces a real rv32
// contract invocation on the canonical step-at-a-time Machine and builds a
// histogram of adjacent *fall-through* instruction pairs -- the candidates an
// interpreter could fuse into a single super-instruction. Each contract gets
// its own histogram. Run the binary and read the dumped tables on stdout.
//
// A pair (A, B) is counted only when B executes by falling through from A
// (B's pc == A's pc + 4) and A is straight-line code (not a branch/jump/system
// op): those are the only pairs a decode-time fusion can rely on. Each pair is
// further split by data dependency -- whether B reads A's destination register
// -- since "compute then use" is the classic fusion win.

namespace hogd {
namespace {

using rv32::DecodedInstruction;
using rv32::InstructionKind;
using rv32::Op;

std::string_view
op_name(Op op)
{
  // The schedule names are `u8"..."` (ASCII) wire constants; reinterpret the
  // bytes as a narrow view purely for printing.
  return reinterpret_cast<const char *>(
      rv32::kInstructionGasSchedule[static_cast<size_t>(op)].name);
}

// Whether an instruction of this kind, as the *first* of a pair, can be the
// head of a fused super-instruction. Control-flow and system ops cannot: a
// fused handler runs both halves unconditionally, so a (conditional) branch or
// a jump or an ecall must end the run. (Taken branches/jumps already break pc
// contiguity; this also rejects the not-taken-branch fall-through.)
bool
is_fusable_first(InstructionKind kind)
{
  switch (kind) {
  case InstructionKind::kBranch:
  case InstructionKind::kJal:
  case InstructionKind::kJalr:
  case InstructionKind::kFence:
  case InstructionKind::kEcall:
  case InstructionKind::kEbreak:
  case InstructionKind::kUnimp:
    return false;
  case InstructionKind::kLui:
  case InstructionKind::kAuipc:
  case InstructionKind::kLoad:
  case InstructionKind::kStore:
  case InstructionKind::kOpImm:
  case InstructionKind::kOp:
    return true;
  }
  return false;
}

bool
writes_rd(InstructionKind kind)
{
  switch (kind) {
  case InstructionKind::kLui:
  case InstructionKind::kAuipc:
  case InstructionKind::kJal:
  case InstructionKind::kJalr:
  case InstructionKind::kLoad:
  case InstructionKind::kOpImm:
  case InstructionKind::kOp:
    return true;
  default:
    return false;
  }
}

bool
reads_rs1(InstructionKind kind)
{
  switch (kind) {
  case InstructionKind::kJalr:
  case InstructionKind::kBranch:
  case InstructionKind::kLoad:
  case InstructionKind::kStore:
  case InstructionKind::kOpImm:
  case InstructionKind::kOp:
    return true;
  default:
    return false;
  }
}

bool
reads_rs2(InstructionKind kind)
{
  switch (kind) {
  case InstructionKind::kBranch:
  case InstructionKind::kStore:
  case InstructionKind::kOp:
    return true;
  default:
    return false;
  }
}

enum class DepKind { kIndependent, kRdToRs1, kRdToRs2 };

// Does the second instruction read the register the first just wrote?
DepKind
classify_dep(const DecodedInstruction &first, const DecodedInstruction &second)
{
  if (!writes_rd(first.kind) || first.rd == 0) {
    return DepKind::kIndependent;
  }
  if (reads_rs1(second.kind) && second.rs1 == first.rd) {
    return DepKind::kRdToRs1;
  }
  if (reads_rs2(second.kind) && second.rs2 == first.rd) {
    return DepKind::kRdToRs2;
  }
  return DepKind::kIndependent;
}

struct PairCounts {
  uint64_t dep_rs1 = 0;
  uint64_t dep_rs2 = 0;
  uint64_t independent = 0;

  [[nodiscard]] uint64_t total() const
  {
    return dep_rs1 + dep_rs2 + independent;
  }
};

// Accumulator only: pairing decisions (fall-through, fusability) are made by
// the driver, which holds the per-run "previous instruction" state so each run
// starts clean.
class PairHistogram {
public:
  void note_instruction() { ++instructions_; }

  void add(Op first, Op second, DepKind dep)
  {
    PairCounts &counts = pairs_[{first, second}];
    switch (dep) {
    case DepKind::kRdToRs1:
      ++counts.dep_rs1;
      break;
    case DepKind::kRdToRs2:
      ++counts.dep_rs2;
      break;
    case DepKind::kIndependent:
      ++counts.independent;
      break;
    }
    ++pairs_counted_;
  }

  // A fall-through triple (a; b; c) with a and b straight-line. Surfaces 3-op
  // fusion candidates (e.g. the rotate idiom srl; sll; or).
  void add_triple(Op a, Op b, Op c) { ++triples_[{a, b, c}]; }

  [[nodiscard]] uint64_t instructions() const { return instructions_; }
  [[nodiscard]] uint64_t pairs_counted() const { return pairs_counted_; }
  [[nodiscard]] const std::map<std::pair<Op, Op>, PairCounts> &pairs() const
  {
    return pairs_;
  }
  [[nodiscard]] const std::map<std::tuple<Op, Op, Op>, uint64_t> &
  triples() const
  {
    return triples_;
  }

private:
  std::map<std::pair<Op, Op>, PairCounts> pairs_;
  std::map<std::tuple<Op, Op, Op>, uint64_t> triples_;
  uint64_t instructions_ = 0;
  uint64_t pairs_counted_ = 0;
};

void
print_histogram(std::string_view name, const PairHistogram &hist)
{
  std::println("\n=== {} ===", name);
  std::println("executed instructions:      {}", hist.instructions());
  std::println("fall-through fusable pairs: {}  ({} distinct)",
               hist.pairs_counted(), hist.pairs().size());

  std::vector<std::pair<std::pair<Op, Op>, PairCounts>> rows(
      hist.pairs().begin(), hist.pairs().end());
  std::ranges::sort(rows, [](const auto &a, const auto &b) {
    return a.second.total() > b.second.total();
  });

  for (const auto &[key, counts] : rows) {
    std::println("  {:<7} -> {:<7} {:>12}   [rd->rs1 {}, rd->rs2 {}, indep {}]",
                 op_name(key.first), op_name(key.second), counts.total(),
                 counts.dep_rs1, counts.dep_rs2, counts.independent);
  }

  std::vector<std::pair<std::tuple<Op, Op, Op>, uint64_t>> trows(
      hist.triples().begin(), hist.triples().end());
  std::ranges::sort(trows, [](const auto &a, const auto &b) {
    return a.second > b.second;
  });
  std::println("-- top fall-through triples (straight-line; straight-line; "
               "any) --");
  for (const auto &[key, count] : trows) {
    std::println("  {:<7} -> {:<7} -> {:<7} {:>12}", op_name(std::get<0>(key)),
                 op_name(std::get<1>(key)), op_name(std::get<2>(key)), count);
  }
}

// Maps a raw syscall_entry result onto the Machine's host-result API. This
// mirrors the production threaded path (rv32_contract.cc / syscall_bridge.cc):
// syscall_entry has already written its output directly into guest memory, so
// there is nothing to commit here.
rv32::HostResult
to_host_result(
    const std::pair<HostFnStatus<SyscallErrorCode>, GasAmount> &result)
{
  const auto &[status, gas_consumed] = result;
  if (status.has_value()) {
    return rv32::HostResult::continue_with(static_cast<uint32_t>(
                                               std::to_underlying(*status)),
                                           gas_consumed);
  }
  switch (status.error().code()) {
  case HostFnErrorCode::Return:
    return rv32::HostResult::halt(0, gas_consumed);
  case HostFnErrorCode::Exit:
    return rv32::HostResult::deterministic_trap(gas_consumed);
  case HostFnErrorCode::OutOfGas:
    return rv32::HostResult::out_of_gas(gas_consumed);
  default:
    return rv32::HostResult::trap(gas_consumed,
                                  static_cast<uint32_t>(status.error().code()));
  }
}

// A Runtime that runs the invocation on the canonical Machine (re-decoded from
// the artifact) one instruction at a time, recording fall-through pairs into a
// borrowed histogram. Drives ecalls through syscall_entry exactly like
// Rv32Runtime::invoke_internal, so behavior matches the production path.
class TracingRv32Runtime final : public Runtime {
public:
  TracingRv32Runtime(rv32::ArtifactImage image,
                     SystemCallHandler *syscall_handler, PairHistogram *hist)
    : Runtime(syscall_handler), image_(std::move(image)), hist_(hist)
  {}

private:
  rv32::ArtifactImage image_;
  PairHistogram *hist_;

  std::pair<InvokeStatus<void>, GasAmount>
  invoke_internal(MethodID method_id, uint64_t gas_limit) override
  {
    rv32::Machine machine(image_.program, image_.memory_bytes, gas_limit);
    std::ranges::copy(image_.initial_memory, machine.memory().begin());
    machine.seed_method_id(method_id);

    ExternHostCallContext host_fn_ctx = syscall_handler_;

    const DecodedInstruction *prev = nullptr;
    uint32_t prev_pc = 0;
    const DecodedInstruction *prev2 = nullptr;
    uint32_t prev2_pc = 0;

    rv32::StepResult result = rv32::StepResult::kContinue;
    for (;;) {
      const uint32_t pc = machine.pc();
      const bool fetched = !machine.halted() &&
                           !machine.pending_ecall().has_value() &&
                           image_.program->contains_pc(pc);
      const DecodedInstruction *cur =
          fetched ? &image_.program->instruction_at_aligned_pc(pc) : nullptr;

      result = machine.step();

      if (cur != nullptr) {
        hist_->note_instruction();
        if (prev != nullptr && pc == prev_pc + rv32::kInstructionBytes &&
            is_fusable_first(prev->kind)) {
          hist_->add(prev->op, cur->op, classify_dep(*prev, *cur));
        }
        // Fall-through triple (prev2; prev; cur) with prev2, prev
        // straight-line.
        if (prev2 != nullptr && prev != nullptr &&
            pc == prev_pc + rv32::kInstructionBytes &&
            prev_pc == prev2_pc + rv32::kInstructionBytes &&
            is_fusable_first(prev2->kind) && is_fusable_first(prev->kind)) {
          hist_->add_triple(prev2->op, prev->op, cur->op);
        }
        prev2 = prev;
        prev2_pc = prev_pc;
        prev = cur;
        prev_pc = pc;
      }

      if (result == rv32::StepResult::kContinue) {
        continue;
      }
      if (result == rv32::StepResult::kEcall) {
        const std::optional<rv32::EcallArgs> pending = machine.pending_ecall();
        if (!pending.has_value()) {
          break; // unreachable: kEcall always stashes args
        }
        const rv32::EcallArgs args = *pending;
        ExecutionContext context = {
            .memory = std::as_writable_bytes(machine.memory()),
            .gas_remaining = machine.gas_remaining(),
        };
        result = machine.complete_ecall(to_host_result(
            syscall_entry(host_fn_ctx, context, args.a0, args.a1, args.a2,
                          args.a3, args.a4, args.a5, args.a6, args.a7)()));
        if (result == rv32::StepResult::kContinue) {
          continue;
        }
      }
      break;
    }

    const GasAmount consumed = gas_limit - machine.gas_remaining();
    if (result == rv32::StepResult::kHalted) {
      // Halt only arises from a clean Return; Exit maps to a deterministic
      // trap in to_host_result.
      return {{}, consumed};
    }
    if (machine.trap() == rv32::Trap::kOutOfGas) {
      return {InvokeError{InvokeErrorCode::OutOfGas}, consumed};
    }
    return {InvokeError{InvokeErrorCode::Deterministic}, consumed};
  }
};

// RuntimeContext that hands out TracingRv32Runtimes. The active histogram is
// set by the test before each contract's invocations; cross-calls (none in the
// traced contracts here) would accumulate into the same bucket.
class TracingRv32Context final : public RuntimeContext {
public:
  PairHistogram *active_histogram = nullptr;

  InvokeErrorCode verify_contract(Contract const &contract) const override
  {
    return delegate_.verify_contract(contract);
  }

  SystemStatus<std::unique_ptr<Runtime>>
  make_new_runtime(DeployedContract const &deployed_contract,
                   SystemCallHandler *syscall_handler) override
  {
    const ContractBlob *blob = nullptr;
    if (deployed_contract.entry != nullptr) {
      blob = &deployed_contract.entry->contract.rv32_contract();
    }
    else {
      auto contract =
          syscall_handler->resolve_contract(deployed_contract.hash.hash)();
      if (!contract.has_value()) {
        return contract.error();
      }
      if (contract.value() == nullptr) {
        return SystemError{SystemErrorCode::Unrecoverable};
      }
      blob = &contract.value()->rv32_contract();
    }

    auto image = rv32::parse_artifact(blob->data);
    if (!image.has_value()) {
      return SystemError{SystemErrorCode::Unrecoverable};
    }
    return std::make_unique<TracingRv32Runtime>(std::move(image.value()),
                                                syscall_handler,
                                                active_histogram);
  }

private:
  Rv32Context delegate_;
};

class Rv32PairHistogramTest : public ::testing::Test,
                              public NativeContractTestFramework {
public:
  Rv32PairHistogramTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kMockInMemory)
  {}

protected:
  TracingRv32Context runtime_context_;
  std::map<std::string, PairHistogram> histograms_;

  constexpr static GasAmount kSufficientGas = 100'000'000;

  ContractAddress deploy_rv32(std::string_view artifact, uint64_t addr_seed)
  {
    auto contract = std::make_shared<Contract>(ContractType::kRv32);
    contract->rv32_contract() = load_contract_blob(artifact);
    auto addr = make_contract_address(addr_seed);
    write_contract(contract);
    write_deployment(addr, contract);
    setup_grant_storage_token(addr, 1'000'000'000);
    return addr;
  }

  // Runs one invocation, accumulating its trace into histograms_[bucket].
  // Returns whether the root invocation succeeded.
  template<typename Invocation>
  bool trace_invocation(std::string const &bucket, Invocation invocation)
  {
    runtime_context_.active_histogram = &histograms_[bucket];
    auto handler = make_new_system_call_handler(
        SignedTransaction{.tx =
                              Transaction{.invocation = std::move(invocation),
                                          .uniquifier = make_new_uniquifier()}},
        runtime_context_);
    bool ok = handler.root_invoke()().is_root_success();
    if (ok) {
      handler.try_commit_transaction()();
      advance_block();
    }
    return ok;
  }
};

TEST_F(Rv32PairHistogramTest, TraceFibonacciAndErc20)
{
  {
    auto addr = deploy_rv32("fibonacci.rv32a", 0);
    ContractTraitClient<fibonacci::Fibonacci> client(addr);
    EXPECT_TRUE(
        trace_invocation("fibonacci",
                         client.make_invocation<u8"fibonacci">(kSufficientGas,
                                                               10U)));
  }

  {
    // A root transaction has no msg_sender, so any erc20 method that calls
    // get_caller() (mint/transfer/burn/adjust_allowance, and get_balance, which
    // is get_balance_of(get_caller())) reverts at the root frame. We drive the
    // caller-free reads instead -- they still exercise the full calldata-parse,
    // address-key-hashing, and storage-read paths.
    auto addr = deploy_rv32("itc_erc20.rv32a", 1);
    ContractTraitClient<itc_erc20::ERC20> client(addr);
    const auto owner = make_contract_address(99);
    const auto spender = make_contract_address(100);
    EXPECT_TRUE(
        trace_invocation("itc_erc20",
                         client.make_invocation<u8"initialize">(kSufficientGas,
                                                                owner)));
    EXPECT_TRUE(trace_invocation(
        "itc_erc20", client.make_invocation<u8"total_supply">(kSufficientGas)));
    EXPECT_TRUE(trace_invocation(
        "itc_erc20",
        client.make_invocation<u8"get_balance_of">(kSufficientGas, owner)));
    EXPECT_TRUE(trace_invocation(
        "itc_erc20",
        client.make_invocation<u8"allowance">(kSufficientGas, owner, spender)));
  }

  for (auto const &[name, hist] : histograms_) {
    print_histogram(name, hist);
  }
}

} // namespace
} // namespace hogd
