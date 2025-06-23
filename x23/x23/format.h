// -*- C++ -*-

#pragma once

#include <x23/cleanup.h>
#include <x23/x23.h>

namespace x23 {

template<is_xdr_type X0, is_bytevec Out = std::string>
Out
show(const X0 &x, size_t indent = 0, Out ret = {})
{
  auto go = []<is_xdr_type X>(this auto &&go, is_bytevec auto *out, const X &x,
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
