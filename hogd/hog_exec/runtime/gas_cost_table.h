#pragma once

#include <array>
#include <cassert>
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

// What an NNInt storage syscall costs on top of the crossing. Charged by
// NonnegativeIntGet and NonnegativeIntSetAdd alike, on every call.
//
// This covers three measured costs at once, and deliberately charges all of
// them every time:
//
//   the access itself   reaching the frame's item map, hashing the key,
//                       reading or staging the value.  ~47-55 gas.
//   the first touch     a key this transaction has not seen before misses the
//                       active frame, walks its parents and creates a view,
//                       loading from the snapshot cache on a read.  ~93 gas
//                       for a read and ~34 for a write, at four thousand keys
//                       in the frame.
//   the commit          a successful child frame merges each staged item into
//                       its parent, in frame completion rather than in any
//                       syscall.  ~32 gas per item.
//
// Charging the first touch on every access, not only the first, is the same
// trade kSHA256StreamCost makes above and for the same reason: the alternative
// makes the price depend on whether the frame happened to hold the key
// already, which is a thing about the frame's state rather than about the
// call. The flat charge keeps every cost in this table a function of its
// arguments, and it keeps a contract's bill independent of its own execution
// history, which is what makes gas estimable before running it.
//
// It overbills a repeat touch by 1.6-2x at the key counts anything realistic
// reaches -- measured 1.64x at one key in the frame, 1.60x at 256, 1.96x at
// 4096 -- which is the direction this schedule already rounds and well inside
// the spread it replaced.
//
// Prepaying the commit on every write is what lets it be charged at all: the
// merge happens in InvocationFrameStack::finish_frame, which has no gas meter
// and no syscall running. Charging it at the write overbills a frame that
// later fails and never merges, which is the safe direction -- the staging
// work happened either way.
//
// 150 covers the dearest sum of the three (a read: 47 + 93) with room, and
// lands the total at a round 200 with the base. Writes come to 121 by the same
// arithmetic, close enough that one constant serves both; two would be false
// precision in a digest, and would give a contract a reason to prefer one call
// over another for pricing rather than for what it needs.
//
// What this still does not cover is a frame holding far more than a few
// thousand keys: the first touch rises to ~234 gas at 65536 and ~529 at
// 262144 as the frame's map outgrows cache. What bounds that is the gas a
// transaction must burn to reach those key counts. See
// docs/rv32/rv32-gas-calibration.md.
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
// See docs/rv32/rv32-gas-calibration.md, "Bytestrings: the length axis".
constexpr static uint64_t kBytestringBlockBytes = 512;
constexpr static uint64_t kBytestringWriteBlockCost = 13;
constexpr static uint64_t kBytestringReadBlockCost = 7;

constexpr static uint64_t kNoSyscallCost = 10;
constexpr static uint64_t kGenericBadSyscallCost = 10;

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

// The largest length a cost function may be handed. Every argument they price
// is a guest span length, which rv32::kMaxMemoryBytes caps at 64 KiB, so
// anything past this is a caller passing something that is not a length -- a
// bug to catch, not a case to price.
constexpr static uint64_t kMaxCostBytes = uint64_t{1} << 32;

// Costs a syscall computes from its arguments, as opposed to the flat
// constants above.
struct GasCost {
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

  static constexpr uint64_t verify_signature(uint64_t message_len)
  {
    assert(message_len <= kMaxCostBytes);
    return kVerifySignatureBaseCost +
           (kVerifySignatureBlockCost *
            sha512_blocks(message_len + kEd25519HashedPrefixBytes));
  }
};

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
constexpr std::array<SyscallGasCostEntry, 12> kSyscallGasCostSchedule = {{
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
}};

} // namespace hogd
