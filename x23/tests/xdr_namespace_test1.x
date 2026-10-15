
namespace xdr_namespace_test_1 {

struct ns_1_struct {
	unsigned int b;
};

// A boxed union arm whose type is declared in this namespace: Box's get<I>
// must be found for it when (de)serializing.
struct ns_1_boxed_inner {
	unsigned int a;
	opaque b<>;
};

enum ns_1_boxed_type { kNs1None = 0, kNs1Inner = 1 };

union ns_1_boxed switch (ns_1_boxed_type t) {
case kNs1None:
	void;
case kNs1Inner:
	ns_1_boxed_inner inner; // #[cpp(box)]
};

}
