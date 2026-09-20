const LIMIT = 16;

enum Status { BAD = -1, OK = 0, BUSY = 1 };

typedef opaque Digest[16];
typedef string Name<LIMIT>;

// A newtype whose Debug is hand-written downstream.  Both markers share one
// trailing comment line here, as they do in the protocol definitions.
typedef opaque Fingerprint[16]; // #[rust(newtype)] #[rust(no_debug)]

struct Record {
  int id;
  unsigned int count;
  hyper signed_big;
  unsigned hyper big;
  bool enabled;
  Name name;
  opaque payload<LIMIT>;
  Digest digest;
  Status status;
  int fixed[2];
  int values<>;
  Record *next;
};

union Reply switch (Status status) {
case OK:
  Record record;
case BUSY:
case BAD:
  string message<>;
default:
  void;
};

program sample {
  version sample_v1 {
    void ping(void) = 0;
    Reply lookup(int) = 1;
    int add(int, int) = 2;
  } = 1;
} = 0x20000001;
