#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "hogd/hogexec/runtime/rv32/arch_trace.h"
#include "hogd/hogexec/runtime/rv32/host.h"
#include "hogd/hogexec/runtime/rv32/oracle.h"
#include "hogd/hogexec/runtime/rv32/program.h"

// SpikeOracle: drives the external Spike (riscv-isa-sim) reference simulator as
// an Oracle, so our Machine can be differentially compared against an
// independent RISC-V implementation -- the check on the checker that the
// in-tree spec model cannot provide. This is Rung 1 of the acquisition ladder:
// Spike is a subprocess located via the RV32_ORACLE_BIN environment variable,
// so nothing about Spike enters the build graph or default CI;
// spike_oracle_available() lets the test skip cleanly when it is absent.
//
// First-increment scope: register-only, ecall-terminated, zero-initial-register
// programs (no loads/stores, no jalr). Code is loaded at kSpikeTextBase
// (0x80000000, Spike's default DRAM base) and every control transfer is
// pc-relative, so our 0-based pc-space maps to Spike by a constant offset with
// no address-space mismatch. Loads/stores (which need the data region mapped
// without colliding with code or Spike's bootrom) and computed jumps are a
// later increment. There is no production code here; it is harness
// infrastructure.

namespace hogd::rv32 {

inline constexpr uint32_t kSpikeTextBase = 0x80000000U;

// The terminal ecall traps in M-mode and Spike then spins in its default trap
// handler (below kSpikeTextBase, so it emits no in-range commits). We wait that
// spin out with a short `timeout`; the program itself retires in microseconds,
// so every in-range commit is emitted well before the kill. A future
// optimization could stream and kill on the first ecall-exception marker.
inline constexpr const char *kSpikeTimeoutSeconds = "0.5";

inline std::string
spike_oracle_binary()
{
  const char *path = std::getenv("RV32_ORACLE_BIN");
  if (path == nullptr || path[0] == '\0') {
    return {};
  }
  return std::string(path);
}

inline bool
spike_oracle_available()
{
  const std::string bin = spike_oracle_binary();
  return !bin.empty() && access(bin.c_str(), X_OK) == 0;
}

inline void
elf_put_u16(std::vector<uint8_t> &out, uint16_t value)
{
  out.push_back(static_cast<uint8_t>(value));
  out.push_back(static_cast<uint8_t>(value >> 8));
}

inline void
elf_put_u32(std::vector<uint8_t> &out, uint32_t value)
{
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<uint8_t>(value >> (8 * i)));
  }
}

// Build a minimal ET_EXEC RV32 ELF with one R+X PT_LOAD segment holding `code`
// at `vaddr`, entry at `entry`. A NULL + .shstrtab section table is included
// because Spike's ELF loader asserts e_shstrndx < e_shnum. No symbols, so Spike
// prints a harmless "tohost/fromhost not in ELF" warning.
inline std::vector<uint8_t>
emit_rv32_elf(std::span<const uint32_t> code_words, uint32_t entry,
              uint32_t vaddr)
{
  const std::vector<uint8_t> code = code_bytes_from_words(code_words);
  const uint32_t code_off = 0x1000;
  const std::string shstr(std::string("\0.shstrtab\0", 11));
  const auto shstr_off = static_cast<uint32_t>(code_off + code.size());
  const uint32_t shoff =
      (shstr_off + static_cast<uint32_t>(shstr.size()) + 3U) & ~3U;

  std::vector<uint8_t> elf;
  // e_ident: ELF32, little-endian, version 1, then padding to 16 bytes.
  const std::array<uint8_t, 16> ident = {0x7f, 'E', 'L', 'F', 1, 1, 1, 0,
                                         0,    0,   0,   0,   0, 0, 0, 0};
  elf.insert(elf.end(), ident.begin(), ident.end());
  elf_put_u16(elf, 2);     // e_type = ET_EXEC
  elf_put_u16(elf, 243);   // e_machine = EM_RISCV
  elf_put_u32(elf, 1);     // e_version
  elf_put_u32(elf, entry); // e_entry
  elf_put_u32(elf, 52);    // e_phoff (right after the 52-byte header)
  elf_put_u32(elf, shoff); // e_shoff
  elf_put_u32(elf, 0);     // e_flags
  elf_put_u16(elf, 52);    // e_ehsize
  elf_put_u16(elf, 32);    // e_phentsize
  elf_put_u16(elf, 1);     // e_phnum
  elf_put_u16(elf, 40);    // e_shentsize
  elf_put_u16(elf, 2);     // e_shnum
  elf_put_u16(elf, 1);     // e_shstrndx

  // Program header (PT_LOAD, R+X).
  elf_put_u32(elf, 1);                                  // p_type = PT_LOAD
  elf_put_u32(elf, code_off);                           // p_offset
  elf_put_u32(elf, vaddr);                              // p_vaddr
  elf_put_u32(elf, vaddr);                              // p_paddr
  elf_put_u32(elf, static_cast<uint32_t>(code.size())); // p_filesz
  elf_put_u32(elf, static_cast<uint32_t>(code.size())); // p_memsz
  elf_put_u32(elf, 5);                                  // p_flags = R+X
  elf_put_u32(elf, 0x1000);                             // p_align

  elf.resize(code_off, 0);
  elf.insert(elf.end(), code.begin(), code.end());
  elf.insert(elf.end(), shstr.begin(), shstr.end());
  elf.resize(shoff, 0);

  // Section header 0: SHT_NULL (all zero).
  for (int i = 0; i < 40; ++i) {
    elf.push_back(0);
  }
  // Section header 1: .shstrtab (SHT_STRTAB).
  elf_put_u32(elf, 1);         // sh_name = offset of ".shstrtab"
  elf_put_u32(elf, 3);         // sh_type = SHT_STRTAB
  elf_put_u32(elf, 0);         // sh_flags
  elf_put_u32(elf, 0);         // sh_addr
  elf_put_u32(elf, shstr_off); // sh_offset
  elf_put_u32(elf, static_cast<uint32_t>(shstr.size())); // sh_size
  elf_put_u32(elf, 0);                                   // sh_link
  elf_put_u32(elf, 0);                                   // sh_info
  elf_put_u32(elf, 1);                                   // sh_addralign
  elf_put_u32(elf, 0);                                   // sh_entsize
  return elf;
}

struct SpikeCommit {
  uint32_t pc = 0;
  bool has_reg_write = false;
  uint8_t rd = 0;
  uint32_t value = 0;
};

inline bool
parse_hex_u32(const std::string &token, uint32_t &out)
{
  const char *begin = token.c_str();
  char *end = nullptr;
  const uint64_t value = std::strtoul(begin, &end, 16);
  if (end == begin) {
    return false;
  }
  out = static_cast<uint32_t>(value);
  return true;
}

// Parse one Spike --log-commits line, keeping only retired instructions whose
// pc is inside [text_base, text_base + code_bytes). Format:
//   core   <n>: <priv> 0x<pc> (0x<insn>) [x<rd>  0x<value>]
// Exception/tval lines and out-of-range (bootrom / trap-handler) lines return
// nullopt.
inline std::optional<SpikeCommit>
parse_spike_commit(const std::string &line, uint32_t text_base,
                   uint32_t code_bytes)
{
  if (!line.starts_with("core")) {
    return std::nullopt;
  }
  std::istringstream stream(line);
  std::vector<std::string> tokens;
  std::string token;
  while (stream >> token) {
    tokens.push_back(token);
  }
  // tokens: [core, <n>:, <priv>, 0x<pc>, (0x<insn>), x<rd>, 0x<value>]
  if (tokens.size() < 5) {
    return std::nullopt;
  }
  uint32_t pc = 0;
  if (!parse_hex_u32(tokens[3], pc)) {
    return std::nullopt;
  }
  if (pc < text_base || pc >= text_base + code_bytes) {
    return std::nullopt;
  }
  SpikeCommit commit;
  commit.pc = pc;
  if (tokens.size() >= 7 && tokens[5].size() >= 2 && tokens[5][0] == 'x') {
    uint32_t value = 0;
    if (parse_hex_u32(tokens[6], value)) {
      char *reg_end = nullptr;
      const uint64_t reg_index =
          std::strtoul(tokens[5].c_str() + 1, &reg_end, 10);
      commit.has_reg_write = true;
      commit.rd = static_cast<uint8_t>(reg_index);
      commit.value = value;
    }
  }
  return commit;
}

inline std::vector<SpikeCommit>
run_spike_commits(const std::string &binary, const std::vector<uint8_t> &elf,
                  uint32_t text_base, uint32_t code_bytes)
{
  std::vector<SpikeCommit> commits;

  std::string tmpl = "/tmp/rv32_spike_XXXXXX";
  const int fd = mkstemp(tmpl.data());
  if (fd < 0) {
    return commits;
  }
  std::FILE *elf_file = fdopen(fd, "wb");
  if (elf_file == nullptr) {
    close(fd);
    std::remove(tmpl.c_str());
    return commits;
  }
  std::fwrite(elf.data(), 1, elf.size(), elf_file);
  std::fclose(elf_file);

  std::ostringstream cmd;
  cmd << "timeout " << kSpikeTimeoutSeconds << " '" << binary
      << "' --isa=rv32im --log-commits --pc=0x" << std::hex << text_base << " '"
      << tmpl << "' 2>&1";

  // Intentional subprocess: this test-only harness runs the external Spike
  // binary, with the command built from the developer-set RV32_ORACLE_BIN.
  // NOLINTNEXTLINE(bugprone-command-processor)
  std::FILE *pipe = popen(cmd.str().c_str(), "r");
  if (pipe != nullptr) {
    std::array<char, 512> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe) !=
           nullptr) {
      const std::optional<SpikeCommit> commit =
          parse_spike_commit(std::string(buf.data()), text_base, code_bytes);
      if (commit.has_value()) {
        commits.push_back(commit.value());
      }
    }
    pclose(pipe);
  }
  std::remove(tmpl.c_str());
  return commits;
}

// The static subset this adapter supports under the constant-offset pc mapping:
// non-empty code terminated by ecall; an aligned, in-range initial pc; zero
// initial registers and memory (Spike runs from reset); and register-only code
// -- no loads/stores, no jalr, no auipc or linking jal. Those excluded ops
// either address a data region this increment does not map, or write/consume a
// pc value that differs by Spike's base offset. Out-of-subset programs are
// rejected up front rather than silently mis-reconstructed from zero state.
inline bool
spike_register_only_supported(const DifferentialProgram &program)
{
  if (program.code.empty() || program.code.back() != 0x00000073U) {
    return false;
  }
  const auto code_bytes =
      static_cast<uint32_t>(program.code.size() * kInstructionBytes);
  if (program.initial_pc % kInstructionBytes != 0 ||
      program.initial_pc >= code_bytes) {
    return false;
  }
  for (const uint32_t reg : program.initial_regs) {
    if (reg != 0) {
      return false;
    }
  }
  for (const uint8_t byte : program.initial_memory) {
    if (byte != 0) {
      return false;
    }
  }
  // Reject the pc/memory-dependent ops the constant-offset mapping cannot model
  // (the terminal ecall at the back is excluded from this scan).
  for (size_t i = 0; i + 1 < program.code.size(); ++i) {
    const DecodeResult decoded = decode_instruction(program.code[i]);
    if (!decoded.ok) {
      return false;
    }
    switch (decoded.instruction.op) {
    case Op::kLb:
    case Op::kLh:
    case Op::kLw:
    case Op::kLbu:
    case Op::kLhu:
    case Op::kSb:
    case Op::kSh:
    case Op::kSw:
    case Op::kJalr:
    case Op::kAuipc:
      return false;
    case Op::kJal:
      if (decoded.instruction.rd != 0) {
        return false; // linking jal writes a pc-relative value
      }
      break;
    default:
      break;
    }
  }
  return true;
}

class SpikeOracle final : public Oracle {
public:
  explicit SpikeOracle(std::string binary) : binary_(std::move(binary)) {}

  [[nodiscard]] ExecTrace run(const DifferentialProgram &program) override
  {
    if (!valid_differential_program(program) ||
        !spike_register_only_supported(program)) {
      return invalid_differential_program_trace();
    }
    const uint32_t entry = kSpikeTextBase + program.initial_pc;
    const std::vector<uint8_t> elf =
        emit_rv32_elf(std::span<const uint32_t>(program.code.data(),
                                                program.code.size()),
                      entry, kSpikeTextBase);
    const auto code_bytes =
        static_cast<uint32_t>(program.code.size() * kInstructionBytes);
    const std::vector<SpikeCommit> commits =
        run_spike_commits(binary_, elf, kSpikeTextBase, code_bytes);

    // A program with computation before the terminal ecall must retire commits;
    // none means Spike never ran it (e.g. the subprocess failed or was not
    // found), so surface a trap rather than synthesizing a successful halt.
    if (commits.empty() && program.code.size() > 1) {
      return invalid_differential_program_trace();
    }

    // Reconstruct the architectural trace by accumulating register writes from
    // the zero reset state. The terminal ecall does not retire (it traps), so
    // it is appended as a synthetic halt step mirroring the Machine's ecall
    // semantics: pc advances past the ecall, registers/memory unchanged. The
    // ecall boundary is harness convention, not RISC-V semantics -- the real
    // computation comes entirely from Spike's commits.
    ExecTrace trace;
    std::array<uint32_t, kRegisterCount> regs = {};
    const std::vector<uint8_t> memory(program.memory_bytes, 0);
    const uint32_t ecall_pc = code_bytes - kInstructionBytes;

    for (size_t i = 0; i < commits.size(); ++i) {
      const SpikeCommit &commit = commits[i];
      ArchState before;
      before.pc = commit.pc - kSpikeTextBase;
      before.regs = regs;
      before.memory = memory;
      if (commit.has_reg_write && commit.rd != 0 &&
          commit.rd < kRegisterCount) {
        regs[commit.rd] = commit.value;
      }
      ArchState after;
      after.pc = (i + 1 < commits.size()) ? (commits[i + 1].pc - kSpikeTextBase)
                                          : ecall_pc;
      after.regs = regs;
      after.memory = memory;
      trace.steps.push_back(TraceStep{.result = StepResult::kContinue,
                                      .state_before = before,
                                      .state = after});
    }

    ArchState halt_before;
    halt_before.pc = ecall_pc;
    halt_before.regs = regs;
    halt_before.memory = memory;
    ArchState halt_after;
    halt_after.pc = ecall_pc + kInstructionBytes;
    halt_after.regs = regs;
    halt_after.memory = memory;
    halt_after.halted = true;
    trace.steps.push_back(TraceStep{.result = StepResult::kHalted,
                                    .state_before = halt_before,
                                    .state = halt_after});
    trace.final_result = StepResult::kHalted;
    return trace;
  }

private:
  std::string binary_;
};

} // namespace hogd::rv32
