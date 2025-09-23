// -*-C++-*-

#pragma once

#include <array>
#include <compare>
#include <concepts>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include <x23/utility.h>
#include <x23/vec.h>

namespace x23 {

struct XdrBadUnionAccess : XdrException {
  using XdrException::XdrException;
};

enum class XdrCategory {
  kInvalid = 0,
  kPrimitive = 1 << 1,
  kEnum = 1 << 2,
  kString = 1 << 3,
  kOpaqueVec = 1 << 4,
  kOpaqueArray = 1 << 5,
  kPtr = 1 << 6,
  kVec = 1 << 7,
  kArray = 1 << 8,
  kStruct = 1 << 9, // structs, but also tuples
  kUnion = 1 << 10,
};

template<typename T> struct xdr_meta;

namespace detail {
template<char... C> constexpr char index_string_val[] = {'<', C..., '>', '\0'};

// crude way to turn a number into a const char * at compile time
template<size_t I, char... C>
consteval const char *
index_string()
{
  if constexpr (I <= 9)
    return index_string_val<I + '0', C...>;
  else
    return index_string<I / 10, I % 10 + '0', C...>();
}

// An array of const char * with {"0", "1", ... "N"} to use as field
// names in tuples.
template<size_t N>
constexpr auto num_string_array = []<size_t... I>(std::index_sequence<I...>) {
  return std::array<const char *, N>{index_string<I>()...};
}(std::make_index_sequence<N>{});
} // namespace detail

// Treat tuples like structures to make it easy to bundle multiple
// arguments into a tuple.  Also, makes std::tuple<> serialize to 0
// bytes.
template<typename... T> struct xdr_meta<std::tuple<T...>> {
  static constexpr XdrCategory category = XdrCategory::kStruct;
  static constexpr const char *type_name = "tuple";
  static constexpr auto &field_names = detail::num_string_array<sizeof...(T)>;
};

template<typename T, size_t N> requires (!std::same_as<T, std::uint8_t>)
struct xdr_meta<std::array<T, N>> {
  static constexpr XdrCategory category = XdrCategory::kArray;
};

template<typename T, size_t N> requires (!std::same_as<T, std::uint8_t>)
struct xdr_meta<Vec<T, N>> {
  static constexpr XdrCategory category = XdrCategory::kVec;
};

template<size_t N> struct xdr_meta<BoundedStr<N>> {
  static constexpr XdrCategory category = XdrCategory::kString;
};

template<size_t N, typename A> struct xdr_meta<Vec<std::uint8_t, N, A>> {
  static constexpr XdrCategory category = XdrCategory::kOpaqueVec;
};

template<size_t N> struct xdr_meta<std::array<std::uint8_t, N>> {
  static constexpr XdrCategory category = XdrCategory::kOpaqueArray;
};

template<typename T>
constexpr XdrCategory category = [] {
  using reffed_type = std::remove_cvref_t<T>;
  using std::same_as;
  using enum XdrCategory;
  if constexpr (requires {
                  { xdr_meta<reffed_type>::category } -> decays_to<XdrCategory>;
                })
    return xdr_meta<reffed_type>::category;
  else if constexpr (same_as<reffed_type, bool> ||
                     same_as<reffed_type, std::int32_t> ||
                     same_as<reffed_type, std::uint32_t> ||
                     same_as<reffed_type, std::int64_t> ||
                     same_as<reffed_type, std::uint64_t> ||
                     same_as<reffed_type, float> ||
                     same_as<reffed_type, double>)
    return kPrimitive;
  else
    return kInvalid;
}();

template<typename T> concept is_xdr_type = category<T> != XdrCategory::kInvalid;

// Matches XDR types that match one of the categories
template<typename T, XdrCategory... C>
concept is_xdr_cat = is_xdr_type<T> && ((category<T> == C) || ...);
// redundant is_xdr_type<T> makes it more specific than is_xdr_type

template<typename T> concept is_xdr_enum = is_xdr_cat<T, XdrCategory::kEnum>;

template<typename T>
concept is_xdr_struct = is_xdr_cat<T, XdrCategory::kStruct>;

template<typename T> concept is_xdr_union = is_xdr_cat<T, XdrCategory::kUnion>;

template<typename... T>
using common_twc_result =
    std::common_comparison_category_t<std::strong_ordering,
                                      std::compare_three_way_result_t<T>...>;

// A Ptr is kind of like std::optional, except it can be recursive,
// with a struct containing a Ptr to itself.  Put another way, a Ptr
// is just a std::unique_ptr with a copy constructor that copies the
// underlying object.
//
// Note that the constness handling isn't airtight, because it's based on
// std::unique_ptr yet should behave like std::optional.
template<typename T> struct Ptr : std::unique_ptr<T> {
  using super = std::unique_ptr<T>;
  using typename super::element_type;
  using value_type = T;
  using difference_type = std::ptrdiff_t;
  using iterator = value_type *;
  using const_iterator = const value_type *;

  using super::super;
  Ptr(const Ptr &other)
  {
    if (other)
      this->reset(new element_type(*other));
  }
  Ptr(Ptr &&other) noexcept = default;

  Ptr &operator=(const Ptr &other)
  {
    if (other)
      this->reset(new element_type(*other));
    else
      this->reset();
    return *this;
  }
  Ptr &operator=(Ptr &&other) noexcept = default;

  element_type *get() noexcept { return super::get(); }
  const element_type *get() const noexcept { return super::get(); }
  decltype(auto) operator*(this auto &&self) { return *self.get(); }

  template<typename... Args>
  requires std::constructible_from<element_type, Args...>
  element_type &emplace(Args &&...args)
  {
    this->reset(new element_type(std::forward<Args>(args)...));
    return *this->get();
  }

  // Make it look a litle Vec-like to simplify marshaling code
  static constexpr size_t max_size() { return 1; }
  size_t size() const noexcept { return bool(*this); }
  void resize(size_t n)
  {
    switch (n) {
    case 0:
      this->reset();
      break;
    case 1:
      emplace();
      break;
    default:
      [[unlikely]] throw XdrLengthError("Attempt to resize Ptr to more than 1");
    }
  }
  static void reserve(size_t) {}
  auto begin(this auto &&self) noexcept { return self.get(); }
  auto end(this auto &&self) noexcept { return self.begin() + self.size(); }
  value_type &back(this auto &&self) { return *self.get(); }
};

template<typename T>
inline bool
operator==(const Ptr<T> &a, const Ptr<T> &b)
{
  if (a && b)
    return *a == *b;
  return bool(a) == bool(b);
}

template<typename T>
inline std::compare_three_way_result_t<T>
operator<=>(const Ptr<T> &a, const Ptr<T> &b)
{
  if (a && b)
    return a.get() <=> b.get();
  return bool(a) <=> bool(b);
}

template<typename T> struct xdr_meta<Ptr<T>> {
  static constexpr XdrCategory category = XdrCategory::kPtr;
};

} // namespace x23

template<x23::is_xdr_enum E>
struct std::formatter<E> : std::formatter<std::string> {
  using super = std::formatter<std::string>;
  static inline const std::unordered_map<E, std::string> valmap{
      x23::xdr_meta<E>::tags.begin(), x23::xdr_meta<E>::tags.end()};
  auto format(E e, auto &ctx) const
  {
    if (auto it = valmap.find(e); it != valmap.end())
      return super::format(it->second, ctx);
    return super::format(std::format("{}#{}", x23::xdr_meta<E>::type_name,
                                     std::underlying_type_t<E>(e)),
                         ctx);
  }
};

namespace x23 {

// Rather than generate many copies of the same boiler-plate code for
// each union type, we implement the guts of the operations in the
// friend class union_ops.
template<is_xdr_union T> struct union_ops {
  using union_type = T;
  using meta = xdr_meta<union_type>;

  // union_size is one more than the number of non-void arms.
  // Depending on context, arm number 0 is reserved for void arms or
  // for access to the discriminant.
  static constexpr size_t union_size = meta::field_names.size();

protected:
  template<size_t I> requires (I < union_size)
  static constexpr auto fieldptr()
  {
    return union_type::template __xdr_union_fieldptr<I>();
  }

public:
  using discriminant_type = meta::discriminant_type;

  // Which arm is active (0 for void, -1 for invalid discriminant)
  static int index(discriminant_type d)
  {
    return union_type::__xdr_union_arm_number(d);
  }
  static int index(const union_type &u) { return index(get_discriminant(u)); }

  static discriminant_type get_discriminant(const union_type &u)
  {
    return u.*fieldptr<0>();
  }

  static void set_discriminant(union_type &u, discriminant_type d)
  {
    // Deliberately not optimizing for the case where the active arm
    // doesn't change, because when it occurs by accident that could
    // lead to nondeterminism bugs.
    destroy(u);
    construct(u, d);
  }

  // Get a particular field by number.  Uses a template argument U to
  // preserive const and lvalue/rvalue reference status of u.
  template<size_t I, references<union_type> U> requires (I < union_size)
  static decltype(auto) get(U &&u)
  {
    if (int n = index(u); n < 0)
      throw XdrBadUnionAccess(
          "attempt to access field {} in {} with invalid discriminant {}",
          I > 0 ? meta::field_names[I] : "void", meta::type_name,
          get_discriminant(u));
    else if (n != I)
      throw XdrBadUnionAccess(
          "attempt to access field {} in {} when {} is selected",
          I > 0 ? meta::field_names[I] : "void", meta::type_name,
          n > 0 ? meta::field_names[n] : "void");
    return unsafe_get<I>(std::forward<U>(u));
  }

  // Type of union arm I.  With I == 0, returns the discriminant type.
  template<size_t I> requires (I < union_size)
  using union_element_t = std::conditional_t<
      I == 0, discriminant_type,
      std::remove_reference_t<decltype(get<I>(std::declval<union_type>()))>>;

  static bool eq(const union_type &a, const union_type &b)
  {
    discriminant_type d = get_discriminant(a);
    if (d != get_discriminant(b))
      return false;
    if (int n = index(d); n <= 0)
      return true;
    else
      return with_n<union_size, 1, bool>(n, [&a, &b](auto i) {
        return a.*fieldptr<i>() == b.*fieldptr<i>();
      });
  }

  static auto cmp(const union_type &a, const union_type &b)
  {
    using return_type = decltype([]<size_t... I>(std::index_sequence<I...>) {
      return common_twc_result<union_element_t<I>...>{0 <=> 0};
    }(std::make_index_sequence<union_size>{}));
    discriminant_type d = get_discriminant(a);
    return_type r = d <=> get_discriminant(b);
    if (r == 0)
      if (int n = index(d); n > 0)
        r = with_n<union_size, 1, return_type>(n, [&a, &b](auto i) {
          return unsafe_get<i>(a) <=> unsafe_get<i>(b);
        });
    return r;
  }

private:
  template<size_t I, references<union_type> U> requires (I < union_size)
  static decltype(auto) unsafe_get(U &&u)
  {
    if constexpr (I != 0)
      return std::forward_like<U>(u.*fieldptr<I>());
  }

  static void destroy(union_type &u)
  {
    if (int n = index(u); n > 0)
      with_n<union_size, 1>(n, [&u](auto n) {
        if constexpr (n > 0)
          std::destroy_at(&unsafe_get<n>(u));
      });
  }

  template<typename... Args>
  static void construct(union_type &u, discriminant_type d, Args &&...args)
      // We depend on value initialization (no args) being noexcept to
      // recover from exceptions in copy assignment.
      noexcept(sizeof...(Args) == 0)
  {
    if (int n = index(d); n <= 0) {
      if constexpr (sizeof...(Args) > 0)
        throw XdrBadUnionAccess(
            "constructor arguments supplied for {} discriminant {} in {}",
            n == 0 ? "void" : "invalid", d, meta::type_name);
    }
    else
      with_n<union_size, 1>(n, [&u, &args...](auto n) {
        if constexpr (std::constructible_from<union_element_t<n>, Args...>)
          do_construct<n>(u, std::forward<Args>(args)...);
        else
          throw XdrBadUnionAccess(
              "{} invalid constructor arguments for field {} in {}",
              sizeof...(Args), meta::field_names[n], meta::type_name);
      });
    u.*fieldptr<0>() = d;
  }

  template<int I, typename... Args>
  requires (I < union_size) &&
           std::constructible_from<union_element_t<I>, Args...>
  static void do_construct(union_type &u, Args &&...args)
      noexcept(sizeof...(Args) == 0 ||
               noexcept(union_element_t<I>(std::forward<Args>(args)...)))
  {
    if constexpr (I > 0)
      std::construct_at(&unsafe_get<I>(u), std::forward<Args>(args)...);
  }

  template<references<union_type> U>
  static void construct(union_type &u, U &&other)
  {
    discriminant_type d = get_discriminant(other);
    if (int n = index(d); n > 0)
      with_n<union_size, 1, void>(n, [&u, &other](auto n) {
        std::construct_at(&unsafe_get<n>(u),
                          std::forward_like<U>(unsafe_get<n>(other)));
      });
    u.*fieldptr<0>() = d;
  }

  template<references<union_type> U>
  static void assign(union_type &u, U &&other)
  {
    discriminant_type d = get_discriminant(other);
    int n = index(d);
    if (n != index(u)) {
      destroy(u);
      try {
        construct(u, std::forward<U>(other));
      } catch (...) {
        // We have to leave the object in a state where it can safely
        // be destroyed, which won't be the case after destroy(u).
        // Use a large or highly negative discriminant value that's
        // more likely to be invalid for unions without default arms.
        static_assert(noexcept(construct(u, discriminant_type(0x80000000))));
        construct(u, discriminant_type(0x80000000));
        throw;
      }
      return;
    }
    u.*fieldptr<0>() = d;
    if (n > 0)
      with_n<union_size, 1>(n, [&u, &other](auto n) {
        unsafe_get<n>(u) = std::forward_like<U>(unsafe_get<n>(other));
      });
  }

  friend T;
};

// Set the discriminant of a union.  Note:  you can always get the
// value of a discriminant of union u with get<0>(u).  But since
// setting the union requires additional logic for changing the active
// arm, we need a different function.
template<is_xdr_union U>
inline void
union_set_discriminant(U &u, typename xdr_meta<U>::discriminant_type d)
{
  union_ops<U>::set_discriminant(u, d);
}

template<is_xdr_union U>
inline bool
union_valid(const U &u)
{
  return union_ops<U>::index(u) != -1;
}

// Apply a function object f to the active arm of a union.  If no arm
// is active then does nothing.
template<typename F, is_xdr_union U>
inline void
union_visit_arm(F &&f, U &&u)
{
  using ops = union_ops<std::remove_cvref_t<U>>;
  if (int n = ops::index(u); n > 0)
    with_n<ops::union_size, 1>(n, [&](auto i) {
      f(std::forward_like<U>(get<i>(u)));
    });
}

// Apply a function object f to the active arm of a union and return
// the result.  If no arm is active then return void_result;
template<typename R, typename F, is_xdr_union U>
inline R
union_visit_arm(F &&f, U &&u, R void_result)
{
  using ops = union_ops<std::remove_cvref_t<U>>;
  if (int n = ops::index(u); n > 0)
    return with_n<ops::union_size, 1, R>(n, [&](auto i) -> R {
      return f(std::forward_like<U>(get<i>(u)));
    });
  return void_result;
}

// Apply a function object to every field of a struct or tuple.
template<typename T> requires requires {
  { std::tuple_size_v<std::remove_cvref_t<T>> } -> decays_to<size_t>;
}
inline void
visit_fields(auto &&f, T &&t)
{
  [&]<size_t... I>(std::index_sequence<I...>) {
    (f(get<I>(t)), ...);
  }(std::make_index_sequence<std::tuple_size_v<std::remove_cvref_t<T>>>{});
}

template<is_xdr_type X>
constexpr std::string
xdr_type_name()
{
  using std::string_literals::operator""s;
  using std::same_as;
  using std::uint8_t;
  if constexpr (requires { std::string(xdr_meta<X>::type_name); })
    return std::string(xdr_meta<X>::type_name);
#define DO_SCALAR(T, NAME) else if constexpr (std::same_as<X, T>) return NAME
  DO_SCALAR(bool, "bool");
  DO_SCALAR(std::int32_t, "int");
  DO_SCALAR(std::uint32_t, "unsigned int");
  DO_SCALAR(std::int64_t, "hyper");
  DO_SCALAR(std::uint64_t, "unsigned hyper");
  DO_SCALAR(float, "float");
  DO_SCALAR(double, "double");
#undef DO_SCALAR
  else return Multilambda{
      []<typename T>(Ptr<T> *) { return xdr_type_name<T>() + '*'; },
      []<typename T, size_t MS, typename A>(Vec<T, MS, A> *) {
        std::string bound = MS >= 0xffffffff ? ""s : std::format("{}", MS);
        if constexpr (same_as<T, std::uint8_t>)
          return std::format("opaque<{}>", bound);
        else
          return std::format("{}<{}>", xdr_type_name<T>(), bound);
      },
      []<typename T, size_t N>(std::array<T, N> *) {
        if constexpr (same_as<T, std::uint8_t>)
          return std::format("opaque[{}]", N);
        else
          return std::format("{}[{}]", xdr_type_name<T>(), N);
      },
      []<size_t MS>(BoundedStr<MS> *) {
        std::string bound = MS >= 0xffffffff ? ""s : std::format("{}", MS);
        return std::format("string<{}>", bound);
      },
  }(static_cast<X *>(nullptr));
}

template<is_xdr_struct T>
inline auto
xdr_cmp(const T &a, const T &b)
{
  return [&a, &b]<size_t... I>(std::index_sequence<I...>) {
    common_twc_result<std::tuple_element_t<I, T>...> r{0 <=> 0};
    (void)((r = get<I>(a) <=> get<I>(b), r == 0) && ...);
    return r;
  }(std::make_index_sequence<std::tuple_size_v<T>>{});
}

// This function is used to suppresses a compiler warning about a
// switch statement on a bool in generated code.  You shouldn't need
// to use it in hand-written code.
inline std::uint32_t
unbool(bool b)
{
  return b;
}
template<typename T>
requires (!std::same_as<T, bool> && (std::integral<T> || std::is_enum_v<T>))
inline T
unbool(T b)
{
  return b;
}

namespace detail {
enum EmptyEnum {};
} // namespace detail

// Used to check version concepts
struct VacuousSendCall {
  template<typename ProcMeta>
  typename ProcMeta::return_type send_call(auto &&...)
  {
    throw;
  }
};

// The code generator doesn't necessarily know what is and is not an
// enum, so regardless of whether or not T is an enum, you can always
// try to import the enum tags to the local namespace with:
//   using enum AlwaysEnum<T>::type;
template<typename T> struct AlwaysEnum {
  using type = std::conditional_t<std::is_enum_v<T>, T, detail::EmptyEnum>;
};

// Specialized to std::true_type by each RPC program/version for types
// Server that implement the RPC interface defined by VersMeta.
// RetTrans is a transformation of the return type (e.g., x23::Async
// for a coroutine-based RPC implementation).  ExtraArgs are extra
// arguments expected to be passed to each function (e.g., which might
// contain session context).
template<typename Server, typename VersMeta,
         template<typename> typename RetTrans = std::type_identity_t,
         typename... ExtraArgs>
struct rpc_compatible : std::false_type {};

// A concept corresponding to boolean in rpc_compatible.  Note that
// each individual program/version already has a concept
// is_program_version.  This concepts just generalizes that to an
// arbitrary VersMeta.
template<typename Server, typename VersMeta,
         template<typename> typename RetTrans = std::type_identity_t,
         typename... ExtraArgs>
concept implements_rpc =
    rpc_compatible<Server, VersMeta, RetTrans, ExtraArgs...>::value;

} // namespace x23
