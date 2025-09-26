// -*- C++ -*-

#pragma once

#include <x23/cleanup.h>
#include <x23/x23.h>

namespace x23 {

// Converts any XDR type to a human-readable string or string-like
// type.  The resulting string will be multiple lines of text for
// structures, unions, arrays, and vectors, one line for each element
// of the container.
//
// You can customize how a particular XDR type X is printed by making
// std::formattable<X, char> true.  A minimal example of how to do
// that is as follows (but only from the root namespace, so be sure to
// close any surrounding namespaces):
//
// template<> struct std::formatter<X> {
//   constexpr auto parse(auto &ctx) { return ctx.begin(); }
//   auto format(const X &x, auto &ctx) const {
//     std::format_to(ctx.out(), "X");
//     return ctx.out();
//   }
// };
// static_assert(std::formattable<X, char>);
//
// Just a reminder of some C++ requirements that generate non-obvious
// outcomes if you fail to meet them:
//
//    - parse must be constexpr
//    - format must be const
//    - the arguments should be auto and not std::format_context<X, char>
//      (even if the latter works with some compilers, it won't with others)
//
// If you want to format to something other than std::string, just
// pass an empty one to the second argument, as in:
//
//     Str text = show(x, Str{});
template<is_xdr_type X0, is_bytevec Out = std::string>
Out
show(const X0 &x, size_t indent = 0, Out ret = {})
{
  auto go = []<is_xdr_type X>(this auto &&go, Out *out, const X &x,
                              size_t indent) -> void {
    auto opengroup = [&indent, out](const char *delims) {
      *out += delims[0];
      ++indent;
      return Defer{[id = indent - 1, out, delim = delims[1], sz = out->size()] {
        if (out->size() != sz)
          uformat_to(out, "\n{:{}}", "", 2 * id);
        *out += delim;
      }};
    };

    using enum XdrCategory;
    if constexpr (std::same_as<X, bool>)
      *out += x ? "TRUE" : "FALSE";
    else if constexpr (category<X> == kString) {
      // GNU libstdc++ doesn't support '?' escaped string format specifier
      *out += '"';
      for (auto c : x) {
        if (c < 0x20 || c >= 0x7f)
          uformat_to(out, "\\x{:02x}", std::int8_t(c));
        else {
          if (c == '"' || c == '\\')
            *out += '\\';
          *out += c;
        }
      }
      *out += '"';
    }
    else if constexpr (category<X> & FlagSet{kOpaqueVec, kOpaqueArray}) {
      constexpr char hexbytes[] = "0123456789abcdef";
      out->reserve(out->size() + 2 * x.size());
      for (auto c : x) {
        out->push_back(hexbytes[c >> 4 & 0xf]);
        out->push_back(hexbytes[c & 0xf]);
      }
    }
    else if constexpr (category<X> == kPtr) {
      if (x)
        go(out, *x, indent);
      else
        uformat_to(out, "NULL");
    }
    else if constexpr (is_template_instance<X, std::tuple>) {
      *out += '(';
      if constexpr (std::tuple_size_v<X>) {
        [&]<size_t... I>(std::index_sequence<I...>) {
          auto doarg = [&]<size_t N> {
            go(out, get<N>(x), indent + 1);
            if constexpr (N + 1 < sizeof...(I))
              *out += ',';
          };
          ((doarg.template operator()<I>()), ...);
        }(std::make_index_sequence<std::tuple_size_v<X>>{});
      }
      *out += ')';
    }
    else if constexpr (std::formattable<X, char>)
      uformat_to(out, "{}", x);
    else if constexpr (category<X> == kStruct) {
      auto _g = opengroup("{}");
      [&]<size_t... I>(std::index_sequence<I...>) {
        ((uformat_to(out, "\n{:{}}{} = ", "", 2 * indent,
                     x23::xdr_meta<X>::field_names[I]),
          go(out, get<I>(x), indent), *out += ','),
         ...);
      }(std::make_index_sequence<std::tuple_size_v<X>>{});
    }
    else if constexpr (category<X> & FlagSet{kArray, kVec}) {
      auto _g = opengroup("[]");
      for (const auto &e : x) {
        uformat_to(out, "\n{:{}}", "", 2 * indent);
        go(out, e, indent);
        *out += ',';
      }
    }
    else if constexpr (category<X> == kUnion) {
      using ops = union_ops<X>;
      auto _g = opengroup("{}");
      uformat_to(out, "\n{:{}}{} = {},", "", 2 * indent,
                 ops::meta::field_names[0], get<0>(x));
      if (int n = ops::index(x); n > 0) {
        uformat_to(out, "\n{:{}}{} = ", "", 2 * indent,
                   ops::meta::field_names[n]);
        union_visit_arm([&go, out,
                         indent](const auto &a) { go(out, a, indent); },
                        x);
        *out += ',';
      }
    }
    else
      throw XdrException("Cannot format xdr_type of unknown catetory");
  };

  go(&ret, x, indent);
  return ret;
}

} // namespace x23
