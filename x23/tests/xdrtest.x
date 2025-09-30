
enum senum {
  SE_NEGATIVE = -1,
  SE_POSITIVE = 1
};

union sunion switch (senum d) {
  case SE_NEGATIVE:
    bool neg;
  case SE_POSITIVE:
    int pos;
  default:
    void;
};

union sunion_no_void switch (senum d) {
  case SE_NEGATIVE:
    bool neg;
  case SE_POSITIVE:
    int pos;
};

union uunion switch (unsigned int d) {
  case 1:
    bool one;
  case 2:
    int two;
  case 3:
    double three;
  case 4:
    string four<>;
};

struct test_recursive {
  string elem<>;
  test_recursive *next;
  test_recursive nextvec<>;
};

struct contains_test_recursive {
  test_recursive t;
};

struct fix_4 {
  int i;
};

struct fix_12 {
  int i;
  double d;
};

struct fix_16 {
  struct {
    fix_4 f4;
    fix_12 f12;
  } x;
};

union u_4_12 switch (int which) {
 case 4:
   fix_4 f4;
 case 12:
   fix_12 f12;
};

typedef fix_12 v12<>;

union uptr switch (bool b) {
  case FALSE:
    void;
  case TRUE:
    int *val;
};

enum color {
  RED = 1,
  REDDER = 2,
  REDDEST = 3
};

union coloru switch (color c) {
case RED:
     string r<>;
case REDDER:
     hyper rr;
case REDDEST:
     double rrr;
};

program myprog {
  version myvers {
    void null(void) = 0;
    int takes_int(int) = 1;
    int takes_two_ints(int, int) = 2;
    coloru takes_recursive(test_recursive, fix_12) = 5;
  } = 1;
} = 3123123;


enum StorageDeltaType {
  kDeleteLast = 0,
  kBytestringSet = 1,
  kNonnegativeInt64SetAdd = 2
};

const kBytestringMaxLen = 4096;

const kFoo = 0xABCD'EF'01;
const kBar = 62'500'000'000;

typedef opaque Bytestring<kBytestringMaxLen>;

struct NonnegativeInt64SetAddArgs {
  hyper set_value; // int64_t
  hyper add_value; // int64_t
};

union StorageDelta switch (StorageDeltaType type)
{
case kDeleteLast:
  void;
case kBytestringSet:
  Bytestring bstring;
case kNonnegativeInt64SetAdd:
  NonnegativeInt64SetAddArgs nnint64;
};

typedef StorageDelta StorageDeltaVec<>;

// empty struct is legal XDR, as per RFC4506
struct VoidCheck {
  void;
};

// Technically allowed by RFC4506, but unclear why this matters
struct VoidCheckStrange {
  void;
  unsigned int x;
  void;
  unsigned int y;
  void;
};

typedef opaque Data<>;
typedef opaque GenericArraySize<15>;

program blank_rpc {
  version blank_rpc_v1 {
    void null(void) = 0;
    Data echo(Data) = 1;
  } = 1;
} = 3123124;

struct MultilineComment {
  hyper value;
};


/************ Stellar's XDR parse problem ************/

struct StellarValue {
  hyper value;
};

enum StellarValueEnum {
  kStellarValue = 0,
  kStellarValueVec = 1
};

/************ End Stellar's XDR parse problem ************/

/* Multiline comment 2
 end multiline comment 2 */

struct MultilineCommentPost {
  unsigned hyper value;
};

union ExtensionPoint switch (int v)
{
case 0:
  void;
};

struct ParseFailure {
  ExtensionPoint ext;
  int value;
};


