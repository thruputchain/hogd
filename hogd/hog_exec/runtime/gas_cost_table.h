#pragma once

#include <array>
#include <cstdint>

namespace hogd {

/**
 * Table of gas costs for various operations.
 *
 * RV32 artifacts commit to these: kSyscallGasCostSchedule is folded into
 * rv32::kGasScheduleDigest and pinned in artifact.h. Changing any cost/name/
 * order is a coordinated gas-schedule update: update the pinned digest/version
 * deliberately and regenerate committed RV32 artifacts/lowerer constants.
 *
 * The costs are measured by the RV32 Gas Calibration workflow, which times the
 * production syscall_entry path and converts nanoseconds to gas via the opcode
 * calibration's baseline instruction. See docs/rv32/rv32-gas-calibration.md.
 *
 * Every constant the cost functions read must be an entry in
 * kSyscallGasCostSchedule, since that is what the digest hashes; a number that
 * changes a contract's bill while the digest sits still is the failure the
 * digest exists to prevent. Block geometry is the exception: it is fixed by the
 * hash functions, so it is static_asserted rather than hashed.
 */

// Every syscall pays this: the crossing itself -- ecall barrier, argument
// parsing, alias guard, coroutine frame -- which is what the cheapest syscalls
// almost entirely consist of. The ecall instruction already charges 1 gas, so
// this is the remainder.
constexpr static uint64_t kSyscallBaseCost = 50;

// Per compression block, which is the unit SHA-256 works in. Charged across
// the whole sweep the billed-to-measured ratio stays within 1.02x, which is
// the check that matters; the fit's R^2 is not, since a sweep spanning 1 to
// 513 blocks would report ~1.0 for any roughly linear model. Pricing the block
// also avoids a per-byte rate, which is a fraction needing a divisor -- one
// more billing-relevant constant to keep in the digest.
//
// There is no separate per-call base. Every point in the sweep hashes at least
// one block, since SHA-256 always compresses its padding, so an intercept is an
// extrapolation to a block count no call can have. What the measurement leaves
// once the crossing and that first block are accounted for is ~2 ns of a 117 ns
// call, which is inside the fit's own residual.
constexpr static uint64_t kSHA256HashBlockCost = 22;

// What touching a hash stream costs on top of the blocks it compresses, paid by
// SHA256Update and SHA256Copy. Both reach the frame's stream table, and both
// create an entry in it when the destination slot does not exist yet -- an
// allocation with no compression attached, so no block term bills it. A
// zero-byte reset update creates a stream and hashes nothing at all.
//
// Charged on every such call, not only the ones that allocate. The alternative
// makes the price depend on whether the slot happened to exist, which is a
// thing about the frame's state rather than about the call; the flat charge
// keeps every cost in this table a function of its arguments. It overbills a
// repeat touch, which measures near the crossing alone, by well under 2x --
// the direction the opcode schedule already rounds, and far inside the spread
// this schedule replaced.
//
// The stream table's size limit is max_sha256_buffers, which is network config
// rather than a protocol constant, so this cannot lean on the limit being
// small. It does not need to: the cost per creation is flat in occupancy
// (measured to 32768), so an unordered_map insert stays amortized O(1) however
// large a chain sets it.
constexpr static uint64_t kSHA256StreamCost = 26;

// ed25519, which this schedule prices for the first time. The curve arithmetic
// dominates; the block term covers hashing the message, and is needed because
// the syscall puts no bound on its length.
constexpr static uint64_t kVerifySignatureBaseCost = 17750;
constexpr static uint64_t kVerifySignatureBlockCost = 131;

constexpr static uint64_t kNoSyscallCost = 10;
constexpr static uint64_t kGenericBadSyscallCost = 10;

// Gas charged per element (modified storage item, event, deployment, or
// returned auth token) merged from a successful child frame into its parent by
// InvocationFrame::commit_successful_child_frame(). The merge does one map
// insert or append per element -- work proportional to the child's write set,
// redone at every successful ancestor -- so without a charge a leaf's write set
// can be re-merged through each ancestor for free (hogd issue #3819).
//
// This is frame-completion accounting rather than a guest syscall, but it is a
// per-element billing constant that changes a contract's bill, so it belongs in
// the schedule the digest hashes: otherwise a change here would silently
// re-meter deployed contracts, the exact failure the digest exists to prevent.
// Priced at the syscall-crossing cost, a conservative upper bound on one map
// operation pending a dedicated calibration.
constexpr static uint64_t kFrameMergeElementCost = kSyscallBaseCost;

// Block geometry, fixed by the hash functions rather than chosen, so
// static_asserted rather than hashed into the schedule. Padding is a 0x80 byte
// plus a length field: 9 bytes for SHA-256, 17 for SHA-512.
constexpr static uint64_t kSHA256BlockBytes = 64;
constexpr static uint64_t kSHA256PadBytes = 9;
static_assert(kSHA256BlockBytes == 64 && kSHA256PadBytes == 9);
// ed25519 hashes R || A || message with SHA-512.
constexpr static uint64_t kSHA512BlockBytes = 128;
constexpr static uint64_t kSHA512PadBytes = 17;
constexpr static uint64_t kEd25519HashedPrefixBytes = 64;
static_assert(kSHA512BlockBytes == 128 && kSHA512PadBytes == 17);

// NOLINTBEGIN(readability-identifier-naming)

// Blocks a hash compresses, padding included. One helper per hash rather than
// one taking the geometry as arguments: three interchangeable uint64_t
// parameters is a call that transposes silently. Guarded against overflow on a
// length near the top of the range.
constexpr static auto SHA256BlockCount =
    [](uint64_t message_bytes) -> uint64_t {
  if (message_bytes > UINT64_MAX - kSHA256PadBytes)
    return UINT64_MAX / kSHA256BlockBytes;
  return (message_bytes + kSHA256PadBytes + kSHA256BlockBytes - 1) /
         kSHA256BlockBytes;
};

constexpr static auto SHA512BlockCount =
    [](uint64_t message_bytes) -> uint64_t {
  if (message_bytes > UINT64_MAX - kSHA512PadBytes)
    return UINT64_MAX / kSHA512BlockBytes;
  return (message_bytes + kSHA512PadBytes + kSHA512BlockBytes - 1) /
         kSHA512BlockBytes;
};

// Saturating: a length chosen to overflow must bill the maximum, not wrap.
constexpr static auto SaturatingCost = [](uint64_t base, uint64_t per_block,
                                          uint64_t blocks) -> uint64_t {
  if (per_block != 0 && blocks > (UINT64_MAX - base) / per_block)
    return UINT64_MAX;
  return base + (blocks * per_block);
};

constexpr static auto SHA256HashCost = [](uint64_t bytes_len) -> uint64_t {
  return SaturatingCost(0, kSHA256HashBlockCost, SHA256BlockCount(bytes_len));
};

// Whole blocks only: a partial tail stays buffered and is charged by whichever
// later call completes it. The carry this under-counts by is at most one block
// per call, far less than the crossing that call already paid.
constexpr static auto SHA256UpdateCost = [](uint64_t bytes_len) -> uint64_t {
  return SaturatingCost(kSHA256StreamCost, kSHA256HashBlockCost,
                        bytes_len / kSHA256BlockBytes);
};

// Copy compresses nothing; the stream it clones into is the whole cost.
constexpr static uint64_t kSHA256CopyCost = kSHA256StreamCost;

// Finalize compresses the padding block the streaming path has not yet done.
// It creates no stream -- it erases one -- so it pays no stream cost: the
// create-side call already paid for both halves of the slot's life.
constexpr static uint64_t kSHA256FinalizeCost = kSHA256HashBlockCost;

constexpr static auto VerifySignatureCost =
    [](uint64_t message_len) -> uint64_t {
  const uint64_t hashed = message_len > UINT64_MAX - kEd25519HashedPrefixBytes
                              ? UINT64_MAX - kEd25519HashedPrefixBytes
                              : message_len + kEd25519HashedPrefixBytes;
  return SaturatingCost(kVerifySignatureBaseCost, kVerifySignatureBlockCost,
                        SHA512BlockCount(hashed));
};
// NOLINTEND(readability-identifier-naming)

struct SyscallGasCostEntry {
  // `u8"..."` literal: folded into the RV32 gas-schedule digest (an artifact
  // wire constant), so the hashed bytes must be the standard's UTF-8 code units
  // rather than implementation-defined ordinary-literal bytes that depend on
  // the execution character set (`-fexec-charset`). See hogd issue #2689.
  const char8_t *name;
  uint64_t cost;
};

// Every constant the cost functions above read. Syscalls not named here charge
// kSyscallBaseCost and nothing more.
constexpr std::array<SyscallGasCostEntry, 8> kSyscallGasCostSchedule = {{
    {.name = u8"SyscallBase", .cost = kSyscallBaseCost},
    {.name = u8"SHA256HashBlock", .cost = kSHA256HashBlockCost},
    {.name = u8"SHA256Stream", .cost = kSHA256StreamCost},
    {.name = u8"VerifySignatureBase", .cost = kVerifySignatureBaseCost},
    {.name = u8"VerifySignatureBlock", .cost = kVerifySignatureBlockCost},
    {.name = u8"NoSyscall", .cost = kNoSyscallCost},
    {.name = u8"GenericBadSyscall", .cost = kGenericBadSyscallCost},
    {.name = u8"FrameMergeElement", .cost = kFrameMergeElementCost},
}};

} // namespace hogd
