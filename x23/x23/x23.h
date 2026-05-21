// -*-C++-*-

#pragma once

#include <algorithm>
#include <array>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>

#include "x23/debug.h"
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
    return index_string<I / 10, (I % 10) + '0', C...>();
}

// An array of const char * with {"0", "1", ... "N"} to use as field
// names in tuples.
template<size_t N>
constexpr auto num_string_array = []<size_t... I>(std::index_sequence<I...>) {
  return std::array<const char *, N>{index_string<I>()...};
}(std::make_index_sequence<N>{});
} // namespace detail

template<typename... T> struct xdr_meta<std::tuple<T...>>;

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

template<> struct xdr_meta<BufView> {
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
                     same_as<reffed_type, int32_t> ||
                     same_as<reffed_type, uint32_t> ||
                     same_as<reffed_type, int64_t> ||
                     same_as<reffed_type, uint64_t> ||
                     same_as<reffed_type, float> ||
                     same_as<reffed_type, double>)
    return kPrimitive;
  else
    return kInvalid;
}();

template<typename T> concept is_xdr_type = category<T> != XdrCategory::kInvalid;

// Treat tuples like structures to make it easy to bundle multiple
// arguments into a tuple.  Also, makes std::tuple<> serialize to 0
// bytes.
template<typename... T> struct xdr_meta<std::tuple<T...>> {
  // is_xdr_serializable but not is_xdr_type for serializable non-xdr-types T
  static constexpr XdrCategory category =
      (is_xdr_type<T> && ...) ? XdrCategory::kStruct : XdrCategory::kInvalid;
  static constexpr const char *type_name = "tuple";
  static constexpr auto &field_names = detail::num_string_array<sizeof...(T)>;
};

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

  // Allow construction from nullptr
  using super::super;

  Ptr(const Ptr &other)
  {
    if (other)
      this->reset(new element_type(*other));
  }
  Ptr(Ptr &&other) noexcept = default;

  // Allow construction from rvalues of type T
  Ptr(value_type t) { emplace(std::move(t)); }

  template<typename... Args>
  requires std::constructible_from<value_type, Args...>
  Ptr(std::in_place_t, Args &&...args)
  {
    emplace(std::forward<Args>(args)...);
  }
  template<typename U, typename... Args>
  requires std::constructible_from<value_type, std::initializer_list<U>,
                                   Args...>
  Ptr(std::in_place_t, std::initializer_list<U> ilist, Args &&...args)
  {
    emplace(ilist, std::forward<Args>(args)...);
  }

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
  template<typename U, typename... Args>
  requires std::constructible_from<element_type, std::initializer_list<U>,
                                   Args...>
  element_type &emplace(std::initializer_list<U> ilist, Args &&...args)
  {
    this->reset(new element_type(ilist, std::forward<Args>(args)...));
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
  static void reserve(size_t /*unused*/) {}
  auto begin(this auto &&self) noexcept { return self.get(); }
  auto end(this auto &&self) noexcept { return self.begin() + self.size(); }
  value_type &back(this auto &&self) { return *self.get(); }
};
template<typename T> Ptr(T &&) -> Ptr<T>;

template<typename T> struct Box : private std::unique_ptr<T> {
  using super = std::unique_ptr<T>;
  using element_type = T;
  using value_type = T;

  Box() noexcept = default;

  Box(const Box &other)
  {
    if (other.has_value())
      this->reset(new T(*other));
  }
  Box(Box &&other) noexcept = default;

  Box(value_type t) : super(new T(std::move(t))) {}

  template<typename... Args> requires std::constructible_from<T, Args...>
  Box(std::in_place_t, Args &&...args)
    : super(new T(std::forward<Args>(args)...))
  {}
  template<typename U, typename... Args>
  requires std::constructible_from<T, std::initializer_list<U>, Args...>
  Box(std::in_place_t, std::initializer_list<U> ilist, Args &&...args)
    : super(new T(ilist, std::forward<Args>(args)...))
  {}

  Box &operator=(const Box &other)
  {
    if (has_value() && other.has_value())
      **this = *other;
    else if (other.has_value())
      this->reset(new T(*other));
    else
      this->reset();
    return *this;
  }
  Box &operator=(Box &&other) noexcept = default;

  bool has_value() const noexcept { return bool(super::get()); }
  void ensure()
  {
    if (!has_value())
      this->reset(new T{});
  }

  T *get() noexcept
  {
    x23assert(has_value());
    return super::get();
  }
  const T *get() const noexcept
  {
    x23assert(has_value());
    return super::get();
  }
  decltype(auto) operator*(this auto &&self) { return *self.get(); }
  auto operator->(this auto &&self) noexcept { return self.get(); }
};
template<typename T> Box(T &&) -> Box<T>;

// A non-owning Ptr that serializes like a Ptr.
template<typename T> struct BorrowedPtr {
  BorrowedPtr() noexcept = default;
  BorrowedPtr(T *p) noexcept : p_(p) {}
  BorrowedPtr(auto ptr) requires requires {
    { ptr.get() } -> std::convertible_to<T *>;
  }
    : p_(ptr.get())
  {}

  T *get() const noexcept { return p_; }
  explicit operator bool() const noexcept { return p_; }
  T *operator->() const noexcept { return p_; }
  T &operator*() const noexcept { return *p_; }

  T *p_{};
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
    return *a <=> *b;
  return bool(a) <=> bool(b);
}

template<typename T> struct xdr_meta<Ptr<T>> {
  static constexpr XdrCategory category = XdrCategory::kPtr;
};

template<typename T>
inline bool
operator==(const Box<T> &a, const Box<T> &b)
{
  static const T da{}, db{};
  return (a.has_value() ? *a : da) == (b.has_value() ? *b : db);
}

template<typename T>
inline std::compare_three_way_result_t<T>
operator<=>(const Box<T> &a, const Box<T> &b)
{
  static const T da{}, db{};
  return (a.has_value() ? *a : da) <=> (b.has_value() ? *b : db);
}

template<typename T> struct xdr_meta<Box<T>> : xdr_meta<T> {};

} // namespace x23

template<typename T> requires requires { typename std::tuple_size<T>::type; }
struct std::tuple_size<x23::Box<T>> : std::tuple_size<T> {};

template<size_t I, typename T>
requires requires { typename std::tuple_element<I, T>::type; }
struct std::tuple_element<I, x23::Box<T>> : std::tuple_element<I, T> {};

template<size_t I, typename T> requires requires(T &t) { get<I>(t); }
decltype(auto)
get(x23::Box<T> &b)
{
  b.ensure();
  return get<I>(*b);
}

template<size_t I, typename T> requires requires(const T &t) { get<I>(t); }
decltype(auto)
get(const x23::Box<T> &b)
{
  x23assert(b.has_value());
  return get<I>(*b);
}

template<size_t I, typename T> requires requires(T &t) { get<I>(t); }
decltype(auto)
get(x23::Box<T> &&b)
{
  x23assert(b.has_value());
  return get<I>(*std::move(b));
}

template<x23::is_xdr_enum E>
struct std::formatter<E> : std::formatter<std::string> {
  using super = std::formatter<std::string>;
  static inline const std::unordered_map<E, std::string> valmap_{
      x23::xdr_meta<E>::tags.begin(), x23::xdr_meta<E>::tags.end()};
  auto format(E e, auto &ctx) const
  {
    if (auto it = valmap_.find(e); it != valmap_.end())
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
  using union_type = std::remove_cvref_t<T>;
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

  static bool eq(const union_type &a, const union_type &b) noexcept
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

  static auto cmp(const union_type &a, const union_type &b) noexcept
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
unbool(bool b) noexcept
{
  return std::uint32_t{b};
}
template<typename T>
requires (!std::same_as<T, bool> && (std::integral<T> || std::is_enum_v<T>))
inline T
unbool(T b) noexcept
{
  return b;
}

namespace detail {
enum EmptyEnum {};
} // namespace detail

// Used to check version concepts
struct VacuousSendCall {
  template<typename ProcMeta>
  typename ProcMeta::return_type send_call(auto &&.../*unused*/)
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

// A type allowing version metadata to be associated with an enum
// representing proc numbers in an RPC interface.
template<typename ProcEnum> requires std::is_enum_v<ProcEnum>
struct version_for_proc_enum;

// True if E represents procedures in a RPC program/version interface.
template<typename E>
concept is_proc_enum = requires { typename version_for_proc_enum<E>::type; };

// Specialize this template to declare that some XDR type serializes
// equivalently to some other type.
template<typename XdrType, typename OtherType>
struct serialize_equiv : std::is_same<XdrType, OtherType> {};

template<> struct serialize_equiv<void, std::monostate> : std::true_type {};
template<> struct serialize_equiv<void, std::tuple<>> : std::true_type {};

// Concept for types T you can use where you want to serialize an X.
template<typename T, typename X>
concept serializes_like = serialize_equiv<X, std::remove_cvref_t<T>>::value;

// Vectors, Arrays, and Pointers serialize equivalently if their value
// types do so as well.
template<typename X, size_t NX, serializes_like<X> T, size_t TX>
requires (NX >= std::min(TX, kMaxXdrSize))
struct serialize_equiv<Vec<X, NX>, Vec<T, TX>> : std::true_type {};

template<typename X, size_t N, serializes_like<X> T>
struct serialize_equiv<std::array<X, N>, std::array<T, N>> : std::true_type {};

template<typename X, serializes_like<X> T>
struct serialize_equiv<Ptr<X>, Ptr<T>> : std::true_type {};

// Concept for types you can pass to an RPC function expecting type X.
template<typename T, typename X>
concept satisfies_rpc_arg = serializes_like<T, X> || std::convertible_to<T, X>;

// Convert arguments that need to be converted to RPC type X.  Note
// the result will have reference semantics, so should not be used
// after the arguments are destroyed.  Specifically:
//
//    f(convert_rpc_arg<XdrStr>("hello"_str));  // OK
//
//    decltype(auto) tmp = convert_rpc_arg<XdrStr>("hello"_str);
//    // argument destroyed at end of full expression above
//    f(tmp);   // Use after free error
//
//    auto tmp2 = convert_rpc_arg<XdrStr>("hello"_str);
//    f(tmp2);  // OK but incured extra move into tmp2
template<typename X, satisfies_rpc_arg<X> T>
inline decltype(auto)
convert_rpc_arg(T &&t)
{
  if constexpr (serializes_like<std::remove_cvref_t<T>, X>)
    return static_cast<T &&>(t);
  else
    return X(t);
}

static_assert(std::same_as<decltype(convert_rpc_arg<Str>("hi")), Str>);
static_assert(std::same_as<decltype(convert_rpc_arg<Str>("hi"_str)), Str &&>);

// untransform_t<RetTrans, RetTrans<T>> should yield type T
template<template<typename> typename Trans, typename T> struct untransform;
template<template<typename> typename Trans, typename T>
using untransform_t = untransform<Trans, T>::type;

template<typename T> struct untransform<std::type_identity_t, T> {
  using type = T;
};

template<template<typename> typename Trans, typename T>
struct untransform<Trans, Trans<T>> {
  using type = T;
};

#if 0
// This is the code we want, but it crashes on clang 20
template<typename T>
using untransform_auto_t =
    decltype([]<template<typename> typename Trans, typename Inner>(
                 Trans<Inner> *) -> Inner {
      throw;
    }(static_cast<T *>(nullptr)));
#else
namespace detail {
template<typename T> struct untransform_auto {};
template<template<typename> typename Trans, typename Inner>
struct untransform_auto<Trans<Inner>> {
  using type = Inner;
};
}; // namespace detail

template<typename T>
using untransform_auto_t = detail::untransform_auto<T>::type;
#endif

template<typename T, typename Xdr,
         template<typename> typename RetTrans = std::type_identity_t>
concept satisfies_rpc_return =
    std::convertible_to<T, RetTrans<Xdr>> ||
    serializes_like<untransform_t<RetTrans, T>, Xdr> ||
    (std::convertible_to<T, RetTrans<untransform_auto_t<T>>> &&
     serializes_like<untransform_auto_t<T>, Xdr>) ||
    (std::is_void_v<T> && std::convertible_to<std::monostate, RetTrans<Xdr>>);

} // namespace x23
