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
 * The costs below are measured rather than placeholders. They come from the
 * RV32 Gas Calibration workflow on the bench pool (hog10, run 33469060485),
 * which times the production syscall_entry path and converts nanoseconds into
 * gas via the baseline instruction the opcode calibration measured on the same
 * host -- 1.6241 ns at the time these were set. See
 * docs/rv32/rv32-gas-calibration.md.
 *
 * EVERY constant that the cost functions below read must be an entry in
 * kSyscallGasCostSchedule, because that array is what the digest hashes. A
 * number that changes what a contract is billed while the digest sits still is
 * precisely the failure the digest exists to prevent. The one exception is the
 * block geometry -- kSHA256BlockBytes and friends -- which is not policy but
 * arithmetic fixed by the hash functions themselves; those are static_asserted
 * rather than hashed, and changing one would be a bug rather than a repricing.
 */

// Every syscall pays this, whatever it then does. What is being priced is the
// crossing itself -- the ecall barrier, argument parsing, the alias guard, the
// coroutine frame -- and the measurement is that the cheapest syscalls all land
// within a few percent of each other because that crossing, not the handler, is
// what they cost. Measured at 82.0 ns for the dearest of them.
//
// One gas of it is already charged as the ecall instruction by the RV32
// instruction schedule, so this is the remainder and the two sum to the
// crossing.
constexpr static uint64_t kSyscallBaseCost = 50;

// SHA-256 compresses fixed 64-byte blocks, and the measurement follows that
// shape exactly: 34.53 ns per block at R^2 = 1.000000 across a 0..32KiB sweep.
// Pricing per block rather than per byte is not just tidier, it is what removes
// the need for a rate denominator -- a per-byte rate of 0.34 gas cannot be
// written as an integer, and the divisor that would fix it is exactly the kind
// of billing-relevant constant that is easy to leave out of the digest.
constexpr static uint64_t kSHA256HashBlockCost = 22;
// What the syscall costs beyond the crossing and the blocks it compresses.
// Small because the block term now carries the work that a per-byte model had
// to hide in the base.
constexpr static uint64_t kSHA256HashBaseCost = 2;

// ed25519 verification, which was charged nothing at all until this schedule
// while doing ~17.6k gas of real work. The constant dominates: the curve
// arithmetic is ~28.6us and the message contributes ~0.2us per 128-byte
// SHA-512 block. The block term still has to be there, because the syscall
// puts no bound on the message length.
constexpr static uint64_t kVerifySignatureBaseCost = 17750;
constexpr static uint64_t kVerifySignatureBlockCost = 131;

constexpr static uint64_t kNoSyscallCost = 10;
constexpr static uint64_t kGenericBadSyscallCost = 10;

// Block geometry, fixed by the hash functions rather than chosen. Not hashed
// into the schedule: these are not prices, and a wrong value here would be a
// bug in the cost model rather than a repricing of it.
constexpr static uint64_t kSHA256BlockBytes = 64;
// SHA-256 padding is a single 0x80 byte followed by zeros and an 8-byte
// big-endian bit length, so a message needs 9 bytes of room beyond itself and
// the block count is ceil((len + 9) / 64).
constexpr static uint64_t kSHA256PadBytes = 9;
static_assert(kSHA256BlockBytes == 64 && kSHA256PadBytes == 9);
// SHA-512 for ed25519, which hashes R || A || message: 128-byte blocks, and a
// 17-byte padding tail (0x80 plus a 16-byte length).
constexpr static uint64_t kSHA512BlockBytes = 128;
constexpr static uint64_t kSHA512PadBytes = 17;
constexpr static uint64_t kEd25519HashedPrefixBytes = 64;
static_assert(kSHA512BlockBytes == 128 && kSHA512PadBytes == 17);

// NOLINTBEGIN(readability-identifier-naming)

// Blocks a hash of `len` bytes compresses, including the padding block. Written
// to avoid overflowing on a length near the top of the range rather than to be
// clever: the addition is the only place it could, so it is the only place
// guarded.
constexpr static auto HashBlockCount = [](uint64_t len, uint64_t block_bytes,
                                          uint64_t pad_bytes) -> uint64_t {
  if (len > UINT64_MAX - pad_bytes)
    return UINT64_MAX / block_bytes;
  return (len + pad_bytes + block_bytes - 1) / block_bytes;
};

// Saturating, so a length chosen to overflow the product bills the maximum
// rather than wrapping to something cheap.
constexpr static auto SaturatingCost = [](uint64_t base, uint64_t per_block,
                                          uint64_t blocks) -> uint64_t {
  if (per_block != 0 && blocks > (UINT64_MAX - base) / per_block)
    return UINT64_MAX;
  return base + (blocks * per_block);
};

constexpr static auto SHA256HashCost = [](uint64_t bytes_len) -> uint64_t {
  return SaturatingCost(kSHA256HashBaseCost, kSHA256HashBlockCost,
                        HashBlockCount(bytes_len, kSHA256BlockBytes,
                                       kSHA256PadBytes));
};

// Update compresses only the whole blocks it completes; a partial tail stays
// buffered for a later call and is charged when that call completes it. Whole
// blocks only, so this never charges for work not yet done -- and the buffered
// carry it can under-count by is at most one block per call, far less than the
// crossing that same call already paid.
constexpr static auto SHA256UpdateCost = [](uint64_t bytes_len) -> uint64_t {
  return SaturatingCost(0, kSHA256HashBlockCost, bytes_len / kSHA256BlockBytes);
};

// Finalize compresses the padding block the streaming path has not yet done.
constexpr static uint64_t kSHA256FinalizeCost = kSHA256HashBlockCost;

// Copy clones a hasher's state: no compression, and the crossing covers it.
constexpr static uint64_t kSHA256CopyCost = 0;

constexpr static auto VerifySignatureCost =
    [](uint64_t message_len) -> uint64_t {
  const uint64_t hashed = message_len > UINT64_MAX - kEd25519HashedPrefixBytes
                              ? UINT64_MAX - kEd25519HashedPrefixBytes
                              : message_len + kEd25519HashedPrefixBytes;
  return SaturatingCost(kVerifySignatureBaseCost, kVerifySignatureBlockCost,
                        HashBlockCount(hashed, kSHA512BlockBytes,
                                       kSHA512PadBytes));
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
// kSyscallBaseCost and nothing more, which after this schedule is a price
// rather than the zero it used to be.
constexpr std::array<SyscallGasCostEntry, 8> kSyscallGasCostSchedule = {{
    {.name = u8"SyscallBase", .cost = kSyscallBaseCost},
    {.name = u8"SHA256HashBase", .cost = kSHA256HashBaseCost},
    {.name = u8"SHA256HashBlock", .cost = kSHA256HashBlockCost},
    {.name = u8"SHA256Copy", .cost = kSHA256CopyCost},
    {.name = u8"VerifySignatureBase", .cost = kVerifySignatureBaseCost},
    {.name = u8"VerifySignatureBlock", .cost = kVerifySignatureBlockCost},
    {.name = u8"NoSyscall", .cost = kNoSyscallCost},
    {.name = u8"GenericBadSyscall", .cost = kGenericBadSyscallCost},
}};

} // namespace hogd
