#include <algorithm>
#include <cstddef>
#include <format>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>

#include "x23/cleanup.h"
#include "x23c/defs.h"

#include "x23/config.h"

using std::optional;
using std::string_view;
using std::string_literals::operator""s;

struct GenRs {
  std::set<std::string> const &recursive_types;

  string top_;
  string bottom_;
  std::size_t indent_{};
  vector<string> namespaces_;
  vector<string> scopes_;

  bool needs_serde_imports = false;
  bool needs_xdr_common_imports = false;

  constexpr static int kIndentWidth = 4;

  void process(const vector<Definition> &defs);

  void show_def(const Definition &d)
  {
    show_comment(d.comment, true);
    prtop("\n");
    visit([this](const auto &d) { show_def(d); }, d);
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
    else if (optional<string> remainder =
                 substring_removed(c.comment, kConstU64)) {
      comment = *remainder;
      type = "u64";
    }

    show_comment(comment, false);
    prtop("pub const {} : {} = {};\n", c.id, type, val);
  }
  void show_def(const Typedef &d)
  {
    prtop("pub type {} = {};\n", d.id, type_name(d, true, true));
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

  void show_decl(const Declaration &d, string const &name_prefix)
  {
    if (d.qual == Declaration::kVoid) {
      return;
    }

    show_comment(d.comment, false);

    if (!d.attrs.empty()) {
      prtop("{}\n", d.attrs);
    }

    if (get_opaque_array_size(d).has_value() && needs_serde_imports) {
      // type is a byte array
      prtop(
          "#[cfg_attr(feature = \"serde\", serde(with = \"serde_arrays\"))]\n");
    }

    auto type_name_str = (holds_alternative<string>(d.type))
                             ? type_name(d, false, false)
                             : (name_prefix + "_" + type_name(d, false, false));

    string vis = d.is_private ? "" : "pub ";
    if (d.id.empty()) {
      prtop("{}{},\n", vis, type_name_str);
    }
    else {
      // Hack: some XDR files (stellar's) use "type" as a field name.
      // This breaks in rust, so we rename it to "type_".
      std::string id = (d.id == "type") ? "type_" : d.id;
      prtop("{}{}: {},\n", vis, id, type_name_str);
    }
  }

  void do_generations(std::string name_prefix, const TypeSpecifier &type);

  void show_typespec(const TypeSpecifier &ts, string t)
  {
    visit([this, &t](const auto &v) { show_type(v, t); }, ts);
  }

  void show_type(const string &s, string const & /*unused*/)
  {
    prtop("{}", map_type(s));
  }
  void show_type(const EnumBody &e, string const &tname);
  void show_type(const StructBody &s, string const &tname);
  void show_type(const UnionBody &b, string const &tname);

  void show_comment(std::string_view comment, bool chomp, string prefix = {});

  static string map_type(const string &t);
  static string map_case(const string &c);
  static string map_bound(const string &b);
  string type_name(const Declaration &d, bool generate,
                   bool in_colon_required_setting);
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
    return x23::Defer([this, indent = indent_, nsdepth = namespaces_.size(),
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
    Join j(std::move(with));
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

  static optional<string> substring_removed(const string &str,
                                            const string_view sub)
  {
    string::size_type i = str.find(sub);
    if (i != string::npos) {
      string str2 = string{str};
      str2.erase(i, sub.length());
      return str2;
    }
    return {};
  }

  static constexpr string_view kRustBoxPtr = R"(#[rust(box_ptr)])";
  static constexpr string_view kRustNewtype = R"(#[rust(newtype)])";
  static constexpr string_view kSerdeArrays =
      R"(#[serde(with = "serde_arrays")])";
  static constexpr string_view kConstU32 = R"(#[rust(const=u32)])";
  static constexpr string_view kConstU64 = R"(#[rust(const=u64)])";
  static constexpr string_view kDefaultDerives =
      R"(#[derive(Clone, Debug, PartialEq, Eq, Hash)])";

  optional<Definition> convert_to_newtype(const Definition &d);
};

optional<Definition>
GenRs::convert_to_newtype(const Definition &d)
{
  struct NewtypeVisitor {
    string comment;
    string attrs;

    // Convert a typedef to a struct with a single field
    Definition operator()(const Typedef &d)
    {
      Declaration inner{
          .type = d.type,
          .id = "inner", // or can leave nameless for a tuple-struct
          .qual = d.qual,
          .bound = d.bound,
          .attrs = attrs,
          .is_private = true,
      };

      StructBody t_body{.transparent = true};
      t_body.push_back(inner);

      Definition ds = StructDecl{.id = d.id, .body = t_body};
      ds.comment = comment;

      return ds;
    }

    Definition operator()(const Definition & /*unused*/)
    {
      throw std::runtime_error(
          "newtype conversion is only implemented for XDR typedefs");
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

    auto visitor = NewtypeVisitor{.comment = comment, .attrs = attrs};
    return visit(visitor, d);
  }

  return std::nullopt;
}

void
GenRs::process(const vector<Definition> &defs)
{
  for (const auto &d : defs) {
    if (optional<Definition> dn = convert_to_newtype(d)) {
      show_def(*dn);
    }
    else {
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
GenRs::show_type(const StructBody &s, string const &tname)
{
  for (const auto &d : s) {
    do_generations(tname + "_" + d.id, d.type);
  }

  {
    optional<string> array_size;

    vector<string> derive_traits = {};

    if (s.transparent) {
      auto const &inner = s.front();
      if (s.size() > 1) {
        throw std::runtime_error(
            "Transparent structs with multiple fields are not supported");
      }

      array_size = get_opaque_array_size(inner);
      if (array_size) {
        // This is a newtyped fixed-size array

        if (*array_size != "64") {
          // arrays with size over 32 don't implement Default, so we can't
          // derive it here
          derive_traits.emplace_back("Default");
        }

        derive_traits.emplace_back("Copy");

        needs_serde_imports = true; // for Serialize, Deserialize
        prtop("#[cfg_attr(feature = \"serde\", derive(Serialize, "
              "Deserialize))]\n");
      }
    }

    prtop("#[cfg_attr(feature = \"xdrs\", derive(ReadXDR, WriteXDR))]\n");
    prtop("#[cfg_attr(feature = \"spec\", derive(XDR))]\n");
    prtop("{}\n", kDefaultDerives);
    if (!derive_traits.empty())
      prtop("#[derive({})]\n", join(derive_traits, ", "));

    if (s.transparent)
      prtop("#[repr(transparent)]\n");
    else
      // Do we actually need this?
      prtop("#[repr(C)]\n");

    prtop("pub struct {} ", tname);
    {
      // NOLINTNEXTLINE(readability-identifier-naming)
      auto _close =
          (s.size() == 1 && s.front().id.empty()) ? open_paren() : open_brace();
      scopes_.push_back(tname);

      for (const auto &d : s) {
        show_decl(d, tname);
      }
    }

    if (s.transparent) {
      // implement some conversion methods

      auto outer_id = tname;
      auto inner = s.front();
      string inner_type = type_name(inner, false, false);
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
)",
            outer_id, inner_type, inner_access,
            inner.id.empty() ? "_inner" : inner.id + ": _inner");

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
        prtop("\nimpl<'a> From<&'a [u8; {}]> for &'a {} {{\n", *array_size,
              outer_id);
        prtop("    fn from(arr: &[u8; {}]) -> Self {{\n", *array_size);
        prtop("        let ptr = arr as *const [u8; {}] as *const {};\n",
              *array_size, outer_id);
        prtop("        unsafe {{ &*ptr }}\n");
        prtop("    }}\n");
        prtop("}}\n");

        /*
        impl<'r> ReadXDR<'r> for &'r ContractAddress {
            fn read_xdr<R: xdrs::Reader<'r>>(r: &mut R) ->
        xdrs::ReadResult<Self> { let arr: &[u8; 32] = xdrs::read_xdr(r)?; let
        val = Self::from(arr); Ok(val)
            }
        }
        */
        prtop("#[cfg(feature = \"xdrs\")]\n");
        prtop("impl<'r> ReadXDR<'r> for &'r {} {{\n", outer_id);
        prtop("    fn read_xdr<R: xdrs::Reader<'r>>(r: &mut R) -> "
              "xdrs::ReadResult<Self> {{\n");
        prtop("        let arr: &[u8; {}] = xdrs::read_xdr(r)?;\n",
              *array_size);
        prtop("        let val = Self::from(arr);\n");
        prtop("        Ok(val)\n");
        prtop("    }}\n");
        prtop("}}\n");
      }
    }
  }
}

void
GenRs::show_type(const EnumBody &e, string const &tname)
{
  prtop(R"(#[cfg_attr(feature = "xdrs", derive(ReadXDR, WriteXDR))])");
  prtop("\n");
  prtop("#[cfg_attr(feature = \"spec\", derive(XDR))]\n");
  prtop("{}\n", kDefaultDerives);
  prtop(R"(#[derive(Copy)]
#[repr(i32)]
pub enum {} )",
        tname);
  {
    // NOLINTNEXTLINE(readability-identifier-naming)
    auto _close = open_brace();
    scopes_.push_back(tname);

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
      prtop("{} = {} as i32,\n", cd.id, cd.val);
    }
  }

  // Export the variants of the enum
  prtop("\npub use {}::*;\n", tname);

  // generate a TryFrom implementation for the enum
  prtop("\nimpl TryFrom<i32> for {} {{\n", tname);
  prtop("    type Error = ();\n");
  prtop("    fn try_from(value: i32) -> Result<Self, Self::Error> {{\n");
  prtop("        match value {{\n");
  for (const auto &cd : e) {
    prtop("            {} => Ok(Self::{}),\n", cd.val, cd.id);
  }
  prtop("            _ => Err(()),\n");
  prtop("        }}\n");
  prtop("    }}\n");
  prtop("}}\n");
}

void
GenRs::show_type(const UnionBody &b, string const &tname)
{
  for (const auto &a : b.arms) {
    auto const &d = a.decl;
    do_generations(tname + "_" + d.id, d.type);
  }

  std::string default_decl;
  std::string default_match;

  const string switch_type = map_type(b.switch_type);

  const bool is_enum = (switch_type == b.switch_type);
  const bool is_bool = (switch_type == "bool");

  auto bool_map = [](std::string_view b) -> std::string {
    if (b == "TRUE") {
      return "true";
    }
    if (b == "FALSE") {
      return "false";
    }
    throw std::runtime_error("unexpected boolean");
  };

  auto qualified_typename = [&](std::string c) {
    if (is_enum) {
      if (is_bool) {
        return bool_map(c);
      }
      return b.switch_type + "::" + c;
    }
    return c;
  };

  auto int_qualified_declname = [&](std::string c) {
    if (!is_enum) {
      return std::string("_") + c;
    }
    return c;
  };
  auto enum_base_type = [&]() -> std::string {
    // TODO(geoff): If we care, we can add support for repr(u32) enums,
    // but this requires changes in the derive(ReadXDR, WriteXDR) macros
    // if (b.switch_type == "bool" || b.switch_type == "unsigned int") {
    //   return "u32";
    // }
    return "i32";
  };
  {
    prtop("{}\n", kDefaultDerives);
    prtop("#[cfg_attr(feature = \"xdrs\", derive(ReadXDR, WriteXDR))]\n");
    prtop("#[cfg_attr(feature = \"spec\", derive(XDR))]\n");
    prtop("#[repr({})]\n", enum_base_type());
    prtop("pub enum {} ", tname);
    // NOLINTNEXTLINE(readability-identifier-naming)
    auto _close = open_brace();
    scopes_.push_back(tname);

    bool double_default = false;
    auto double_default_guard = [&double_default] {
      if (double_default) {
        throw std::runtime_error("Enums with multiple defaults not supported!");
      }
      double_default = true;
    };

    for (const auto &a : b.arms) {

      for (const auto &c : a.cases) {
        prtop("// case {}:\n", c);
      }
      if (a.has_default) {
        double_default_guard();
        prtop("// default:\n");
      }

      show_comment(a.decl.comment, false, "//   ");
      if (a.decl.qual == Declaration::kVoid) {
        prtop("//   void;\n");
        for (const auto &c : a.cases) {
          prtop("{} = {} as i32,\n", int_qualified_declname(c),
                qualified_typename(c));
        }
        if (a.has_default) {
          default_decl = std::format("Default {{ {}: {} }},", b.switch_id,
                                     enum_base_type());
          default_match = std::format("{0}::Default{{ {1} }} => *{1} ,\n",
                                      tname, b.switch_id);
        }
      }
      else {
        std::string name_prefix = tname + "_";
        if (holds_alternative<string>(a.decl.type)) {
          name_prefix = "";
        }

        prtop("//   {} {};\n", type_name(a.decl, false, false), a.decl.id);

        for (const auto &c : a.cases) {
          prtop("{} {{ {}: {} }} = {} as {},\n", int_qualified_declname(c),
                a.decl.id, name_prefix + type_name(a.decl, false, false),
                qualified_typename(c), enum_base_type());

          std::string match_name =
              std::format("{1}::{0}", int_qualified_declname(c),
                          map_type(b.switch_type));

          if (b.switch_type == "int" || b.switch_type == "unsigned int") {
            match_name = c;
          }
        }
        if (a.has_default) {
          std::format_to(std::back_inserter(default_decl),
                         "Default {{ {}: {}, {}: {} }},", b.switch_id,
                         enum_base_type(), a.decl.id,
                         type_name(a.decl, false, false));
          default_match = std::format("{0}::Default{{ {1},..}} => *{1},\n",
                                      tname, b.switch_id);
        }
      }
    }
    top_ += std::string(kIndentWidth * indent_, ' ');
    top_ += default_decl;
    top_ += "\n";
  }

  // Rust doesn't support having methods named "type"; switch that to "type_"
  // if we're generating from xdr that we can't patch.
  std::string switch_id = b.switch_id;
  if (switch_id == "type") {
    switch_id = "type_";
  }

  bottom_ += std::format(R"(impl {0} {{
    pub fn {2}(&self) -> {3} {{
        match self {{
            {1}
            _ => unsafe {{ *(self as *const Self as *const {3}) }},
        }}
    }}
}}

)",
                         tname, default_match, switch_id, enum_base_type());
}

void
GenRs::show_comment(std::string_view comment, bool chomp, string prefix)
{
  constexpr auto kSplit = [](std::string_view sv) {
    // NOLINTNEXTLINE(readability-static-accessed-through-instance)
    if (auto pos = sv.find('\n'); pos != sv.npos)
      return std::pair{sv.substr(0, pos + 1), sv.substr(pos + 1)};
    return std::pair{sv, std::string_view{}};
  };
  constexpr auto kStartcol = [](std::string_view sv) {
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

  auto [first, rest] = kSplit(comment);
  size_t left;
  tie(left, first) = kStartcol(first);
  prtop("{}{}", prefix, first);
  while (!rest.empty()) {
    tie(first, rest) = kSplit(rest);
    std::string_view::size_type col;
    tie(col, first) = kStartcol(first);
    prtop("{}{}{}", prefix, col <= left ? ""s : std::string(col - left, ' '),
          first);
  }
}

string
GenRs::map_type(const string &t)
{
  static const std::map<string, string> type_map{
      {"int", "i32"},   {"unsigned int", "u32"},
      {"hyper", "i64"}, {"unsigned hyper", "u64"},
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
  show_typespec(type, std::move(name_prefix));
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
GenRs::type_name(const Declaration &d, bool generate,
                 bool in_colon_required_setting) // rust is weird
{
  using enum Declaration::Qual;

  if (d.qual == kVoid)
    return "()";

  string tname;
  if (holds_alternative<string>(d.type))
    tname = map_type(get<string>(d.type));
  else {
    tname = d.id;
    if (generate) {
      show_typespec(d.type, d.id);
    }
  }

  bool needs_box = (d.comment.find(kRustBoxPtr) != std::string::npos) ||
                   recursive_types.contains(tname);

  switch (d.qual) {
  case kScalar:
    return tname;
  case kPtr:
  {
    if (needs_box) {
      if (in_colon_required_setting) {
        return std::format("Option::<Box::<{}>>", tname);
      }
      return std::format("Option<Box<{}>>", tname);
    }
    if (in_colon_required_setting) {
      return std::format("Option::<{}>", tname);
    }
    return std::format("Option<{}>", tname);
  }
  case kArray:
  {
    if (needs_box) {
      return std::format("[Box<{}>; {}]", tname, d.bound);
    }
    return std::format("[{}; {}]", tname, d.bound);
  }
  case kVec:
  {
    std::string_view colons = in_colon_required_setting ? "::" : "";
    needs_xdr_common_imports = true;
    if (tname == "string"s || tname == "u8") {
      if (d.bound.empty())
        return std::format("BytesM");
      return std::format("BytesM{}<{}>", colons, map_bound(d.bound));
    }

    // Otherwise, vector of some non-opaque type
    if (d.bound.empty()) {
      if (needs_box) {
        return std::format("VecM{}<Box<{}>>", colons, tname);
      }
      return std::format("VecM{}<{}>", colons, tname);
    }
    if (needs_box) {
      return std::format("VecM{}<Box<{}>, {}>", colons, tname,
                         map_bound(d.bound));
    }
    return std::format("VecM{}<{}, {}>", colons, tname, map_bound(d.bound));
  }
  default:
    return {}; // suppress warnings
  }
}

string
gen_rs(vector<Definition> *defs, std::set<std::string> const &recursive_types)
{
  GenRs gs(recursive_types);
  std::string preamble = R"(// -*-Rust-*-
// Generated Code: DO NOT CHECK IN

// Generated from xdrc commit: )" +
                         std::string(GIT_COMMIT_HASH) + R"(

#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(non_upper_case_globals)]
#![allow(unused_imports)]
#![allow(dead_code)]
#![allow(unexpected_cfgs)]
#![allow(unused_variables)]
#![allow(clippy::unnecessary_cast)]

use crate::*;
use core::option::Option;
use core::ops::{Deref, DerefMut};

#[cfg(feature="std")]
use std::io::{Read, Write};

#[cfg(feature="xdrs")]
use xdrs::*;
#[cfg(feature="spec")]
use xdrs::xdr_repr::*;

)";

  gs.process(*defs);

  // xdrs now re-exports xdr_common, so only include that here if not using xdrs
  if (gs.needs_xdr_common_imports) {
    preamble += R"(
#[cfg(not(feature="xdrs"))]
use xdr_common::*;
)";
  }

  if (gs.needs_serde_imports) {
    preamble += R"(
#[cfg(feature = "serde")]
use ::serde::{Serialize, Deserialize};
#[cfg(feature = "serde")]
use ::serde_arrays;
)";
  }

  return preamble + gs.top_ + R"(
//
// XDR internals below this point
//

)" + gs.bottom_ +
         "\n";
}
