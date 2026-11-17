#pragma once

#include <array>
#include <cassert>
#include <cstdint>

#include "hogd/rust/crypto/bindings.h"

namespace hogd {

/**
 * Table of gas costs for various operations.
 *
 * These are consensus: validators must agree on them. They belong to this
 * build, not to an artifact, which declares only the syscall API version it
 * was built against, so a new price needs no change to any artifact or
 * lowerer. rv32_vm_test spells every entry out, so a change here is deliberate.
 *
 * The costs are measured by the RV32 Gas Calibration workflow, which times the
 * production syscall_entry path and converts nanoseconds to gas via the opcode
 * calibration's baseline instruction. See rv32/gas_calibration/MEASUREMENTS.md.
 *
 * Every constant the cost functions read must be an entry in
 * kSyscallGasCostSchedule, since that is what the test pins; a number that
 * changes a contract's bill with no pinned entry moving is the failure the
 * schedule exists to prevent. Block geometry is the exception: it is fixed by
 * the hash functions, so it is static_asserted rather than listed.
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

// Per-call stream-table cost for SHA256Update and SHA256Copy, in addition to
// compression blocks. Charge every call, including repeat touches, to cover
// stream creation without making this component depend on slot occupancy.
// Creation cost was flat over the measured occupancies through 32768 streams;
// max_sha256_buffers is configurable, so larger tables remain unmeasured.
constexpr static uint64_t kSHA256StreamCost = 26;

// ed25519, which this schedule prices for the first time. The curve arithmetic
// dominates; the block term covers hashing the message, and is needed because
// the syscall puts no bound on its length.
constexpr static uint64_t kVerifySignatureBaseCost = 17750;
constexpr static uint64_t kVerifySignatureBlockCost = 131;

// ML-DSA-65 (pure, empty context), priced from RV32 Gas Calibration run
// 39809791818 on hog10 (gas unit 1.6219 ns, no margin). The entry-row sweep fit
// 75644.9 ns + 258.0 ns per 136-byte SHAKE256 block: 46640 gas at zero blocks
// once the ecall is taken off, of which kSyscallBaseCost is 50, and 160 per
// block. The base is then raised to 46592 so no measured length is
// undercharged: with the syscall base, 0 and 64 bytes (1 block) bill 46802
// against a measured 46802 and 46734, and 1024 bytes (9 blocks) bills 48082
// against 48071. The block term covers hashing the message into mu, which the
// syscall does not bound; the lattice arithmetic is the rest.
constexpr static uint64_t kVerifySignatureMlDsa65BaseCost = 46592;
constexpr static uint64_t kVerifySignatureMlDsa65BlockCost = 160;

// Per-call NNInt access cost, in addition to crossing and database-load costs.
// The flat charge covers map access, first-touch view creation, and child-frame
// merge. At 4096 keys the measured components were ~47-55 gas for access,
// ~93 for a first read or ~34 for a first write, and ~32 per merged item.
// The allowance prepays child-frame merge because finish_frame has no meter.
// Failed frames pay this allowance even though their items are discarded.
//
// Repeat accesses pay the same component, about 1.6-2x their measured cost at
// 1-4096 keys. Total billing still depends on transaction-local database loads.
// First-touch cost rose to ~234 gas at 65536 keys and ~529 at 262144, beyond
// this allowance. See rv32/gas_calibration/MEASUREMENTS.md for measurement
// limits; reachability of these occupancies is not established.
constexpr static uint64_t kStorageAccessCost = 150;

// One read from hog_db, charged wherever an item view loads.
//
// A load is the transaction's first touch of that key: the view is created
// from the frame walk and populated once, and every later touch in the same
// transaction finds it. Which makes this deterministic -- it depends only on
// this transaction's own earlier syscalls, which every validator replays the
// same way. Whether the load then finds the block's shared snapshot cache warm
// does NOT, since that turns on which transactions the validator scheduled
// first, so the charge is the worst case: every load pays as though it went to
// the database.
//
// 55.2 us measured on hog10, +/- 5.4. This is the dear end of that rather than
// the mean, because the spread is ~10% where a warm row's is 0.2% -- a wall
// clock RPC varies in a way more repetitions do not remove. It lands a cold
// NNInt read at a round 37500 with the base and the access, the way
// kStorageAccessCost lands a warm one at 200.
//
// It is a floor in one respect: the measurement's hog_db is in-process over a
// local socket, so a chain running its executor and database on different
// hosts pays a hop this does not.
constexpr static uint64_t kDBRPCCost = 37300;

// What copying a bytestring costs, per block, on top of the storage access
// every bytestring call already pays. A block rather than a per-byte rate
// because the measured rate is a fraction of a gas unit, and a fraction needs
// a divisor. 512 is chosen rather than forced by a hash's geometry, so it is
// hashed into the schedule with the costs. Two constants because a write moves
// the bytes twice and measures almost exactly twice a read's rate; each is
// sized to the dearest band its sweep measured, the per-byte cost not being
// constant across it.
//
// See rv32/gas_calibration/MEASUREMENTS.md, the bytestring measurements.
constexpr static uint64_t kBytestringBlockBytes = 512;
constexpr static uint64_t kBytestringWriteBlockCost = 13;
constexpr static uint64_t kBytestringReadBlockCost = 7;

constexpr static uint64_t kNoSyscallCost = 10;
constexpr static uint64_t kGenericBadSyscallCost = 10;

// Initial price from two M4 Max production-entry sweeps, with >=15% headroom
// over sampled valid-input row medians.
// Calibration: https://linear.app/grndhg/issue/GH-617
constexpr uint64_t kRistretto255MsmBaseCost = 30'000;
constexpr uint64_t kRistretto255MsmTermCost = 13'000;

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
// ML-DSA-65 verification absorbs tr || 0x00 || 0x00 || message into SHAKE256,
// whose rate is 136 bytes; its pad10*1 padding takes at least one byte.
constexpr static uint64_t kSHAKE256RateBytes = 136;
constexpr static uint64_t kSHAKE256PadBytes = 1;
constexpr static uint64_t kMlDsa65HashedPrefixBytes = 64 + 2;
static_assert(kSHAKE256RateBytes == 136 && kSHAKE256PadBytes == 1);

// The largest length a cost function may be handed. Every argument they price
// is a guest span length, which rv32::kMemoryCeilingBytes caps at 16 MiB, so
// anything past this is a caller passing something that is not a length -- a
// bug to catch, not a case to price.
constexpr static uint64_t kMaxCostBytes = uint64_t{1} << 32;

// Costs a syscall computes from its arguments, as opposed to the flat
// constants above.
struct GasCost {
  static constexpr uint64_t ristretto255_msm(uint64_t terms)
  {
    // The caller bounds the term count before computing the price. Even the
    // largest accepted input fits far below the gas type's maximum.
    assert(terms <= RISTRETTO255_MSM_MAX_TERMS);
    static_assert(RISTRETTO255_MSM_MAX_TERMS <=
                  (UINT64_MAX - kRistretto255MsmBaseCost) /
                      kRistretto255MsmTermCost);
    return kRistretto255MsmBaseCost + (terms * kRistretto255MsmTermCost);
  }

  // Blocks a hash compresses, padding included. One helper per hash rather
  // than one taking the geometry as arguments: three interchangeable uint64_t
  // parameters is a call that transposes silently.
  static constexpr uint64_t sha256_blocks(uint64_t message_bytes)
  {
    assert(message_bytes <= kMaxCostBytes);
    return (message_bytes + kSHA256PadBytes + kSHA256BlockBytes - 1) /
           kSHA256BlockBytes;
  }

  static constexpr uint64_t sha512_blocks(uint64_t message_bytes)
  {
    assert(message_bytes <= kMaxCostBytes);
    return (message_bytes + kSHA512PadBytes + kSHA512BlockBytes - 1) /
           kSHA512BlockBytes;
  }

  static constexpr uint64_t shake256_blocks(uint64_t message_bytes)
  {
    assert(message_bytes <= kMaxCostBytes);
    return (message_bytes + kSHAKE256PadBytes + kSHAKE256RateBytes - 1) /
           kSHAKE256RateBytes;
  }

  static constexpr uint64_t sha256_hash(uint64_t bytes_len)
  {
    return kSHA256HashBlockCost * sha256_blocks(bytes_len);
  }

  // Whole blocks only: a partial tail stays buffered and is charged by
  // whichever later call completes it. The carry this under-counts by is at
  // most one block per call, far less than the crossing that call already
  // paid.
  static constexpr uint64_t sha256_update(uint64_t bytes_len)
  {
    assert(bytes_len <= kMaxCostBytes);
    return kSHA256StreamCost +
           (kSHA256HashBlockCost * (bytes_len / kSHA256BlockBytes));
  }

  // Rounded up, with an empty bytestring costing none: it copies nothing, and
  // its storage access is charged anyway.
  static constexpr uint64_t bytestring_blocks(uint64_t bytes_len)
  {
    assert(bytes_len <= kMaxCostBytes);
    return (bytes_len + kBytestringBlockBytes - 1) / kBytestringBlockBytes;
  }

  // Charged on the bytes copied: the value handed to a write, and for a read
  // the bytes that reach the guest rather than the buffer it offered.
  static constexpr uint64_t bytestring_set(uint64_t bytes_len)
  {
    return kStorageAccessCost +
           (kBytestringWriteBlockCost * bytestring_blocks(bytes_len));
  }

  static constexpr uint64_t bytestring_get(uint64_t bytes_len)
  {
    return kStorageAccessCost +
           (kBytestringReadBlockCost * bytestring_blocks(bytes_len));
  }

  // Priced on the bytes that reach the guest, in bytestring read blocks. No
  // storage-access term: this is the entry copy alone.
  static constexpr uint64_t retrocausal_queue_consume(uint64_t bytes_copied)
  {
    return kBytestringReadBlockCost * bytestring_blocks(bytes_copied);
  }

  static constexpr uint64_t verify_signature(uint64_t message_len)
  {
    assert(message_len <= kMaxCostBytes);
    return kVerifySignatureBaseCost +
           (kVerifySignatureBlockCost *
            sha512_blocks(message_len + kEd25519HashedPrefixBytes));
  }

  static constexpr uint64_t verify_signature_mldsa65(uint64_t message_len)
  {
    assert(message_len <= kMaxCostBytes);
    return kVerifySignatureMlDsa65BaseCost +
           (kVerifySignatureMlDsa65BlockCost *
            shake256_blocks(message_len + kMlDsa65HashedPrefixBytes));
  }
};

struct SyscallGasCostEntry {
  // `u8"..."` literal, so the bytes are the standard's UTF-8 code units
  // rather than implementation-defined ordinary-literal bytes that depend on
  // the execution character set (`-fexec-charset`). See hogd issue #2689.
  const char8_t *name;
  uint64_t cost;
};

// Every constant the cost functions above read. Syscalls not named here charge
// kSyscallBaseCost and nothing more.
constexpr std::array<SyscallGasCostEntry, 18> kSyscallGasCostSchedule = {{
    {.name = u8"SyscallBase", .cost = kSyscallBaseCost},
    {.name = u8"SHA256HashBlock", .cost = kSHA256HashBlockCost},
    {.name = u8"SHA256Stream", .cost = kSHA256StreamCost},
    {.name = u8"VerifySignatureBase", .cost = kVerifySignatureBaseCost},
    {.name = u8"VerifySignatureBlock", .cost = kVerifySignatureBlockCost},
    {.name = u8"StorageAccess", .cost = kStorageAccessCost},
    {.name = u8"DBRPC", .cost = kDBRPCCost},
    {.name = u8"BytestringBlockBytes", .cost = kBytestringBlockBytes},
    {.name = u8"BytestringWriteBlock", .cost = kBytestringWriteBlockCost},
    {.name = u8"BytestringReadBlock", .cost = kBytestringReadBlockCost},
    {.name = u8"NoSyscall", .cost = kNoSyscallCost},
    {.name = u8"GenericBadSyscall", .cost = kGenericBadSyscallCost},
    {.name = u8"VerifySignatureMlDsa65Base",
     .cost = kVerifySignatureMlDsa65BaseCost},
    {.name = u8"VerifySignatureMlDsa65Block",
     .cost = kVerifySignatureMlDsa65BlockCost},
    {.name = u8"Ristretto255MsmBase", .cost = kRistretto255MsmBaseCost},
    {.name = u8"Ristretto255MsmTerm", .cost = kRistretto255MsmTermCost},
    {.name = u8"Ristretto255MsmTermBytes", .cost = RISTRETTO255_MSM_TERM_BYTES},
    {.name = u8"Ristretto255MsmMaxTerms", .cost = RISTRETTO255_MSM_MAX_TERMS},
}};

} // namespace hogd
