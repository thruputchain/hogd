#include <algorithm>
#include <cassert>
#include <concepts>
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
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "x23/cleanup.h"
#include "x23c/defs.h"

#include "x23c/config.h"

using std::optional;
using std::string_view;
using std::string_literals::operator""s;

struct GenRs {
  std::set<std::string> const &recursive_types;
  std::set<std::string> const &rust_features;
  std::set<std::string> const &rust_no_features;

  string top_;
  string bottom_;
  std::size_t indent_{};
  vector<string> namespaces_;
  vector<string> scopes_;

  constexpr static int kIndentWidth = 4;

  void process(const vector<Definition> &defs);

  void show_def(const Definition &d)
  {
    // const traits are still unstable in Rust, so ConstWriteXDR is opt-in per
    // type via a #[rust(ConstWriteXDR)] comment marker rather than derived on
    // everything.  Strip the marker here and thread the resulting flag down to
    // the type-defining overloads.
    string comment = d.comment;
    bool const_write_xdr = false;
    if (optional<string> stripped = line_removed(comment, kConstWriteXDR)) {
      comment = std::move(*stripped);
      const_write_xdr = true;
    }
    show_comment(comment, true);
    prtop("\n");
    visit(
        [this, const_write_xdr](const auto &def) {
          using T = std::decay_t<decltype(def)>;
          if constexpr (std::same_as<T, EnumDecl> ||
                        std::same_as<T, StructDecl> ||
                        std::same_as<T, UnionDecl>) {
            show_def(def, const_write_xdr);
          }
          else {
            show_def(def);
          }
        },
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
  void show_def(const EnumDecl &t, bool const_write_xdr)
  {
    show_typespec(t.body, t.id, const_write_xdr);
    prtop("\n");
  }
  void show_def(const StructDecl &t, bool const_write_xdr)
  {
    show_typespec(t.body, t.id, const_write_xdr);
    prtop("\n");
  }
  void show_def(const UnionDecl &t, bool const_write_xdr)
  {
    show_typespec(t.body, t.id, const_write_xdr);
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

  void show_vers_trait(const VersDef &vers, const std::string &prog_name);
  void show_vers_client(const VersDef &vers, const std::string &prog_name,
                        const std::string &prog_id);

  void show_vers(const VersDef &vers, const std::string &prog_name,
                 const std::string &prog_id);
  void show_server_dispatch(VersDef const &vers, std::string const &prog_name,
                            std::string const &prog_id);

  void show_decl(const Declaration &d, string const &name_prefix)
  {
    if (d.qual == Declaration::kVoid) {
      return;
    }

    show_comment(d.comment, false);

    if (!d.attrs.empty()) {
      prtop("{}\n", d.attrs);
    }

    if (get_opaque_array_size(d).has_value()) {
      // type is a byte array
      prtop("{}\n", feature_attr("serde", "serde(with = "
                                          "\"serde_arrays::hex\")"));
    }

    auto type_name_str = (holds_alternative<string>(d.type))
                             ? type_name(d, false, false)
                             : (name_prefix + "_" + type_name(d, false, false));

    string vis = d.is_private ? "" : "pub ";
    if (d.id.empty()) {
      prtop("{}{},\n", vis, type_name_str);
    }
    else {
      // Rust-reserved field names (e.g. "type") are escaped via r#.
      prtop("{}{}: {},\n", vis, escape_rust_id(d.id), type_name_str);
    }
  }

  void do_generations(std::string name_prefix, const TypeSpecifier &type,
                      bool const_write_xdr);

  void show_typespec(const TypeSpecifier &ts, string t, bool const_write_xdr)
  {
    visit([this, &t, const_write_xdr](
              const auto &v) { show_type(v, t, const_write_xdr); },
          ts);
  }

  void show_type(const string &s, string const & /*unused*/,
                 bool /*const_write_xdr*/)
  {
    prtop("{}", map_type(s));
  }
  void show_type(const EnumBody &e, string const &tname, bool const_write_xdr);
  void show_type(const StructBody &s, string const &tname,
                 bool const_write_xdr);
  void show_type(const UnionBody &b, string const &tname, bool const_write_xdr);

  void show_comment(std::string comment, bool chomp, string prefix = {});

  static string map_type(const string &t);
  static string map_proc_return_type(const string &t);
  static string map_case(const string &c);
  static string map_bound(const string &b);
  // Wraps Rust-reserved identifiers in raw-identifier syntax (r#) so XDR
  // names that are legal per RFC 4506 but reserved in Rust (e.g. "type")
  // pass through unchanged at the source level.
  static string escape_rust_id(const string &id);
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

  string feature_attr(const std::string &feature, const std::string &attr)
  {
    if (rust_features.contains(feature)) {
      return std::format("#[{}]", attr);
    }
    if (rust_no_features.contains(feature)) {
      return "";
    }
    return std::format("#[cfg_attr(feature = \"{}\", {})]", feature, attr);
  }

  // The xdrs-feature derive list.  ConstXDRSize is always safe to derive, but
  // ConstWriteXDR relies on const traits (still unstable), so it is only
  // emitted for types opted in via a #[rust(ConstWriteXDR)] comment marker.
  string xdrs_derive_attr(bool const_write_xdr)
  {
    return feature_attr(
        "xdrs", const_write_xdr
                    ? "derive(ReadXDR, WriteXDR, ConstWriteXDR, ConstXDRSize)"
                    : "derive(ReadXDR, WriteXDR, ConstXDRSize)");
  }

  string cfg(const std::string &feature, const std::string &value)
  {
    if (rust_features.contains(feature)) {
      return value;
    }
    if (rust_no_features.contains(feature)) {
      return "";
    }
    return std::format("#[cfg(feature = \"{}\")]\n{}", feature, value);
  }

  static optional<string> substring_removed(std::string_view str,
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

  // Remove the whole comment line carrying `sub` (its leading whitespace, the
  // // prefix, and the trailing newline), returning nullopt if absent.  Unlike
  // substring_removed this leaves no empty "//" residue when a type-level
  // marker sits on its own comment line.  The marker is expected on its own
  // line; any text sharing the line is removed with it.
  static optional<string> line_removed(std::string_view str,
                                       const string_view sub)
  {
    string::size_type i = str.find(sub);
    if (i == string::npos)
      return {};
    string::size_type line_start = str.rfind('\n', i);
    line_start = (line_start == string::npos) ? 0 : line_start + 1;
    string::size_type nl = str.find('\n', i);
    string::size_type line_end = (nl == string::npos) ? str.size() : nl + 1;
    string str2 = string{str};
    str2.erase(line_start, line_end - line_start);
    return str2;
  }

  static constexpr string_view kRustBoxPtr = R"(#[rust(box_ptr)])";
  static constexpr string_view kRustNewtype = R"(#[rust(newtype)])";
  static constexpr string_view kSerdeArrays =
      R"(#[serde(with = "serde_arrays::hex")])";
  static constexpr string_view kConstU32 = R"(#[rust(const=u32)])";
  static constexpr string_view kConstU64 = R"(#[rust(const=u64)])";
  static constexpr string_view kConstWriteXDR = R"(#[rust(ConstWriteXDR)])";
  static constexpr string_view kDocHidden = R"(#[doc(hidden)])";
  static constexpr string_view kDefaultDerives =
      R"(#[derive(Clone, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)])";

  static optional<Definition> convert_to_newtype(const Definition &d);
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

// Generate the trait that the server implements to support the rpc
void
GenRs::show_vers_trait(const VersDef &vers, const std::string &prog_name)
{
  prtop(R"(
#[allow(non_camel_case_types)]
#[async_trait::async_trait]
pub trait {}_{}: Send + Sync {{
)",
        prog_name, vers.id);

  for (auto const &proc : vers.procs) {
    std::vector<string> args = {
        "&self",
    };
    for (size_t i = 0; i < proc.arg_types.size(); i++) {
      args.push_back(std::format("arg{} : {}", i, map_type(proc.arg_types[i])));
    }
    if (!proc.comment.empty())
      prtop("{}\n", proc.comment);
    prtop("    async fn {}({}) -> Result<{}, ::rpc_driver::ProgramError>;\n",
          proc.id, join(args, ", "), map_proc_return_type(proc.return_type));
  }

  prtop("}}\n");
}

void
GenRs::show_server_dispatch(VersDef const &vers, std::string const &prog_name,
                            std::string const &prog_id)
{
  prtop(R"(
#[allow(non_camel_case_types)]
pub struct {0}_{1}_Dispatcher {{
  inner: Box<dyn {0}_{1}>,
}}

impl {0}_{1}_Dispatcher {{
  pub fn new(inner: Box<dyn {0}_{1}>) -> Self {{
    Self {{ inner }}
  }}
}}

#[async_trait::async_trait]
impl ::rpc_driver::handler::RpcHandler for {0}_{1}_Dispatcher {{
    fn prog_vers(&self) -> ::rpc_driver::ProgVers {{
        ::rpc_driver::ProgVers::new({2}, {3})
    }}
    async fn dispatch(&self, proc: u32, data: &[u8]) -> Result<Vec<u8>, ::rpc_driver::RpcError> {{
        match proc {{
)",
        prog_name, vers.id, prog_id, vers.val);

  auto make_arg_detuple = [&](size_t n) {
    std::vector<string> out;
    out.reserve(n);
    for (size_t i = 0; i < n; i++) {
      out.push_back(std::format("args.{}", i));
    }
    return join(out, ", ");
  };

  for (auto const &proc : vers.procs) {
    std::string type_args;
    for (auto const &arg : proc.arg_types) {
      type_args += map_type(arg) + ", ";
    }

    prtop("            // {}\n", proc.id);
    prtop("            {} => {{\n", proc.val);
    prtop("                let args: ({}) =\n", type_args);
    prtop("                    xdrs::txs::decode(data).map_err(|_| "
          "::rpc_driver::RpcError::GarbageArguments)?;\n");
    auto detuple = make_arg_detuple(proc.arg_types.size());
    prtop("                let res = self.inner.{}({}).await.map_err(|_| "
          "::rpc_driver::RpcError::SystemError)?;\n",
          proc.id, detuple);
    prtop("                xdrs::txs::encode(&res).map_err(|_| "
          "::rpc_driver::RpcError::SystemError)\n");
    prtop("            }}\n");
  }

  prtop("            _ => Err(::rpc_driver::RpcError::InvalidProcedure),\n");

  prtop("        }}\n"); // close match
  prtop("    }}\n");     // close fn
  prtop("}}");           // close trait impl
}

void
GenRs::show_vers_client(const VersDef &vers, const std::string &prog_name,
                        const std::string &prog_id)
{

  prtop(R"(
#[derive(Clone)]
pub struct {0}_{1}_Client {{
    handle: ::std::sync::Arc<dyn ::rpc_driver::RpcSender>,
}}

impl {0}_{1}_Client {{
    pub fn new(handle: ::std::sync::Arc<dyn ::rpc_driver::RpcSender>) -> Self {{
        Self {{ handle }}
    }}

    fn prog_vers() -> ::rpc_driver::ProgVers {{
        ::rpc_driver::ProgVers::new({2}, {3})
    }}
)",
        prog_name, vers.id, prog_id, vers.val);
  auto is_ref_type = [](const string &type) -> string {
    const static std::set<string_view> nonref_types = {"hyper",
                                                       "unsigned hyper", "bool",
                                                       "int", "unsigned int"};
    if (nonref_types.contains(type)) {
      return "";
    }
    return "&";
  };

  for (auto const &proc : vers.procs) {
    std::vector<string> args = {"&self"};
    std::vector<string> call_args;
    for (size_t i = 0; i < proc.arg_types.size(); i++) {
      args.push_back(std::format("arg{} : {}{}", i,
                                 is_ref_type(proc.arg_types[i]),
                                 map_type(proc.arg_types[i])));
      call_args.push_back(
          std::format("{}arg{}", is_ref_type(proc.arg_types[i]), i));
    }
    prtop("    pub async fn {}({}) -> Result<{}, ::rpc_driver::RpcError> {{\n",
          proc.id, join(args, ", "), map_proc_return_type(proc.return_type));
    prtop("         let data = xdrs::txs::encode(&({})).map_err(|_| "
          "::rpc_driver::RpcError::GarbageArguments)?;\n",
          join(call_args, ", "));
    prtop("         let buf = self.handle.send_call(Self::prog_vers(), {}, "
          "&data).await?;\n",
          proc.val);
    prtop("         xdrs::txs::decode(&buf).map_err(|_| "
          "::rpc_driver::RpcError::SystemError)\n");
    prtop("    }}\n");
  }
  prtop("}}\n");
  prtop(R"(
impl From<::std::sync::Arc<dyn ::rpc_driver::RpcSender>> for {0}_{1}_Client {{
    fn from(handle: ::std::sync::Arc<dyn ::rpc_driver::RpcSender>) -> Self {{
        Self::new(handle)
    }}
}}
)",
        prog_name, vers.id);
}

void
GenRs::show_vers(const VersDef &vers, const std::string &prog_name,
                 const std::string &prog_id)
{
  show_vers_trait(vers, prog_name);
  prtop("\n");
  show_server_dispatch(vers, prog_name, prog_id);
  prtop("\n");
  show_vers_client(vers, prog_name, prog_id);
  prtop("\n");
}

void
GenRs::show_def(const ProgDef &prog)
{
  for (auto const &vers : prog.versions) {
    show_vers(vers, prog.id, prog.val);
  }
}

void
GenRs::show_type(const StructBody &s, string const &tname, bool const_write_xdr)
{
  for (const auto &d : s) {
    do_generations(tname + "_" + d.id, d.type, const_write_xdr);
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
        size_t array_size_int = std::stoul(*array_size);
        if (array_size_int <= 32) {
          // arrays with size over 32 don't implement Default, so we can't
          // derive it here
          derive_traits.emplace_back("Default");
        }

        derive_traits.emplace_back("Copy");
      }
    }

    prtop("{}\n", feature_attr("serde", "derive(Serialize, Deserialize)"));
    prtop("{}\n", xdrs_derive_attr(const_write_xdr));
    prtop("{}\n", feature_attr("spec", "derive(XDR)"));
    if (!array_size)
      prtop("{}\n", feature_attr("schema", "derive(schemars::JsonSchema)"));
    prtop("{}\n", kDefaultDerives);
    if (!derive_traits.empty())
      prtop("#[derive({})]\n", join(derive_traits, ", "));

    if (s.transparent) {
      prtop("#[repr(transparent)]\n");
      prtop("{}\n", feature_attr("serde", "serde(transparent)"));
    }
    else
      // We want this, at least optionally on some structs,
      // to enable FFI usage via memcpy instead of a full de/serialize via
      // XDR.
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
      // inner_access is used both as a field-access (self.<x>) and as a
      // binding name; either way an r#-escaped form is correct Rust.
      string inner_access = (inner.id.empty()) ? "0" : escape_rust_id(inner.id);

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
        prtop("        Self {{ {}: _inner }}\n", escape_rust_id(inner.id));
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
            inner.id.empty() ? "_inner"
                             : escape_rust_id(inner.id) + ": _inner");

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

        auto xdrs_read = std::format(R"(impl<'r> ReadXDR<'r> for &'r {1} {{
  fn read_xdr<R: xdrs::Reader<'r>>(r: &mut R) -> xdrs::ReadResult<Self> {{
      let arr: &[u8; {0}] = xdrs::read_xdr(r)?;
      let val = Self::from(arr);
      Ok(val)
  }}
}}
)",
                                     *array_size, outer_id);
        prtop("{}\n", cfg("xdrs", xdrs_read));

        auto schema_impl = std::format(
            R"(impl schemars::JsonSchema for {0} {{
    fn schema_name() -> ::std::borrow::Cow<'static, str> {{
        "{0}".into()
    }}
    fn json_schema(_gen: &mut schemars::SchemaGenerator) -> schemars::Schema {{
        schemars::json_schema!({{
            "type": "string",
            "contentEncoding": "hex",
            "contentMediaType": "application/binary",
            "minLength": {1},
            "maxLength": {1}
        }})
    }}
}}
)",
            outer_id, std::stoul(*array_size) * 2);
        prtop("{}\n", cfg("schema", schema_impl));
      }
    }
  }
}

void
GenRs::show_type(const EnumBody &e, string const &tname, bool const_write_xdr)
{
  prtop("{}\n", feature_attr("serde", "derive(Serialize, Deserialize)"));
  prtop("{}\n", feature_attr("serde", "serde(transparent)"));
  prtop("{}\n", xdrs_derive_attr(const_write_xdr));
  prtop("{}\n", feature_attr("spec", "derive(XDR)"));
  prtop("{}\n", feature_attr("schema", "derive(schemars::JsonSchema)"));
  prtop("{}\n", kDefaultDerives);
  prtop(R"(#[derive(Copy)]
#[repr(transparent)]
pub struct {}(pub i32);
)",
        tname);

  std::string match_block;

  prtop("\nimpl {} ", tname);
  {
    // NOLINTNEXTLINE(readability-identifier-naming)
    auto _close = open_brace();

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
      prtop("pub const {}: Self = Self({});\n", cd.id, cd.val);
      match_block += std::format(R"(
            x if x == Self::{1}.0 => {{
                write!(f, "{0}::{1}")
            }},)",
                                 tname, cd.id);
    }
  }
  prtop("\n");

  // generate a From<i32> implementation for the newtype struct
  prtop("impl From<i32> for {} {{\n", tname);
  prtop("    fn from(v: i32) -> Self {{\n");
  prtop("        Self(v)\n");
  prtop("    }}\n");
  prtop("}}\n");

  std::string write_block =
      std::format("write!(f, \"{0}({{}})\", self.0)", tname);

  std::string display_block = std::format(R"(impl core::fmt::Display for {0} {{
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {{
        match self.0 {{{1}
            _ => {{
                {2}
            }}
        }}
    }}
}})",
                                          tname, match_block, write_block);
  prtop("{}\n", cfg("std", display_block));
}

void
GenRs::show_type(const UnionBody &b, string const &tname, bool const_write_xdr)
{
  for (const auto &a : b.arms) {
    auto const &d = a.decl;
    do_generations(tname + "_" + d.id, d.type, const_write_xdr);
  }

  std::string default_decl;
  std::string default_match;

  // b.switch_id appears in generated Rust as (a) a field name in the
  // Default variant, (b) a pattern/expression binding inside the match
  // arm in bottom_, and (c) the method name on the impl block.  All three
  // accept raw identifiers, so we escape once and use the result
  // everywhere.
  const std::string switch_id = escape_rust_id(b.switch_id);

  const string switch_type = map_type(b.switch_type);

  const bool is_enum = (switch_type == b.switch_type);
  const bool is_bool = (switch_type == "bool");

  // Convert an i32 expression to the switch_type.  Enum newtypes have
  // From<i32>, but primitive types like u32/bool do not.
  auto convert_i32 = [&](std::string expr) -> std::string {
    if (is_bool) {
      return std::format("({}) != 0", expr);
    }
    if (is_enum) {
      return std::format("({}).into()", expr);
    }
    if (switch_type == "i32") {
      return expr;
    }
    // u32, i64, u64, etc.
    return std::format("({}) as {}", expr, switch_type);
  };

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
      // Enum types are newtype structs with associated constants,
      // so access the inner value with .0
      return b.switch_type + "::" + c + ".0";
    }
    return c;
  };

  auto int_qualified_declname = [&](std::string c) {
    if (!is_enum) {
      return std::string("_") + c;
    }
    return c;
  };

  // We could change this base type if we need to, for e.g. boolean enums or
  // unsigned int enums, but this suffices for now.  This would require changes
  // to the derive(ReadXDR, WriteXDR) macros if we want to support that.
  // It's also not strictly necessary, given that the serialization of bool
  // is equivalent to the serialization of a uint32, and the serialization
  // of a uint32 is equivalent to the serialization of an i32 (converted to
  // bytes).
  constexpr static auto kEnumBaseType = "i32";
  {
    prtop("{}\n", kDefaultDerives);
    prtop("{}\n", feature_attr("serde", "derive(Serialize, Deserialize)"));
    prtop("{}\n", xdrs_derive_attr(const_write_xdr));
    prtop("{}\n", feature_attr("spec", "derive(XDR)"));
    prtop("{}\n", feature_attr("schema", "derive(schemars::JsonSchema)"));
    // This repr(i32) suffices to fully specify the layout of the enum, making
    // it FFI-safe. See Rust RFC 2195:
    // https://github.com/rust-lang/rfcs/blob/master/text/2195-really-tagged-unions.md
    // This is not the standard "tagged union" layout (that's repr(C, i32)),
    // but the layout is still well-defined and can be generated by cbindgen.
    prtop("#[repr({})]\n", kEnumBaseType);
    prtop("#[non_exhaustive]\n");
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

      show_comment(a.decl.comment, false);

      for (const auto &c : a.cases) {
        prtop("// case {}:\n", c);
      }
      if (a.has_default) {
        double_default_guard();
        prtop("// default:\n");
      }

      if (a.decl.qual == Declaration::kVoid) {
        prtop("//   void;\n");
        for (const auto &c : a.cases) {
          prtop("{} = {} as i32,\n", int_qualified_declname(c),
                qualified_typename(c));
        }
        if (a.has_default) {
          default_decl =
              std::format("#[xdr(default_variant)] Default {{ {}: {} }},",
                          switch_id, kEnumBaseType);
          default_match =
              std::format("{0}::Default{{ {1} }} => {2},\n", tname, switch_id,
                          convert_i32(std::format("*{}", switch_id)));
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
                escape_rust_id(a.decl.id),
                name_prefix + type_name(a.decl, false, false),
                qualified_typename(c), kEnumBaseType);

          std::string match_name =
              std::format("{1}::{0}", int_qualified_declname(c),
                          map_type(b.switch_type));

          if (b.switch_type == "int" || b.switch_type == "unsigned int") {
            match_name = c;
          }
        }
        if (a.has_default) {
          std::format_to(
              std::back_inserter(default_decl),
              "#[xdr(default_variant)] Default {{ {}: {}, {}: {} }},",
              switch_id, kEnumBaseType, escape_rust_id(a.decl.id),
              type_name(a.decl, false, false));
          default_match =
              std::format("{0}::Default{{ {1},..}} => {2},\n", tname, switch_id,
                          convert_i32(std::format("*{}", switch_id)));
        }
      }
    }
    top_ += std::string(kIndentWidth * indent_, ' ');
    top_ += default_decl;
    top_ += "\n";
  }

  std::string unsafe_read =
      std::format("unsafe {{ *(self as *const Self as *const {}) }}",
                  kEnumBaseType);
  std::string converted_read = convert_i32(unsafe_read);

  bottom_ +=
      std::format(R"(impl {0} {{
    pub fn {2}(&self) -> {3} {{
        match self {{
            {1}
            _ => {4},
        }}
    }}
}}

)",
                  tname, default_match, switch_id, switch_type, converted_read);
}

void
GenRs::show_comment(std::string comment, bool chomp, string prefix)
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
  if (auto stripped = substring_removed(comment, kDocHidden)) {
    comment = *stripped + "\n#[doc(hidden)]";
  }

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
GenRs::map_proc_return_type(const string &t)
{
  if (t == "void") {
    return "()";
  }
  return map_type(t);
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

string
GenRs::escape_rust_id(const string &id)
{
  // Rust strict + edition keywords that are also legal XDR (RFC 4506)
  // identifiers.  Keywords that XDR itself reserves (const, enum, struct,
  // true, false) are omitted since they can never appear as XDR names.
  // "crate", "self", "Self", and "super" are also omitted: they cannot be
  // raw identifiers, and they are unlikely to occur as XDR field names.
  static const std::set<string> kRustKeywords = {
      "as",   "async",  "await", "break",  "continue", "dyn",
      "else", "extern", "fn",    "for",    "if",       "impl",
      "in",   "let",    "loop",  "match",  "mod",      "move",
      "mut",  "pub",    "ref",   "return", "static",   "trait",
      "type", "unsafe", "use",   "where",  "while",
  };
  if (kRustKeywords.contains(id)) {
    return "r#" + id;
  }
  return id;
}

void
GenRs::do_generations(std::string name_prefix, const TypeSpecifier &type,
                      bool const_write_xdr)
{
  if (holds_alternative<string>(type)) {
    return;
  }
  show_typespec(type, std::move(name_prefix), const_write_xdr);
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
      // Inline types generated from a typedef alias; ConstWriteXDR is opt-in
      // on top-level struct/union/enum declarations, not typedef inline types.
      show_typespec(d.type, d.id, /*const_write_xdr=*/false);
    }
  }

  bool needs_box = (d.comment.find(kRustBoxPtr) != std::string::npos) ||
                   recursive_types.contains(tname);
  std::string_view colons = in_colon_required_setting ? "::" : "";

  switch (d.qual) {
  case kScalar:
    if (needs_box) {
      if (in_colon_required_setting) {
        return std::format("Box::<{}>", tname);
      }
      return std::format("Box<{}>", tname);
    }
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
      return std::format("[Box<{}>; {} as usize]", tname, d.bound);
    }
    return std::format("[{}; {} as usize]", tname, d.bound);
  }
  case kVec:
  {
    assert(tname != "string"s);
    if (tname == "u8") {
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
  case kString:
  {
    if (d.bound.empty()) {
      return "StringM";
    }
    return std::format("StringM{}<{}>", colons, map_bound(d.bound));
  }
  case kVoid:
    return {}; // nothing to do
  }
  throw std::runtime_error(
      std::format("Unknown declaration qual: {}", std::to_underlying(d.qual)));
}

string
gen_rs(vector<Definition> *defs, std::set<std::string> const &recursive_types,
       std::set<std::string> const &rust_features,
       std::set<std::string> const &rust_no_features)
{
  GenRs gs(recursive_types, rust_features, rust_no_features);
  std::string preamble =
      R"(// -*-Rust-*-
// Generated Code: DO NOT CHECK IN

// Generated from xdrc commit: )" +
      x23::git_commit_hash() +
      std::format(R"(

#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(non_upper_case_globals)]
#![allow(unused_imports)]
#![allow(dead_code)]
#![allow(unused_variables)]
#![allow(clippy::unnecessary_cast)]

use crate::*;
use core::option::Option;
use core::ops::{{Deref, DerefMut}};

{0}
{1}
{2}

)",
                  gs.cfg("std", "use std::io::{Read, Write};"),
                  gs.cfg("xdrs", "use xdrs::*;"),
                  gs.cfg("spec", "use xdrs::xdr_repr::*;"));

  gs.process(*defs);

  preamble +=
      std::format(R"(
{0}
{1}
)",
                  gs.cfg("serde", "use ::serde::{{Serialize, Deserialize}};"),
                  gs.cfg("serde", "use ::serde_arrays;"));

  string output = preamble + gs.top_ + R"(
//
// XDR internals below this point
//

)" + gs.bottom_;
  while (output.ends_with("\n\n"))
    output.pop_back();
  if (output.empty() || output.back() != '\n')
    output.push_back('\n');
  return output;
}
