#include <cstddef>
#include <format>
#include <iterator>
#include <map>
#include <print>
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

using std::string_literals::operator""s;

struct GenRs {
  string top_;
  string bottom_;
  std::size_t indent_{};
  vector<string> namespaces_;
  vector<string> scopes_;

  constexpr static int INDENT_WIDTH = 4;

  void process(const vector<Definition> &defs);

  void show_def(const ConstDef &c)
  {
    show_comment(c.comment, true);
    prtop("pub const {}: u32 = {};\n", c.id, c.val);
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

  void show_decl(const Declaration &d, std::string name_prefix)
  {
    if (d.qual == Declaration::kVoid) {
      return;
    }
    show_comment(d.comment, false);
    if (holds_alternative<string>(d.type)) {
      prtop("pub {}: {},\n", d.id, type_name(d, false));
    } else {
      prtop("pub {}: {},\n", d.id, name_prefix + "_" + type_name(d, false));
    }
  }

  void
  do_generations(std::string name_prefix, const TypeSpecifier &type);

  void show_typespec(const TypeSpecifier &ts, string t)
  {
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

  template<typename... Args>
  void prtop(std::format_string<Args...> fmt, Args &&...args)
  {
    if (top_.empty() || top_.back() == '\n')
      top_ += std::string(INDENT_WIDTH * indent_, ' ');
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
};

void
GenRs::process(const vector<Definition> &defs)
{
  for (const auto &d : defs) {
    show_comment(d.comment, true);
    visit(
        [this](const auto &d) {
          prtop("\n");
          show_def(d);
        },
        d);
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
    prtop("#[repr(C)]\n");
    prtop("#[derive(PartialEq, Debug, Default, Clone)]\n");
    prtop("#[cfg_attr(feature=\"std\", derive(ReadXdr,WriteXdr))]\n");
    prtop("pub struct {} ", t);
    auto _close = open_brace();
    scopes_.push_back(t);
    for (const auto &d : s) {
      show_decl(d, t);
    }
  }
}

void
GenRs::show_type(const EnumBody &e, string t)
{
  prtop(R"(
#[derive(Clone, PartialEq, Debug)]
#[cfg_attr(feature="std", derive(ReadXdrEnum,WriteXdrEnum))]
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
    prtop("#[cfg_attr(feature=\"std\", derive(ReadXdr,WriteXdr))]\n");
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
          default_decl = std::format("default {{ {}: i32 }},", b.switch_id);
          default_match = std::format("{0}::default{{ {1} }} => *{1} ,\n", t, b.switch_id);
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
            "default {{ {}: i32, {}: {} }},", b.switch_id, a.decl.id, type_name(a.decl, false));
          default_match = std::format("{0}::default{{ {1},..}} => *{1},\n", t, b.switch_id);
        }
      }
    }
    top_ += std::string(INDENT_WIDTH * indent_, ' ');
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
    if (in_colon_required_setting) {
      if (tname == "string"s)
        return std::format("BytesM::<{}>", map_bound(d.bound));
      return std::format("VecM::<{}, {}>", tname, map_bound(d.bound));
    }
    if (tname == "string"s)
      return std::format("BytesM<{}>", map_bound(d.bound));
    return std::format("VecM<{}, {}>", tname, map_bound(d.bound));
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

use core::option::Option;

use crate::*;

#[cfg(feature="std")]
use std::io::{Read, Write};

)";

  gs.process(*defs);

  return preamble + gs.top_ + R"(
//
// XDR internals below this point
//

)" + gs.bottom_ + "\n";
}
