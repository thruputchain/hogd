#include <algorithm>
#include <cstddef>
#include <format>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>

#include "x23/cleanup.h"
#include "x23c/defs.h"

using namespace x23;

using std::optional;
using std::string_view;
using std::string_literals::operator""s;

struct GenRs {
  string top_;
  string bottom_;
  std::size_t indent_{};
  vector<string> namespaces_;
  vector<string> scopes_;

  bool needs_serde_imports = false;

  constexpr static int kIndentWidth = 4;

  void process(const vector<Definition> &defs);

  void show_def(const Definition &d) {
    show_comment(d.comment, true);
    prtop("\n");
    visit(
        [this](const auto &d) { show_def(d); },
        d);
  }
  void show_def(const ConstDef &c)
  {
    string val = c.val;
    std::ranges::replace(val, '\'', '_');

    string comment = c.comment;
    string type = "u32";


    if (optional<string> remainder = substring_removed(c.comment, kConstU32)) {
      comment = *remainder;
      type = "u32";
    }
    else if (optional<string> remainder = substring_removed(c.comment, kConstU64)) {
      comment = *remainder;
      type = "u64";
    }

    show_comment(comment, false);
    prtop("pub const {} : {} = {};\n", c.id, type, val);
  }
  void show_def(const Typedef &d)
  {
    prtop("pub type {} = {};\n", d.id, type_name(d, true));
  }
  void show_def(const EnumDecl &t)
  {
    show_typespec(t.body, t.id);
    prtop("\n");
  }
  void show_def(const StructDecl &t)
  {
    show_typespec(t.body, t.id);
    prtop("\n");
  }
  void show_def(const UnionDecl &t)
  {
    show_typespec(t.body, t.id);
    prtop("\n");
  }
  void show_def(const OpenNS &ns)
  {
    // Ignore
  }
  void show_def(const CloseNS &ns)
  {
    // Ignore
  }
  void show_def(const Literal &l)
  { 
    // ignore
  }
  void show_def(const ProgDef &prog);

  void show_decl(const Declaration &d, string const& name_prefix)
  {
    if (d.qual == Declaration::kVoid) {
      return;
    }

    show_comment(d.comment, false);

    if (!d.attrs.empty()) {
      prtop("{}\n", d.attrs);
    }

    if (auto array_size = get_opaque_array_size(d)) {
        // type is a byte array
        if (needs_serde_imports)
            prtop("#[cfg_attr(feature = \"serde\", serde(with = \"serde_arrays\"))]\n");
    } else if (d.qual == Declaration::kVec && get<string>(d.type) == "opaque") {
        // type is a byte vector
    }

    auto type_name_str = (holds_alternative<string>(d.type))
      ? type_name(d, false)
      : (name_prefix + "_" + type_name(d, false));

    string vis = d.is_private ? "" : "pub ";
    if (d.id.empty()) {
      prtop("{}{},\n", vis, type_name_str);
    } else {
      prtop("{}{}: {},\n", vis, d.id, type_name_str);
    }
  }

  void
  do_generations(std::string name_prefix, const TypeSpecifier &type);

  void show_typespec(const TypeSpecifier &ts, string t)
  {
    if (optional<string> remainder = substring_removed(ts.comment, kRustWriteXDR)) {
      // comment = *remainder;
      prtop("#[derive(WriteXDR)]\n");
    }

    visit([this, &t](const auto &v) { show_type(v, t); }, ts);
  }

  void show_type(const string &s, string) { prtop("{}", map_type(s)); }
  void show_type(const EnumBody &e, string);
  void show_type(const StructBody &s, string);
  void show_type(const UnionBody &b, string);

  void show_comment(std::string_view comment, bool chomp, string prefix = {});

  static string map_type(const string &);
  static string map_case(const string &);
  static string map_bound(const string &);
  string type_name(const Declaration &d, bool generate, bool in_colon_required_setting = false);
  // return nullopt unless the type is an opaque array
  static std::optional<string> get_opaque_array_size(const Declaration &d);

  template<typename... Args>
  void prtop(std::format_string<Args...> fmt, Args &&...args)
  {
    if (top_.empty() || top_.back() == '\n')
      top_ += std::string(kIndentWidth * indent_, ' ');
    top_ += std::vformat(fmt.get(), std::make_format_args(args...));
  }

  auto open_scope(string close = {})
  {
    return Defer([this, indent = indent_, nsdepth = namespaces_.size(),
                  sdepth = scopes_.size(), close = std::move(close)] {
      indent_ = indent;
      namespaces_.resize(nsdepth);
      scopes_.resize(sdepth);
      if (!close.empty())
        prtop("{}", close);
    });
  }

  auto open_brace()
  {
    prtop("{{\n");
    auto ret = open_scope("}"s);
    ++indent_;
    return ret;
  }

  auto open_paren()
  {
    prtop("(\n");
    auto ret = open_scope(");"s);
    ++indent_;
    return ret;
  }

  string join(auto &&v, string with)
  {
    string ret;
    Join j(with);
    for (const auto &n : v) {
      ret += j.get();
      ret += n;
    }
    return ret;
  }

  string ns() { return join(namespaces_, "::"); }

  string scope(bool qualified = false)
  {
    string ret = qualified ? ns() + "::"s : ""s;
    ret += join(scopes_, "::");
    return ret;
  }

  optional<string> substring_removed(const string &str, const string_view sub) {
      string::size_type i = str.find(sub);
      if (i != string::npos) {
          string str2 = string{str};
          str2.erase(i, sub.length());
          return str2;
      }
      return {};
  }

  static constexpr string_view kRustNewtype = R"(#[rust(newtype)])";
  static constexpr string_view kRustWriteXDR = R"(#[rust(WriteXDR)])";
  static constexpr string_view kSerdeArrays = R"(#[serde(with = "serde_arrays")])";
  static constexpr string_view kConstU32 = R"(#[rust(const=u32)])";
  static constexpr string_view kConstU64 = R"(#[rust(const=u64)])";

  optional<Definition> convert_to_newtype(const Definition &d);
};

optional<Definition>
GenRs::convert_to_newtype(const Definition &d)
{
  struct NewtypeVisitor {
    string comment;
    string attrs;

    // Convert a typedef to a struct with a single field
    Definition operator() (const Typedef &d)
    {
      Declaration inner {
          .type = d.type,
          .id = "inner",    // or can leave nameless for a tuple-struct
          .qual = d.qual,
          .bound = d.bound,
          .attrs = attrs,
          .is_private = true,
      };

      StructBody t_body { .transparent = true };
      t_body.push_back(inner);

      Definition ds = StructDecl {
        .id = d.id,
        .body = t_body
      };
      ds.comment = comment;

      return ds;
    }

    Definition operator() (const Definition& /*unused*/)
    {
      throw std::runtime_error("newtype conversion is only implemented for XDR typedefs");
    }
  };

  if (optional<string> remainder = substring_removed(d.comment, kRustNewtype)) {
    string comment = *remainder;
    string attrs;

    if (optional<string> remainder = substring_removed(comment, kSerdeArrays)) {
      comment = *remainder;
      if (!attrs.empty()) {
          attrs += "\n";
      }
      attrs += kSerdeArrays;
      needs_serde_imports = true;
    }

    auto visitor = NewtypeVisitor {
      .comment = comment,
      .attrs = attrs
    };
    return visit(visitor, d);

  }
  return {};
}

void
GenRs::process(const vector<Definition> &defs)
{
  for (const auto &d : defs) {
    if (optional<Definition> dn = convert_to_newtype(d)) {
      show_def(*dn);
    } else {
      show_def(d);
    }
  }
}

void
GenRs::show_def(const ProgDef &prog)
{
  // unimplemented
}

void
GenRs::show_type(const StructBody &s, string t)
{
  for (const auto &d : s) {
    do_generations(t + "_" + d.id, d.type);
  }

  {
    optional<string> array_size;

    string derive_traits = "PartialEq, Debug, Clone";
    if (s.transparent) {
        auto inner = s.front();
        array_size = get_opaque_array_size(inner);

        if (!(array_size && *array_size == "64")) {
            // arrays with size over 32 don't implement Default, so we can't
            // derive it here
            derive_traits += ", Default";
        }

        derive_traits += ", Eq, Hash, Copy";
    }
    derive_traits += ", ReadXDR, WriteXDR";
    prtop("#[derive({})]\n", derive_traits);

    if (s.transparent) {
        needs_serde_imports = true; // for Serialize, Deserialize
        prtop("#[cfg_attr(feature = \"serde\", derive(Serialize, Deserialize))]\n");
    }

    if (s.transparent)
        prtop("#[repr(transparent)]\n");
    else
        // Do we actually need this?
        prtop("#[repr(C)]\n");

    prtop("pub struct {} ", t);
    {
        auto _close = (s.size() == 1 && s.front().id.empty())
          ? open_paren() : open_brace();
        scopes_.push_back(t);

        for (const auto &d : s) {
          show_decl(d, t);
        }
    }

    if (s.transparent) {
        // implement some conversion methods

        auto outer_id = t;
        auto inner = s.front();
        string inner_type = type_name(inner, false);
        string inner_access = (inner.id.empty()) ? "0" : inner.id;

        /*
        impl From<$InnerType> for $OuterType {
            fn from(_inner: $InnerType) -> Self {
                Self { $inner_id: _inner } // or Self(_inner)
            }
        }
        */
        prtop("\nimpl From<{}> for {} {{\n", inner_type, outer_id);
        prtop("    fn from(_inner: {}) -> Self {{\n", inner_type);
        if (inner.id.empty())
            prtop("        Self(_inner)\n");
        else
            prtop("        Self {{ {}: _inner }}\n", inner.id);
        prtop("    }}\n");
        prtop("}}");

        /*
        impl $OuterType {
            pub const fn new(_inner: $InnerType) -> Self {
                Self { $inner_field: _inner }
            }
            pub const fn into_inner(self) -> $InnerType {
                self.$inner_access
            }
        }
        */
        prtop(R"(
impl {0} {{
    pub fn into_inner(self) -> {1} {{
        self.{2}
    }}
    pub const fn new(_inner: {1}) -> Self {{
        Self {{ {3} }}
    }}
}}
)", outer_id, inner_type, inner_access, inner.id.empty() ? "_inner" : inner.id + ": _inner");

        /*
        impl Deref for $OuterType {
            type Target = $InnerType;

            fn deref(&self) -> &Self::Target {
                &self.$inner_access
            }
        }
        */
        prtop("\nimpl Deref for {} {{\n", outer_id);
        prtop("    type Target = {};\n\n", inner_type);
        prtop("    fn deref(&self) -> &Self::Target {{\n");
        prtop("        &self.{}\n", inner_access);
        prtop("    }}\n");
        prtop("}}");

        /*
        impl DerefMut for $OuterType {
            fn deref_mut(&mut self) -> &mut Self::Target {
                &mut self.$inner_access
            }
        }
        */
        prtop("\nimpl DerefMut for {} {{\n", outer_id);
        prtop("    fn deref_mut(&mut self) -> &mut Self::Target {{\n");
        prtop("        &mut self.{}\n", inner_access);
        prtop("    }}\n");
        prtop("}}");

        if (auto array_size = get_opaque_array_size(inner)) {

            /*
                impl AsRef<[u8]> for $OuterType {
                    fn as_ref(&self) -> &[u8] {
                        self.deref().as_slice()
                    }
                }

            */
            prtop("\nimpl AsRef<[u8]> for {} {{\n", outer_id);
            prtop("    fn as_ref(&self) -> &[u8] {{\n");
            prtop("        self.deref().as_slice()\n");
            prtop("    }}\n");
            prtop("}}");

            /*
            prtop("\nimpl AsRef<[u8; {}]> for {} {{\n", *array_size, outer_id);
            prtop("    fn as_ref(&self) -> &[u8; {}] {{\n", *array_size);
            prtop("        &self.{}\n", inner_access);
            prtop("    }}\n");
            prtop("}}");
            */

            /*
            impl<'a> From<&'a [u8; 32]> for &'a ContractAddress {
                fn from(arr: &[u8; 32]) -> Self {
                    let ptr = arr as *const [u8; 32] as *const ContractAddress;
                    unsafe { &*ptr }
                }
            }
            */
            prtop("\nimpl<'a> From<&'a [u8; {}]> for &'a {} {{\n", *array_size, outer_id);
            prtop("    fn from(arr: &[u8; {}]) -> Self {{\n", *array_size);
            prtop("        let ptr = arr as *const [u8; {}] as *const {};\n", *array_size, outer_id);
            prtop("        unsafe {{ &*ptr }}\n");
            prtop("    }}\n");
            prtop("}}");

            /*
            impl<'r> ReadXDR<'r> for &'r ContractAddress {
                fn read_xdr<R: xdrs::Reader<'r>>(r: &mut R) -> xdrs::ReadResult<Self> {
                    let arr: &[u8; 32] = xdrs::read_xdr(r)?;
                    let val = Self::from(arr);
                    Ok(val)
                }
            }
            */
            prtop("\nimpl<'r> ReadXDR<'r> for &'r {} {{\n", outer_id);
            prtop("    fn read_xdr<R: xdrs::Reader<'r>>(r: &mut R) -> xdrs::ReadResult<Self> {{\n");
            prtop("        let arr: &[u8; {}] = xdrs::read_xdr(r)?;\n", *array_size);
            prtop("        let val = Self::from(arr);\n");
            prtop("        Ok(val)\n");
            prtop("    }}\n");
            prtop("}}");
        }
    }
  }
}

void
GenRs::show_type(const EnumBody &e, string t)
{
  prtop(R"(
#[derive(Clone, Copy, PartialEq, Debug)]
#[repr(i32)]
pub enum {} )", t);
  auto _close = open_brace();
  scopes_.push_back(t);
  
  bool first = true, skip_anyway = false;
  for (const auto &cd : e) {
    if (!cd.comment.empty()) {
      if (!first)
        top_ += '\n';
      skip_anyway = true;
      show_comment(cd.comment, false);
    }
    else if (skip_anyway) {
      top_ += '\n';
      skip_anyway = false;
    }
    first = false;
    prtop("{} = {},\n", cd.id, cd.val);
  }
}

void
GenRs::show_type(const UnionBody &b, string t)
{
  for (const auto& a : b.arms) {
    auto const& d = a.decl;
    do_generations(t + "_" + d.id, d.type);
  }

  std::string default_decl;
  std::string default_match;

  const string switch_type = map_type(b.switch_type);

    const bool is_enum = (switch_type == b.switch_type);
    const bool is_bool = (switch_type == "bool");

    auto bool_map = [] (std::string b) -> std::string {
      if (b == "TRUE") {
        return "true";
      }
      if (b == "FALSE") {
        return "false";
      }
      throw std::runtime_error("unexpected boolean");
    };

    auto qualified_typename = [&] (std::string c) {
      if (is_enum) {
        if (is_bool) {
          return bool_map(c);
        }
        return b.switch_type + "::" + c;
      } else {
        return c;
      }
    };

    auto int_qualified_declname = [&] (std::string c) {
      if (!is_enum) {
        return std::string("_") + c;
      }
      return c;
    };
  {
    prtop("#[repr(i32)]\n");
    prtop("#[derive(PartialEq, Debug, Clone)]\n");
    prtop("#[derive(ReadXDR, WriteXDR)]\n");
    prtop("pub enum {} ", t);
    auto _close = open_brace();
    scopes_.push_back(t);

    for (const auto &a : b.arms) {
      bool double_case = false;
      auto double_case_guard = [&double_case] {
        if (double_case) {
          throw std::runtime_error("Enums with multiple cases per arm not supported at the moment");
        }
        double_case = true;
      };

      for (const auto &c : a.cases) {
        double_case_guard();
        prtop("// case {}:\n", c);
      }
      if (a.has_default) {
        double_case_guard();
        prtop("// default:\n");
      }

      show_comment(a.decl.comment, false, "//   ");
      if (a.decl.qual == Declaration::kVoid) {
        prtop("//   void;\n");
        for (const auto& c : a.cases) {
          prtop("{} = {} as i32,\n", int_qualified_declname(c), qualified_typename(c));
        }
        if (a.has_default) {
          default_decl = std::format("Default {{ {}: i32 }},", b.switch_id);
          default_match = std::format("{0}::Default{{ {1} }} => *{1} ,\n", t, b.switch_id);
        }
      }
      else
      {
        std::string name_prefix = t + "_";
        if (holds_alternative<string>(a.decl.type)) {
          name_prefix = "";
        }

        prtop("//   {} {};\n", type_name(a.decl, false), a.decl.id);

        for (const auto& c : a.cases) {
          prtop("{} {{ {}: {} }} = {} as i32,\n", int_qualified_declname(c),  a.decl.id, name_prefix + type_name(a.decl, false), qualified_typename(c));
          
          std::string match_name = std::format("{1}::{0}", int_qualified_declname(c), map_type(b.switch_type));

          if (b.switch_type == "int") {
            match_name = c;
          }
          if (b.switch_type == "unsigned int") {
            throw std::runtime_error("unsupported enum discriminant type");
          }
        }
        if (a.has_default) {
          std::format_to(
            std::back_inserter(default_decl),
            "Default {{ {}: i32, {}: {} }},", b.switch_id, a.decl.id, type_name(a.decl, false));
          default_match = std::format("{0}::Default{{ {1},..}} => *{1},\n", t, b.switch_id);
        }
      }
    }
    top_ += std::string(kIndentWidth * indent_, ' ');
    top_ += default_decl;
    top_ += "\n";
  }
  
  bottom_ += std::format(R"(impl {0} {{
    pub fn {2}(&self) -> i32 {{
        match self {{
            {1}
            _ => unsafe {{ *(self as *const Self as *const i32) }},
        }}
    }}
}}

)", t, default_match, b.switch_id);
}

void
GenRs::show_comment(std::string_view comment, bool chomp, string prefix)
{
  constexpr auto split = [](std::string_view sv) {
    if (auto pos = sv.find('\n'); pos != sv.npos)
      return std::pair{sv.substr(0, pos + 1), sv.substr(pos + 1)};
    return std::pair{sv, std::string_view{}};
  };
  constexpr auto startcol = [](std::string_view sv) {
    size_t pos = 0, col = 1;
    for (; pos < sv.size(); ++pos) {
      if (sv[pos] == '\t')
        col += 8 - ((col - 1) % 8);
      else if (sv[pos] == ' ')
        ++col;
      else
        break;
    }
    return std::pair{col, sv.substr(pos)};
  };

  if (comment.empty())
    return;

  if (comment.find_first_not_of(" \t\n") == string::npos)
    return;

  if (chomp) {
    if (comment.back() == '\n') {
      top_ += '\n';
      comment = comment.substr(0, comment.size() - 1);
    }
  }

  auto [first, rest] = split(comment);
  size_t left;
  tie(left, first) = startcol(first);
  prtop("{}{}", prefix, first);
  while (!rest.empty()) {
    tie(first, rest) = split(rest);
    std::string_view::size_type col;
    tie(col, first) = startcol(first);
    prtop("{}{}{}", prefix, col <= left ? ""s : std::string(col - left, ' '),
          first);
  }
}

string
GenRs::map_type(const string &t)
{
  static const std::map<string, string> type_map{
      {"int", "i32"},    {"unsigned int", "u32"},
      {"hyper", "i64"},  {"unsigned hyper", "u64"},
      {"opaque", "u8"},
  };
  if (auto it = type_map.find(t); it != type_map.end())
    return it->second;
  return t;
}

string
GenRs::map_case(const string &c)
{
  static const std::map<string, string> case_map{
      {"TRUE", "true"},
      {"FALSE", "false"},
  };
  if (auto it = case_map.find(c); it != case_map.end())
    return it->second;
  return c;
}

string
GenRs::map_bound(const string &b)
{
  static const string defbound = "0xffffffff";
  if (b.empty())
    return defbound;
  return b;
}

void 
GenRs::do_generations(std::string name_prefix, const TypeSpecifier &type)
{
  if (holds_alternative<string>(type)) {
    return;
  }
  show_typespec(type, name_prefix);
  top_ += "\n\n";
}

std::optional<string>
GenRs::get_opaque_array_size(const Declaration &d)
{
  if (!holds_alternative<string>(d.type)) {
    return std::nullopt;
  }
  if (d.qual == Declaration::kArray && get<string>(d.type) == "opaque") {
    return d.bound;
  }
  return std::nullopt;
}

string
GenRs::type_name(const Declaration &d, bool generate, bool in_colon_required_setting) // rust is weird
{
  using enum Declaration::Qual;

  if (d.qual == kVoid)
    return "()";

  string tname;
  if (holds_alternative<string>(d.type))
    tname = map_type(get<string>(d.type));
  else {
    if (holds_alternative<EnumBody>(d.type))
      tname = d.id;
    else
      tname = d.id;
    if (generate) {
      show_typespec(d.type, d.id);
    }
  }

  switch (d.qual) {
  case kScalar:
    return tname;
  case kPtr:
    if (in_colon_required_setting) {
      return std::format("Option::<{}>", tname);
    }
    return std::format("Option<{}>", tname);
  case kArray:
    return std::format("[{}; {}]", tname, d.bound);
  case kVec:
    {
      auto colons = in_colon_required_setting ? "::" : "";
      if (tname == "string"s || tname == "u8") {
        if (d.bound.empty())
          return std::format("BytesM");
        return std::format("BytesM{}<{}>", colons, map_bound(d.bound));
      } else {
        if (d.bound.empty())
          return std::format("VecM{}<{}>", colons, tname);
        return std::format("VecM{}<{}, {}>", colons, tname, map_bound(d.bound));
      }
    }
  default:
    return {}; // suppress warnings
  }
}

string
gen_rs(vector<Definition> *defs)
{
  GenRs gs;
  std::string preamble = R"(// -*-Rust-*-
// Generated Code: DO NOT CHECK IN

#![allow(non_camel_case_types)]
#![allow(non_upper_case_globals)]
#![allow(unused_imports)]
#![allow(dead_code)]
#![allow(unexpected_cfgs)]
#![allow(unused_variables)]

use crate::*;
use core::option::Option;
use core::ops::{Deref, DerefMut};

#[cfg(feature="std")]
use std::io::{Read, Write};

use ::xdrs::{ReadXDR, WriteXDR};

)";

  gs.process(*defs);

  std::string serde_imports = gs.needs_serde_imports ? R"(
#[cfg(feature = "serde")]
use ::serde::{Serialize, Deserialize};
#[cfg(feature = "serde")]
use ::serde_arrays;
)" : "";

  return preamble + serde_imports + gs.top_ + R"(
//
// XDR internals below this point
//

)" + gs.bottom_ + "\n";
}
