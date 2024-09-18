
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
