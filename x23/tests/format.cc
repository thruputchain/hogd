
#include <iostream>
#include <print>

#include <x23/format.h>

#include "tests/xdrtest.hh"

using namespace x23;

template<> struct std::formatter<fix_4> {
  constexpr auto parse(auto &ctx) { return ctx.begin(); }
  auto format(const fix_4 &f4, auto &ctx) const {
    std::format_to(ctx.out(), "fix_4(i={})", f4.i);
    return ctx.out();
  }
};
static_assert(std::formattable<fix_4, char>);

int
main()
{
  using enum color;
  fix_4 f4{.i = 999};
  std::cout << show(f4) << std::endl;

  static_assert(is_xdr_enum<color>);
  [[maybe_unused]] std::formatter<color> xxx{};
  std::println("{}", REDDER);

  using ops = x23::union_ops<coloru>;
  static_assert(std::same_as<ops::template union_element_t<0>, color>);
  static_assert(std::same_as<ops::template union_element_t<3>, double>);

  coloru c(REDDEST);
  c.rrr() = 3.141592654;
  std::cout << show(c) << std::endl;

  return 0;
}
