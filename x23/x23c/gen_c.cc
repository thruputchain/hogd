// C backend for x23c (x23c --c): C11 types plus codecs over x23/xdr_c.h, one
// set per format.  RPC programs are skipped.

#include <algorithm>
#include <cctype>
#include <format>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include "x23c/defs.h"

namespace {

using enum Declaration::Qual;

// The C value type and function-name prefix of an XDR type name.
struct CType {
  string ctype;
  string prefix;
};

CType
c_type(const string &xdr)
{
  static const std::map<string, CType> prims{
      {"int", {"int32_t", "x23_i32"}},
      {"unsigned int", {"uint32_t", "x23_u32"}},
      {"hyper", {"int64_t", "x23_i64"}},
      {"unsigned hyper", {"uint64_t", "x23_u64"}},
      {"bool", {"bool", "x23_bool"}},
      {"float", {"float", "x23_f32"}},
      {"double", {"double", "x23_f64"}},
  };
  // No C type is a 128-bit float everywhere, so quadruple, like a type x23c
  // has not seen, is left for the includer to define along with its codecs
  // (quadruple_decode_xdr and so on) and quadruple_free.
  if (auto it = prims.find(xdr); it != prims.end())
    return it->second;
  return {xdr, xdr};
}

// C keywords (C11 and C23) and a few reserved words that XDR does not
// itself reserve, so could appear as field names.
string
c_id(const string &id)
{
  static const std::set<string> kw{
      "alignas",
      "alignof",
      "auto",
      "break",
      "char",
      "constexpr",
      "continue",
      "do",
      "else",
      "extern",
      "for",
      "goto",
      "if",
      "inline",
      "long",
      "nullptr",
      "register",
      "restrict",
      "return",
      "short",
      "signed",
      "sizeof",
      "static",
      "static_assert",
      "thread_local",
      "true",
      "false",
      "typeof",
      "volatile",
      "while",
      "_Alignas",
      "_Alignof",
      "_Atomic",
      "_BitInt",
      "_Bool",
      "_Complex",
      "_Generic",
      "_Imaginary",
      "_Noreturn",
      "_Static_assert",
      "_Thread_local",
      "class",
      "delete",
      "new",
      "this",
      "template",
      "private",
      "public",
      "protected",
      "virtual",
      "operator",
      "friend",
      "namespace",
      "using",
      "typename",
      "explicit",
      "mutable",
      "export",
      "try",
      "catch",
      "throw",
      "asm",
  };
  return kw.contains(id) ? id + "_" : id;
}

// C11 has no digit separators.
string
c_num(string v)
{
  std::erase(v, '\'');
  return v;
}

string
map_bound(const string &b)
{
  return b.empty() ? "X23_UNBOUNDED" : "(" + c_num(b) + ")";
}

bool
is_number(const string &v)
{
  return !v.empty() && (std::isdigit(static_cast<unsigned char>(v[0])) ||
                        v[0] == '-' || v[0] == '+');
}

// XDR scopes enum constants and the protocol reuses names, so C prefixes each
// with its enum's name.
string
enum_const(const string &enum_name, const string &c)
{
  return enum_name + "_" + c;
}

string
type_of(const Declaration &d)
{
  return get<string>(d.type);
}

struct GenC {
  // Flattened definitions: anonymous bodies hoisted out, so every
  // Declaration's type is a type name.
  vector<Definition> defs_;
  std::map<string, const Definition *> local_; // named local types

  string fwd_;    // section 1
  string types_;  // section 2
  string protos_; // section 3
  string funcs_;  // section 4

  void flatten(const vector<Definition> &in);
  void hoist(Declaration &d, const string &name, string comment);
  void hoist_body(TypeSpecifier &ts, const string &name);

  void gen();
  void gen_const(const ConstDef &c);
  void gen_literal(const Literal &l);
  void gen_enum(const EnumDecl &e, const string &comment);
  void gen_struct(const StructDecl &s, const string &comment);
  void gen_union(const UnionDecl &u, const string &comment);
  void gen_typedef(const Typedef &t, const string &comment);

  // Non-primitive discriminant types from other files are assumed to be enums.
  bool is_enum(const string &t) const
  {
    if (t == "bool" || t == "int" || t == "unsigned int" || t == "hyper" ||
        t == "unsigned hyper")
      return false;
    auto it = local_.find(t);
    if (it == local_.end())
      return true;
    if (auto *td = std::get_if<Typedef>(it->second))
      return td->qual == kScalar && is_enum(type_of(*td));
    return holds_alternative<EnumDecl>(*it->second);
  }
  // A union case label.  A bool discriminant is switched on as an int,
  // since compilers warn about switching on a bool.
  string map_case(const string &switch_type, const string &c) const
  {
    if (switch_type == "bool")
      return c == "TRUE" ? "1" : c == "FALSE" ? "0" : c;
    // The prefix is the discriminant's type name as written.  A typedef is
    // not followed to its enum, since it may come from another file.
    if (!is_number(c) && is_enum(switch_type))
      return enum_const(switch_type, c);
    return c_num(c);
  }

  // Common to every named type: prototypes, vector and pointer codecs and
  // whole-buffer helpers.
  void gen_common(const string &name);

  // Codecs exist once per format.  Their text is written with @F where the
  // format's name goes, and emitted for each.
  static string per_format(const string &text)
  {
    string out;
    for (std::string_view fmt : {"xdr", "cxs"}) {
      string t = text;
      for (size_t p = 0; (p = t.find("@F", p)) != string::npos;)
        t.replace(p, 2, fmt);
      out += t;
    }
    return out;
  }

  string field(const Declaration &d, const string &indent) const;
  string decode_stmt(const Declaration &d, const string &lv,
                     const string &indent) const;
  string encode_stmt(const Declaration &d, const string &rv,
                     const string &indent) const;
  string free_stmt(const Declaration &d, const string &lv,
                   const string &indent) const;
  static string comment_block(const string &comment, const string &indent);

  // #[cpp(box)] fields are held as T *, matching gen_hh so C and C++ box the
  // same fields.
  static bool boxed(const Declaration &d)
  {
    return d.qual == kScalar && d.comment.find("#[cpp(box)]") != string::npos;
  }
};

void
GenC::hoist_body(TypeSpecifier &ts, const string &name)
{
  if (auto *s = std::get_if<StructBody>(&ts))
    for (auto &d : *s)
      hoist(d, name + "_" + d.id, {});
  else if (auto *u = std::get_if<UnionBody>(&ts))
    for (auto &a : u->arms)
      if (a.decl.qual != kVoid)
        hoist(a.decl, name + "_" + a.decl.id, {});
}

// Replace an anonymous body in d by a named definition appended to defs_.
void
GenC::hoist(Declaration &d, const string &name, string comment)
{
  if (d.qual == kVoid || holds_alternative<string>(d.type))
    return;
  hoist_body(d.type, name);
  Definition def;
  if (auto *e = std::get_if<EnumBody>(&d.type))
    def = EnumDecl{.id = name, .body = *e};
  else if (auto *s = std::get_if<StructBody>(&d.type))
    def = StructDecl{.id = name, .body = *s};
  else
    def = UnionDecl{.id = name, .body = get<UnionBody>(d.type)};
  def.comment = std::move(comment);
  defs_.push_back(std::move(def));
  d.type = name;
}

void
GenC::flatten(const vector<Definition> &in)
{
  for (const auto &def : in) {
    std::visit(
        [&]<typename T>(const T &v) {
          if constexpr (std::is_same_v<T, Typedef>) {
            Typedef t = v;
            if (t.qual == kScalar && !holds_alternative<string>(t.type)) {
              // typedef struct {...} Name;  is struct Name {...};
              hoist(t, t.id, def.comment);
              return;
            }
            hoist(t, t.id + "_elem", {});
            Definition nd = t;
            nd.comment = def.comment;
            defs_.push_back(std::move(nd));
          }
          else if constexpr (std::is_same_v<T, StructDecl>) {
            StructDecl s = v;
            for (auto &d : s.body)
              hoist(d, s.id + "_" + d.id, {});
            Definition nd = std::move(s);
            nd.comment = def.comment;
            defs_.push_back(std::move(nd));
          }
          else if constexpr (std::is_same_v<T, UnionDecl>) {
            UnionDecl u = v;
            for (auto &a : u.body.arms)
              if (a.decl.qual != kVoid)
                hoist(a.decl, u.id + "_" + a.decl.id, {});
            Definition nd = std::move(u);
            nd.comment = def.comment;
            defs_.push_back(std::move(nd));
          }
          else if constexpr (std::is_same_v<T, EnumDecl> ||
                             std::is_same_v<T, ConstDef> ||
                             std::is_same_v<T, Literal>) {
            defs_.push_back(def);
          }
          // OpenNS, CloseNS: C has no namespaces.
          // ProgDef: RPC programs are not supported.
        },
        def);
  }
  for (const auto &def : defs_) {
    std::visit(
        [&]<typename T>(const T &v) {
          if constexpr (!std::is_same_v<T, ConstDef> &&
                        !std::is_same_v<T, Literal>)
            local_[v.id] = &def;
        },
        def);
  }
}

string
GenC::comment_block(const string &comment, const string &indent)
{
  if (comment.empty())
    return {};
  // Re-indent each line, keeping indentation relative to the first.
  vector<string> lines;
  for (size_t pos = 0; pos < comment.size();) {
    size_t nl = comment.find('\n', pos);
    if (nl == string::npos)
      nl = comment.size();
    lines.push_back(comment.substr(pos, nl - pos));
    pos = nl + 1;
  }
  size_t strip = string::npos;
  for (const auto &l : lines)
    if (auto p = l.find_first_not_of(" \t"); p != string::npos)
      strip = std::min(strip, p);
  string out;
  for (const auto &l : lines) {
    if (l.find_first_not_of(" \t") == string::npos)
      out += "\n";
    else
      out += indent + l.substr(strip) + "\n";
  }
  return out;
}

string
GenC::field(const Declaration &d, const string &indent) const
{
  string out = comment_block(d.comment, indent);
  const string id = c_id(d.id);
  const string t = type_of(d);
  switch (d.qual) {
  case kVoid:
    return {};
  case kScalar:
    if (boxed(d))
      out += std::format("{}{} *{};\n", indent, c_type(t).ctype, id);
    else
      out += std::format("{}{} {};\n", indent, c_type(t).ctype, id);
    break;
  case kArray:
    if (t == "opaque")
      out += std::format("{}uint8_t {}[{}];\n", indent, id, c_num(d.bound));
    else
      out += std::format("{}{} {}[{}];\n", indent, c_type(t).ctype, id,
                         c_num(d.bound));
    break;
  case kVec:
    if (t == "opaque")
      out += std::format("{}x23_bytes {};\n", indent, id);
    else
      out += std::format("{}{}_vec {};\n", indent, c_type(t).prefix, id);
    break;
  case kString:
    out += std::format("{}x23_string {};\n", indent, id);
    break;
  case kPtr:
    out += std::format("{}{} *{};\n", indent, c_type(t).ctype, id);
    break;
  }
  return out;
}

// Decode d into the lvalue lv.
string
GenC::decode_stmt(const Declaration &d, const string &lv,
                  const string &in) const
{
  const string t = d.qual == kVoid ? string{} : type_of(d);
  switch (d.qual) {
  case kVoid:
    return {};
  case kScalar:
    return std::format("{}X23_TRY({}_{}decode_@F(r, &{}));\n", in,
                       c_type(t).prefix, boxed(d) ? "box_" : "", lv);
  case kArray:
    if (t == "opaque")
      return std::format("{}X23_TRY(x23_get_opaque_@F(r, {}, {}));\n", in,
                         c_num(d.bound), lv);
    return std::format("{0}for (uint32_t i_ = 0; i_ < ({1}); ++i_)\n"
                       "{0}  X23_TRY({2}_decode_@F(r, &{3}[i_]));\n",
                       in, c_num(d.bound), c_type(t).prefix, lv);
  case kVec:
    if (t == "opaque")
      return std::format("{}X23_TRY(x23_bytes_decode_@F(r, &{}, {}));\n", in,
                         lv, map_bound(d.bound));
    return std::format("{}X23_TRY({}_vec_decode_@F(r, &{}, {}));\n", in,
                       c_type(t).prefix, lv, map_bound(d.bound));
  case kString:
    return std::format("{}X23_TRY(x23_string_decode_@F(r, &{}, {}));\n", in, lv,
                       map_bound(d.bound));
  case kPtr:
    return std::format("{}X23_TRY({}_ptr_decode_@F(r, &{}));\n", in,
                       c_type(t).prefix, lv);
  }
  return {};
}

// Encode the lvalue rv, declared as d.
string
GenC::encode_stmt(const Declaration &d, const string &rv,
                  const string &in) const
{
  const string t = d.qual == kVoid ? string{} : type_of(d);
  switch (d.qual) {
  case kVoid:
    return {};
  case kScalar:
    if (boxed(d))
      return std::format("{}X23_TRY({}_box_encode_@F(w, {}));\n", in,
                         c_type(t).prefix, rv);
    return std::format("{}X23_TRY({}_encode_@F(w, &{}));\n", in,
                       c_type(t).prefix, rv);
  case kArray:
    if (t == "opaque")
      return std::format("{}X23_TRY(x23_put_opaque_@F(w, {}, {}));\n", in, rv,
                         c_num(d.bound));
    return std::format("{0}for (uint32_t i_ = 0; i_ < ({1}); ++i_)\n"
                       "{0}  X23_TRY({2}_encode_@F(w, &{3}[i_]));\n",
                       in, c_num(d.bound), c_type(t).prefix, rv);
  case kVec:
    if (t == "opaque")
      return std::format("{}X23_TRY(x23_bytes_encode_@F(w, &{}, {}));\n", in,
                         rv, map_bound(d.bound));
    return std::format("{}X23_TRY({}_vec_encode_@F(w, &{}, {}));\n", in,
                       c_type(t).prefix, rv, map_bound(d.bound));
  case kString:
    return std::format("{}X23_TRY(x23_string_encode_@F(w, &{}, {}));\n", in, rv,
                       map_bound(d.bound));
  case kPtr:
    return std::format("{}X23_TRY({}_ptr_encode_@F(w, {}));\n", in,
                       c_type(t).prefix, rv);
  }
  return {};
}

// Release what the lvalue lv, declared as d, owns.
string
GenC::free_stmt(const Declaration &d, const string &lv, const string &in) const
{
  const string t = d.qual == kVoid ? string{} : type_of(d);
  switch (d.qual) {
  case kVoid:
    return {};
  case kScalar:
    return std::format("{}{}_{}free(&{});\n", in, c_type(t).prefix,
                       boxed(d) ? "ptr_" : "", lv);
  case kArray:
    if (t == "opaque")
      return {};
    return std::format("{0}for (uint32_t i_ = 0; i_ < ({1}); ++i_)\n"
                       "{0}  {2}_free(&{3}[i_]);\n",
                       in, c_num(d.bound), c_type(t).prefix, lv);
  case kVec:
    if (t == "opaque")
      return std::format("{}x23_bytes_free(&{});\n", in, lv);
    return std::format("{}{}_vec_free(&{});\n", in, c_type(t).prefix, lv);
  case kString:
    return std::format("{}x23_string_free(&{});\n", in, lv);
  case kPtr:
    return std::format("{}{}_ptr_free(&{});\n", in, c_type(t).prefix, lv);
  }
  return {};
}

void
GenC::gen_common(const string &name)
{
  protos_ += per_format(std::format(
      "X23_FN x23_status {0}_decode_@F(x23_reader *r, {0} *out);\n"
      "X23_FN x23_status {0}_encode_@F(x23_writer *w, const {0} *v);\n",
      name));
  protos_ += std::format("X23_FN void {0}_free({0} *v);\n"
                         "X23_PTR_PROTOS({0}, {0})\n",
                         name);
  funcs_ += std::format("X23_PTR_FUNCS({0}, {0})\n"
                        "X23_BUF_FUNCS({0}, {0})\n\n",
                        name);
}

void
GenC::gen_const(const ConstDef &c)
{
  types_ += comment_block(c.comment, "");
  types_ += std::format("#define {} {}\n\n", c.id, c_num(c.val));
}

// The only literal C++ that carries over is including the header generated
// from another .x file.
void
GenC::gen_literal(const Literal &l)
{
  static const std::regex inc(R"(^\s*#\s*include\s*"(.*)\.hh\"\s*$)");
  size_t pos = 0;
  while (pos < l.lines.size()) {
    size_t nl = l.lines.find('\n', pos);
    if (nl == string::npos)
      nl = l.lines.size();
    string line = l.lines.substr(pos, nl - pos);
    std::smatch m;
    if (std::regex_match(line, m, inc))
      types_ += std::format("#include \"{}.h\"\n", m[1].str());
    pos = nl + 1;
  }
}

void
GenC::gen_enum(const EnumDecl &e, const string &comment)
{
  fwd_ += std::format("typedef int32_t {0};\nX23_VEC_TYPE({0}, {0})\n", e.id);

  types_ += comment_block(comment, "");
  types_ += "enum {\n";
  for (size_t i = 0; i < e.body.size(); ++i) {
    string val = c_num(e.body[i].val);
    // A value may name an earlier constant of the same enum.
    if (std::ranges::any_of(e.body, [&](auto &c) { return c.id == val; }))
      val = enum_const(e.id, val);
    types_ += comment_block(e.body[i].comment, "  ");
    types_ += std::format("  {} = {}{}\n", enum_const(e.id, e.body[i].id), val,
                          i + 1 < e.body.size() ? "," : "");
  }
  types_ += "};\n\n";

  string valid;
  for (const auto &c : e.body)
    valid += std::format("{}v == {}", valid.empty() ? "" : " ||\n         ",
                         enum_const(e.id, c.id));
  protos_ += std::format("X23_FN bool {}_valid(int32_t v);\n", e.id);
  // Like the C++ and Rust bindings, decoding accepts any value; _valid is
  // for callers that care.
  funcs_ += std::format(R"(X23_FN bool
{0}_valid(int32_t v)
{{
  return {1};
}}
)",
                        e.id, valid);
  funcs_ += per_format(std::format(R"(X23_FN x23_status
{0}_decode_@F(x23_reader *r, {0} *out)
{{
  return x23_i32_decode_@F(r, out);
}}
X23_FN x23_status
{0}_encode_@F(x23_writer *w, const {0} *v)
{{
  return x23_i32_encode_@F(w, v);
}}
)",
                                   e.id));
  funcs_ += std::format(R"(X23_FN void
{0}_free({0} *v)
{{
  (void)v;
}}
)",
                        e.id);
  gen_common(e.id);
}

void
GenC::gen_struct(const StructDecl &s, const string &comment)
{
  fwd_ +=
      std::format("typedef struct {0} {0};\nX23_VEC_TYPE({0}, {0})\n", s.id);

  types_ += comment_block(comment, "");
  types_ += std::format("struct {} {{\n", s.id);
  bool any = false;
  for (const auto &d : s.body) {
    types_ += field(d, "  ");
    any = any || d.qual != kVoid;
  }
  if (!any)
    types_ += "  char unused_; /* C forbids empty structs */\n";
  types_ += "};\n\n";

  string dec, enc, fr;
  for (const auto &d : s.body) {
    if (d.qual == kVoid)
      continue;
    dec += decode_stmt(d, "out->" + c_id(d.id), "  ");
    enc += encode_stmt(d, "v->" + c_id(d.id), "  ");
    fr += free_stmt(d, "v->" + c_id(d.id), "  ");
  }
  if (dec.empty()) {
    dec = "  (void)r;\n";
    enc = "  (void)w;\n  (void)v;\n";
  }
  if (fr.empty())
    fr = "  (void)v;\n";
  // Zeroing first leaves a partly decoded value safe to free.
  funcs_ += per_format(std::format(R"(X23_FN x23_status
{0}_decode_@F(x23_reader *r, {0} *out)
{{
  x23_zero(out, sizeof(*out));
{1}  return X23_OK;
}}
X23_FN x23_status
{0}_encode_@F(x23_writer *w, const {0} *v)
{{
{2}  return X23_OK;
}}
)",
                                   s.id, dec, enc));
  funcs_ += std::format(R"(X23_FN void
{0}_free({0} *v)
{{
{1}}}
)",
                        s.id, fr);
  gen_common(s.id);
}

void
GenC::gen_union(const UnionDecl &u, const string &comment)
{
  const UnionBody &b = u.body;
  fwd_ +=
      std::format("typedef struct {0} {0};\nX23_VEC_TYPE({0}, {0})\n", u.id);
  const CType disc = c_type(b.switch_type);
  const string disc_id = c_id(b.switch_id);

  types_ += comment_block(comment, "");
  types_ += std::format("struct {} {{\n  {} {};\n", u.id, disc.ctype, disc_id);
  string arms;
  for (const auto &a : b.arms) {
    if (a.decl.qual == kVoid)
      continue;
    string cases;
    for (const auto &c : a.cases)
      cases += (cases.empty() ? "" : ", ") + c;
    if (a.has_default)
      cases += cases.empty() ? "default" : ", default";
    arms += std::format("    /* {} */\n", cases);
    arms += field(a.decl, "    ");
  }
  if (!arms.empty())
    types_ += "  union {\n" + arms + "  };\n";
  types_ += "};\n\n";

  string dec, enc, fr;
  bool has_default = false;
  for (const auto &a : b.arms) {
    string labels;
    for (const auto &c : a.cases)
      labels += std::format("  case {}:\n", map_case(b.switch_type, c));
    if (a.has_default) {
      labels += "  default:\n";
      has_default = true;
    }
    dec += labels;
    enc += labels;
    fr += labels;
    dec += decode_stmt(a.decl, "out->" + c_id(a.decl.id), "    ");
    enc += encode_stmt(a.decl, "v->" + c_id(a.decl.id), "    ");
    fr += free_stmt(a.decl, "v->" + c_id(a.decl.id), "    ");
    dec += "    break;\n";
    enc += "    break;\n";
    fr += "    break;\n";
  }
  if (!has_default) {
    dec += "  default:\n    return X23_ERR_UNION;\n";
    enc += "  default:\n    return X23_ERR_UNION;\n";
    fr += "  default:\n    break;\n";
  }
  const string sw = b.switch_type == "bool" ? "(int)" : "";
  funcs_ += per_format(std::format(R"(X23_FN x23_status
{0}_decode_@F(x23_reader *r, {0} *out)
{{
  x23_zero(out, sizeof(*out));
  X23_TRY({1}_decode_@F(r, &out->{2}));
  switch ({5}out->{2}) {{
{3}  }}
  return X23_OK;
}}
X23_FN x23_status
{0}_encode_@F(x23_writer *w, const {0} *v)
{{
  X23_TRY({1}_encode_@F(w, &v->{2}));
  switch ({5}v->{2}) {{
{4}  }}
  return X23_OK;
}}
)",
                                   u.id, disc.prefix, disc_id, dec, enc, sw));
  funcs_ += std::format(R"(X23_FN void
{0}_free({0} *v)
{{
  switch ({3}v->{1}) {{
{2}  }}
}}
)",
                        u.id, disc_id, fr, sw);

  gen_common(u.id);
}

void
GenC::gen_typedef(const Typedef &t, const string &comment)
{
  const string &n = t.id;
  const string base = type_of(t);
  types_ += comment_block(comment, "");
  string dec, enc, fr;
  switch (t.qual) {
  case kVoid:
    throw std::runtime_error("void typedef " + n);
  case kScalar:
    types_ += std::format("typedef {} {};\n", c_type(base).ctype, n);
    break;
  case kArray:
    // Wrapped in a struct, since C arrays cannot be assigned or returned.
    if (base == "opaque")
      types_ +=
          std::format("typedef struct {} {{\n  uint8_t bytes[{}];\n}} {};\n", n,
                      c_num(t.bound), n);
    else
      types_ += std::format("typedef struct {} {{\n  {} elems[{}];\n}} {};\n",
                            n, c_type(base).ctype, c_num(t.bound), n);
    break;
  case kVec:
    if (base == "opaque")
      types_ += std::format("typedef x23_bytes {};\n", n);
    else
      types_ += std::format("typedef {}_vec {};\n", c_type(base).prefix, n);
    break;
  case kString:
    types_ += std::format("typedef x23_string {};\n", n);
    break;
  case kPtr:
    types_ += std::format("typedef {} *{};\n", c_type(base).ctype, n);
    break;
  }
  types_ += std::format("X23_VEC_TYPE({0}, {0})\n\n", n);

  if (t.qual == kArray) {
    Declaration d = t;
    d.id = base == "opaque" ? "bytes" : "elems";
    dec = "  x23_zero(out, sizeof(*out));\n" +
          decode_stmt(d, "out->" + d.id, "  ");
    enc = encode_stmt(d, "v->" + d.id, "  ");
    fr = free_stmt(d, "v->" + d.id, "  ");
  }
  else {
    // The typedef is the same C type as the declaration, so decode it as a
    // field would be.
    dec = decode_stmt(t, "*out", "  ");
    enc = encode_stmt(t, "*v", "  ");
    fr = free_stmt(t, "*v", "  ");
    // "&*out" is just out.
    for (string *s : {&dec, &enc, &fr}) {
      for (auto [from, to] : {std::pair{"&*out", "out"}, {"&*v", "v"}})
        for (size_t p; (p = s->find(from)) != string::npos;)
          s->replace(p, std::string_view(from).size(), to);
    }
  }
  funcs_ += per_format(std::format(R"(X23_FN x23_status
{0}_decode_@F(x23_reader *r, {0} *out)
{{
{1}  return X23_OK;
}}
X23_FN x23_status
{0}_encode_@F(x23_writer *w, const {0} *v)
{{
{2}  return X23_OK;
}}
)",
                                   n, dec, enc));
  funcs_ += std::format(R"(X23_FN void
{0}_free({0} *v)
{{
{1}}}
)",
                        n, fr.empty() ? "  (void)v;\n" : fr);
  gen_common(n);
}

void
GenC::gen()
{
  for (const auto &def : defs_) {
    if (auto *c = std::get_if<ConstDef>(&def))
      gen_const(*c);
    else if (auto *l = std::get_if<Literal>(&def))
      gen_literal(*l);
    else if (auto *e = std::get_if<EnumDecl>(&def))
      gen_enum(*e, def.comment);
    else if (auto *s = std::get_if<StructDecl>(&def))
      gen_struct(*s, def.comment);
    else if (auto *u = std::get_if<UnionDecl>(&def))
      gen_union(*u, def.comment);
    else if (auto *t = std::get_if<Typedef>(&def))
      gen_typedef(*t, def.comment);
  }
}

} // namespace

// Forward declarations come first and prototypes before definitions, so
// recursive types work.
string
gen_c(vector<Definition> *defs, string const &header)
{
  GenC g;
  g.flatten(*defs);
  g.gen();

  string out = "/* Generated by x23c --c.  Do not edit. */\n\n";
  if (!header.empty())
    out += header + '\n';
  out += "#pragma once\n\n#include <x23/xdr_c.h>\n\n";
  // Includes of other generated headers come from literals, which land in
  // types_ in source order; hoist them above everything else.
  string includes, types;
  for (size_t pos = 0; pos < g.types_.size();) {
    size_t nl = g.types_.find('\n', pos);
    string line = g.types_.substr(pos, nl - pos + 1);
    (line.starts_with("#include ") ? includes : types) += line;
    pos = nl + 1;
  }
  if (!includes.empty())
    out += includes + '\n';
  out += "X23_C_LINKAGE_BEGIN\n\n";
  if (!g.fwd_.empty())
    out += "/* Forward declarations */\n\n" + g.fwd_ + '\n';
  out += types;
  if (!g.protos_.empty())
    out += "/* Prototypes */\n\n" + g.protos_ + '\n';
  out += g.funcs_;
  out += "X23_C_LINKAGE_END\n";
  return out;
}
