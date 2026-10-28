// What generated crates rely on from x23c's Rust output.  check.sh
// generates Rust from this and type-checks it with the uses in src/lib.rs.

const MAX_BATCH = 4;
const SIG_SIZE = 64;

enum Status {
  OK = 0,
  REJECTED = 1
};

// #[rust(newtype)]
typedef opaque Hash[32];

struct Tx {
  unsigned hyper seq;
  opaque memo<16>;
  string note<32>;
  Hash hash;
  opaque digest[32];
  opaque sig[SIG_SIZE];
};

typedef Tx TxBatch<MAX_BATCH>;
typedef unsigned hyper Amount;

// A schema type with the name a generic parameter would most likely take.
typedef int T;

union Outcome switch (Status status) {
case OK:
  Hash hash;
case REJECTED:
  void;
};

struct Receipt {
  Outcome outcome;
  Tx *tx;
};

program check {
  version check_v1 {
    void ping(void) = 0;
    int add(int, unsigned hyper) = 1;
    Outcome submit(TxBatch, bool) = 2;
    Receipt credit(Amount, Tx) = 3;
    bool verify(Hash) = 4;
    TxBatch pending(void) = 5;
    T scale(T) = 6;
  } = 1;
} = 0x20000003;
