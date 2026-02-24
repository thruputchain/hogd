// Test case demonstrating UB in BoundedStr::c_str()
//
// c_str() does:
//   self.ensure();           // guarantee 1 byte of capacity past b_.end
//   if (*self.b_.end)        // READ of indeterminate value -- UB in C++20/23
//     *self.b_.end = '\0';
//
// The memory at b_.end is allocated but never initialized.  Reading an
// indeterminate value is undefined behavior (pre-C++26).
//
// UBSan typically does NOT detect uninitialized reads (that's MSan's job),
// but we can try to provoke observable misbehavior.

#include <cassert>
#include <cstring>
#include <print>
#include <malloc/malloc.h>

#include "x23/vec.h"

using namespace x23;

// Poison the heap so that freshly-allocated memory is unlikely to be zero.
// This makes the UB in c_str() more likely to produce observable effects:
// the indeterminate read will see non-zero, and c_str() will write a NUL
// into the spare capacity, mutating memory outside the string's logical size.
static void poison_heap(size_t sz)
{
  // Allocate and fill with 0xFF, then free -- the next allocation of
  // similar size will often reuse this memory.
  void *p = malloc(sz);
  memset(p, 0xFF, sz);
  free(p);
}

int main()
{
  std::println("=== BoundedStr::c_str() UB demonstration ===\n");

  // Case 1: Basic indeterminate read
  {
    // Poison the heap so the spare byte is likely non-zero
    poison_heap(64);

    BoundedStr<32> s("hello", 5);
    // s has size=5, capacity >= 6 (after ensure() inside c_str)
    // b_.end points to s.data()[5], which is uninitialized

    std::println("Before c_str: size={}, data bytes at [size]...[size+3]:", s.size());
    // Peek at what's in the spare capacity (itself technically UB, but
    // shows the state before c_str does its thing).
    const char *raw = s.data();
    std::println("  [{:02x} {:02x} {:02x} {:02x}]",
                 (unsigned char)raw[5], (unsigned char)raw[6],
                 (unsigned char)raw[7], (unsigned char)raw[8]);

    const char *cs = s.c_str();
    std::println("After c_str:  returned=\"{}\"", cs);
    std::println("  byte at [size] is now: 0x{:02x}", (unsigned char)raw[5]);
    std::println("  (should be 0x00 -- c_str wrote NUL into spare capacity)\n");
  }

  // Case 2: String at exact capacity -- c_str() forces a reallocation
  // just to have room for the NUL sentinel
  {
    BoundedStr<8> s("abcdefgh", 8);
    assert(s.size() == 8);
    std::println("Case 2: size={} (at max), calling c_str()...", s.size());
    // c_str() calls ensure(1), which must grow capacity from 8 to 9.
    // But max_size() for BoundedStr<8> is 8, so ensure() -> realloc(9)
    // -> check_size(9) -> throws!
    try {
      const char *cs = s.c_str();
      std::println("  c_str returned: \"{}\" (no throw -- unexpected)", cs);
    } catch (const std::exception &e) {
      std::println("  THREW: {} (c_str is broken at max capacity!)", e.what());
    }
    std::println("");
  }

  // Case 3: The conditional write is also suspicious -- if the indeterminate
  // byte happens to be 0x00 already, c_str() skips the write.  This means
  // the behavior depends on uninitialized memory content, which is a classic
  // sign of UB.
  {
    // Try to get a zero byte in spare capacity
    BoundedStr<32> s;
    s.reserve(32);
    // Fill the buffer with zeros via a temporary
    s.resize(32, '\0');
    s.resize(5);
    // Now spare capacity from [5..31] should be destroyed but memory
    // likely still contains zeros
    memcpy(s.data(), "world", 5);

    const char *cs = s.c_str();
    std::println("Case 3: c_str on post-shrink string: \"{}\"", cs);
    std::println("  (NUL was already in spare capacity by luck, so 'works')\n");

    // Now poison that byte
    s.data()[5] = 'X';
    cs = s.c_str();
    std::println("  After poisoning spare byte: c_str returns \"{}\"", cs);
    std::println("  (c_str had to write NUL, overwriting our poison byte)\n");
  }

  std::println("=== Summary ===");
  std::println("The read of *b_.end in c_str() is UB: it reads an indeterminate");
  std::println("value from uninitialized memory.  UBSan does not detect this");
  std::println("class of UB -- use MSan (-fsanitize=memory) instead.");
  std::println("Additionally, c_str() throws when size() == max_size().");
}
