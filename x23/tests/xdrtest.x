
enum senum { SE_NEGATIVE = -1, SE_POSITIVE = 1 };

union sunion switch (senum d)
{
case SE_NEGATIVE:
  bool neg;
case SE_POSITIVE:
  int pos;
default:
  void;
};

union sunion_no_void switch (senum d)
{
case SE_NEGATIVE:
  bool neg;
case SE_POSITIVE:
  int pos;
};

union uunion switch (unsigned int d)
{
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

// Mutually recursive structs (inline struct in union with forward ref)
%struct MutualFrame;

union MutualBody switch (int which)
{
case 0:
  struct {
    int x;
    MutualFrame *child;
  } success;
default:
  void;
};

struct MutualFrame {
  MutualBody body;
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

union u_4_12 switch (int which)
{
case 4:
  fix_4 f4;
case 12:
  fix_12 f12;
};

typedef fix_12 v12<>;

union uptr switch (bool b)
{
case FALSE:
  void;
case TRUE:
  int *val;
};

enum color { RED = 1, REDDER = 2, REDDEST = 3 };

union coloru switch (color c)
{
case RED:
  string r<>;
case REDDER:
  hyper rr;
case REDDEST:
  double rrr;
};

typedef int intvec<>;

program myprog {
  version myvers
  {
    void null(void) = 0;
    int takes_int(int) = 1;
    int takes_two_ints(int, int) = 2;
    coloru takes_recursive(test_recursive, fix_12) = 5;
    intvec takes_vec(intvec) = 6;
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

program blank_rpc
{
  version blank_rpc_v1
  {
    void null(void) = 0;
    Data echo(Data) = 1;
  }
  = 1;
}
= 3123124;

struct MultilineComment {
  hyper value;
};

/************ Stellar's XDR parse problem ************/

struct StellarValue {
  hyper value;
};

enum StellarValueEnum { kStellarValue = 0, kStellarValueVec = 1 };

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

typedef opaque ContractAddress[32];
typedef opaque SHA256Hash[32];

struct TxEmittedEvent {
  ContractAddress contract_address;
  opaque serialized_event<>; // serialized EventData
};

struct TxEventLog {
  SHA256Hash proposed_tx_hash; // sha256(ProposedTransaction)
  TxEmittedEvent events<>;
};

typedef int NotEnum;

union NonEnumTag switch (NotEnum d)
{
case 0:
  void;
};

typedef int *int_opt;
typedef int_opt int_opt_vec<>;
typedef int_opt *int_opt_opt;

// --- Additional fuzz testing types ---

// Float primitive coverage
struct float_struct {
  float f;
};

// Bounded typed vector (tests max-size validation on non-opaque vectors)
typedef int bounded_intvec<8>;

// Fixed-size array of structs (kArray with struct elements)
struct array_of_structs {
  fix_4 items[4];
};

// Fixed-size array of primitives
struct array_of_prims {
  int nums[3];
  double vals[2];
};

// Vector of unions
typedef coloru coloru_vec<>;
typedef StorageDelta storage_delta_vec2<>;

// Struct containing a union field directly
struct struct_with_union {
  int id;
  coloru cu;
  string label<>;
};

// Struct with multiple variable-length fields
struct multi_vec_struct {
  string name<>;
  opaque payload<>;
  int values<>;
  fix_12 items<>;
};

// Optional (pointer to) struct
typedef fix_12 *opt_struct;

// Optional (pointer to) union
typedef coloru *opt_union;

// Optional (pointer to) string
typedef string opt_string<>;
typedef opt_string *opt_string_ptr;

// Vector of vectors (nested dynamic types)
typedef intvec vec_of_vecs<>;

// Vector of optionals of structs
typedef fix_4 *opt_fix4;
typedef opt_fix4 opt_fix4_vec<>;

// Enum with gaps in values (non-contiguous)
enum gappy_enum { GAPPY_A = 0, GAPPY_B = 5, GAPPY_C = 100, GAPPY_D = 255 };

// Union with many arms (exercises larger discriminant switching)
union many_arm_union switch (int tag)
{
case 0:
  void;
case 1:
  bool b;
case 2:
  int i;
case 3:
  unsigned int u;
case 4:
  hyper h;
case 5:
  unsigned hyper uh;
case 6:
  double d;
case 7:
  string s<>;
case 8:
  fix_12 f12;
case 9:
  opaque data<>;
};

// Union where all arms are void
union all_void_union switch (int tag)
{
case 0:
  void;
case 1:
  void;
case 2:
  void;
};

// Deeply nested struct (3 levels of nesting)
struct inner_struct {
  int x;
  double y;
};

struct middle_struct {
  inner_struct inner;
  string tag<>;
};

struct outer_struct {
  middle_struct mid;
  int count;
  middle_struct extras<>;
};

// Union with struct arms that contain vectors (complex nesting)
union complex_arm_union switch (bool which)
{
case FALSE:
  multi_vec_struct mvs;
case TRUE:
  TxEventLog log;
};

// Enum used as a standalone typedef (tests enum deserialization directly)
typedef gappy_enum gappy_enum_vec<>;

// Non-aligned opaque arrays (test padding validation for various sizes)
typedef opaque opaque_5[5];
typedef opaque opaque_7[7];
typedef opaque opaque_1[1];
typedef opaque opaque_33[33];

// Bounded string (tests max-size rejection for string type, not just opaque)
typedef string bounded_string<16>;

// Union with default arm carrying data (not void)
// RFC4506: "default: default-declaration" can have any type.
union default_data_union switch (int tag)
{
case 0:
  int zero_val;
case 1:
  double one_val;
default:
  string fallback<>;
};

// Union with bool discriminant where both arms have data
union bool_both_arms switch (bool b)
{
case FALSE:
  int false_val;
case TRUE:
  double true_val;
};

// Struct mixing fixed-opaque of various alignments
// (exercises first_unsafe_field optimization with mixed field sizes)
struct mixed_opaque_struct {
  opaque a[3];
  int x;
  opaque b[7];
  double y;
  opaque c[1];
};

// Single-element bounded vector (boundary: max_size == 1)
typedef fix_12 single_vec<1>;

// Very small bounded opaque (length prefix + 1 byte max + padding)
typedef opaque tiny_opaque<1>;

// Regression: xdr_size_bounds<const S> must work when S contains a union field
// directly (const propagates through tuple_element_t, and union_ops must handle it).
enum ConstUnionBugType { kConstBugA = 0 };
union ConstUnionBugUnion switch (ConstUnionBugType t) { case kConstBugA: int val; };
struct ConstUnionBugStruct { ConstUnionBugUnion u; };

struct box_tree {
  int value;
  box_tree left;  // #[cpp(box)]
  box_tree right; // #[cpp(box)]
};
