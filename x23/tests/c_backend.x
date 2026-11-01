// Types for c_backend.cc, which checks x23c --c output against the C++
// codec.  Covers every construct the C backend handles.

namespace ctest {

const MAXV = 8;

enum Color { RED = 0, GREEN = 1, BLUE = -5 };

// Reuses Color's names, which C must keep apart.
enum Shade { RED = 7, DARK = 8 };

typedef opaque Hash[32];
typedef opaque Odd[5];
typedef string Label<12>;
typedef int Ints<MAXV>;
typedef Color Hue;

struct Point {
  int x;
  unsigned int y;
};

// Sizes that are products: a fixed array of a fixed-size struct, and one of
// a variable-size type, whose size macro is a literal 0.
struct Grid {
  Point cells[2];
  int tag;
};

struct Names {
  Label names[2];
  int tag;
};

struct Prims {
  int i;
  unsigned int u;
  hyper h;
  unsigned hyper uh;
  bool b;
  float f;
  double d;
  Color c;
  Hue hue;
  Shade shade;
};

// Recursive through an optional.
struct Node {
  int value;
  Node *next;
};

union Shape switch (Color kind) {
case RED:
  Point center;
case GREEN:
  Point corners<4>;
case BLUE:
  void;
};

union Num switch (int k) {
case 1:
  hyper big;
case 2:
case 3:
  unsigned int small;
default:
  void;
};

union Flag switch (bool on) {
case TRUE:
  Label name;
case FALSE:
  void;
};

// Recursive by value through union arms, as contract_spec.x's ContractType
// is.  Both C++ and C box the annotated fields.
enum Kind { K_VOID = 0, K_OPT = 1, K_ARRAY = 2, K_TUPLE = 3 };

% struct TypeT;

struct ArrayT {
  TypeT element; // #[cpp(box)]
  unsigned int len;
};

union TypeT switch (Kind k) {
case K_VOID:
  void;
case K_OPT:
  TypeT inner; // #[cpp(box)]
case K_ARRAY:
  ArrayT array;
case K_TUPLE:
  TypeT tuple<3>;
};

struct Everything {
  Prims prims;
  Hash hash;
  Odd odd;
  opaque blob<MAXV>;
  opaque any<>;
  string name<10>;
  Label label;
  Point fixed[3];
  int ints[2];
  Ints vints;
  Point points<MAXV>;
  Shape shapes<>;
  Label labels<3>;
  Num num;
  Flag flag;
  Node *list;
  Point *maybe;
  TypeT type;
  struct {
    int a;
    union switch (bool z) {
    case TRUE:
      int t;
    case FALSE:
      void;
    } inner;
  } anon;
};

} // namespace ctest
