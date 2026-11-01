// C backend for x23c (x23c --c).
//
// Generates a header of plain C11 types plus static inline encode and decode
// functions, built on the runtime in x23/xdr_c.h.  See that header for the
// representation: variable-length opaque and string decode to views of the
// input buffer, variable-length arrays and optionals to views of their
// encoded bytes that decode elements on demand, and everything else to native
// fields.  Nothing allocates.
//
// RPC program definitions are not supported and are skipped.  The output
// is organized so that recursive types work:
//
//   1. forward declarations of every struct and union, and of the view
//      types (T_vec, T_opt, T_box) of every struct, union and enum
//   2. constants and enums in source order, then the other types, each after
//      the types it holds by value
//   3. T_XDR_SIZE macros and prototypes of every encode/decode function
//   4. function definitions
//
// A field that would make a struct contain itself by value (possible through
// union arms) is stored as a T_box view instead; see find_boxed.

#include <algorithm>
#include <cctype>
#include <format>
#include <functional>
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
  if (xdr == "quadruple")
    throw std::runtime_error("the C backend does not support quadruple");
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

string
map_bound(const string &b)
{
  return b.empty() ? "X23_UNBOUNDED" : "(" + b + ")";
}

bool
is_number(const string &v)
{
  return !v.empty() && (std::isdigit(static_cast<unsigned char>(v[0])) ||
                        v[0] == '-' || v[0] == '+');
}

// XDR enum constants are scoped to their enum (x23's C++ enums are enum
// class), and the protocol reuses names across enums, so in C each constant
// is prefixed with its enum's name.
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
  // Flattened definitions: only Typedef, EnumDecl, StructDecl, UnionDecl,
  // ConstDef and Literal, with every anonymous body hoisted out to a named
  // definition, so every Declaration's type is a type name.
  vector<Definition> defs_;
  std::map<string, const Definition *> local_; // named local types
  std::set<string> recursive_;
  // Fields that hold their type by value on a cycle of by-value fields.  C
  // cannot nest a struct in itself, so these are stored as T_box views.
  std::set<const Declaration *> boxed_;
  // The type definitions (Typedef, StructDecl, UnionDecl) in an order where
  // each follows those it holds by value.
  vector<const Definition *> type_order_;

  string fwd_;    // section 1
  string types_;  // section 2
  string protos_; // section 3
  string funcs_;  // section 4

  void flatten(const vector<Definition> &in);
  void hoist(Declaration &d, const string &name, string comment);
  void hoist_body(TypeSpecifier &ts, const string &name);
  void find_recursive();
  void find_boxed();
  void order_types();

  void gen();
  void gen_const(const ConstDef &c);
  void gen_literal(const Literal &l);
  void gen_enum(const EnumDecl &e, const string &comment);
  void gen_struct(const StructDecl &s, const string &comment);
  void gen_union(const UnionDecl &u, const string &comment);
  void gen_typedef(const Typedef &t, const string &comment);

  // Whether a discriminant type is an enum.  Types from other files are
  // assumed to be enums unless primitive, as union discriminants almost
  // always are.
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
    if (!is_number(c) && is_enum(switch_type))
      return enum_const(enum_name(switch_type), c);
    return c;
  }
  // The enum a (possibly typedef'd) enum type names, whose name prefixes
  // its constants.
  string enum_name(const string &t) const
  {
    auto it = local_.find(t);
    if (it != local_.end())
      if (auto *td = std::get_if<Typedef>(it->second))
        return enum_name(type_of(*td));
    return t;
  }

  // Common to every named type: size macro, prototypes, view functions and
  // whole-buffer helpers.
  void gen_common(const string &name, const string &size_expr);

  string field(const Declaration &d, const string &indent) const;
  string decode_stmt(const Declaration &d, const string &lv,
                     const string &indent) const;
  string encode_stmt(const Declaration &d, const string &rv,
                     const string &indent) const;
  static string size_expr(const Declaration &d);
  static string comment_block(const string &comment, const string &indent);

  string depth_enter(const string &name) const
  {
    if (!recursive_.contains(name))
      return {};
    return "  if (r->depth == 0)\n"
           "    return X23_ERR_DEPTH;\n"
           "  --r->depth;\n";
  }
  string depth_leave(const string &name) const
  {
    return recursive_.contains(name) ? "  ++r->depth;\n" : "";
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

// A struct or union is recursive if it can contain itself, which is when
// the x23 C++ deserializer charges it against the recursion limit.  Types
// from other files cannot refer back to this one, so looking only at local
// types finds every cycle.
void
GenC::find_recursive()
{
  std::map<string, std::set<string>> edges;
  const auto add = [&](const string &from, const Declaration &d) {
    if (d.qual != kVoid && local_.contains(type_of(d)))
      edges[from].insert(type_of(d));
  };
  for (const auto &[name, def] : local_) {
    if (auto *s = std::get_if<StructDecl>(def))
      for (const auto &d : s->body)
        add(name, d);
    else if (auto *u = std::get_if<UnionDecl>(def))
      for (const auto &a : u->body.arms)
        add(name, a.decl);
    else if (auto *t = std::get_if<Typedef>(def))
      add(name, *t);
  }
  for (const auto &[name, def] : local_) {
    if (!holds_alternative<StructDecl>(*def) &&
        !holds_alternative<UnionDecl>(*def))
      continue;
    std::set<string> seen;
    vector<string> stack(edges[name].begin(), edges[name].end());
    while (!stack.empty()) {
      string n = std::move(stack.back());
      stack.pop_back();
      if (n == name) {
        recursive_.insert(name);
        break;
      }
      if (seen.insert(n).second)
        for (const auto &m : edges[n])
          stack.push_back(m);
    }
  }
}

// XDR allows a struct to contain itself by value through union arms (as
// contract_spec.x's ContractType does), which the C++ and Rust bindings box
// by annotation.  Here a by-value field on a by-value cycle is boxed when it
// refers to its own type or one defined later in the source, which breaks
// every cycle while leaving the other fields on it plain.
void
GenC::find_boxed()
{
  std::map<string, std::set<string>> edges;
  const auto by_value = [&](const Declaration &d) {
    return (d.qual == kScalar || d.qual == kArray) &&
           local_.contains(type_of(d));
  };
  for (const auto &[name, def] : local_) {
    if (auto *s = std::get_if<StructDecl>(def)) {
      for (const auto &d : s->body)
        if (by_value(d))
          edges[name].insert(type_of(d));
    }
    else if (auto *u = std::get_if<UnionDecl>(def)) {
      for (const auto &a : u->body.arms)
        if (by_value(a.decl))
          edges[name].insert(type_of(a.decl));
    }
    else if (auto *t = std::get_if<Typedef>(def)) {
      if (by_value(*t))
        edges[name].insert(type_of(*t));
    }
  }
  const auto reaches = [&](const string &from, const string &to) {
    std::set<string> seen;
    vector<string> stack{from};
    while (!stack.empty()) {
      string n = std::move(stack.back());
      stack.pop_back();
      if (n == to)
        return true;
      if (seen.insert(n).second)
        for (const auto &m : edges[n])
          stack.push_back(m);
    }
    return false;
  };
  std::map<string, size_t> index;
  for (size_t i = 0; i < defs_.size(); ++i)
    std::visit(
        [&]<typename T>(const T &v) {
          if constexpr (requires { v.id; } && !std::is_same_v<T, ConstDef>)
            index[v.id] = i;
        },
        defs_[i]);
  const auto check = [&](const string &owner, const Declaration &d) {
    if (!by_value(d) || index[type_of(d)] < index[owner] ||
        !reaches(type_of(d), owner))
      return;
    if (d.qual == kArray)
      throw std::runtime_error(std::format(
          "{}.{}: the C backend cannot nest a fixed-length array of {} in "
          "itself",
          owner, d.id, type_of(d)));
    boxed_.insert(&d);
  };
  for (const auto &def : defs_) {
    if (auto *s = std::get_if<StructDecl>(&def))
      for (const auto &d : s->body)
        check(s->id, d);
    else if (auto *u = std::get_if<UnionDecl>(&def))
      for (const auto &a : u->body.arms)
        check(u->id, a.decl);
  }
}

// C needs a type defined before another holds it by value.  XDR source
// order usually already does that, but not around recursive types.
void
GenC::order_types()
{
  // Views (T_vec, T_opt, T_box) of structs, unions and enums are declared
  // up front, so only a typedef's views wait for the typedef.
  const auto deps = [&](const Definition &def) {
    vector<string> out;
    const auto add = [&](const Declaration &d) {
      if (d.qual == kVoid || !local_.contains(type_of(d)))
        return;
      const bool view = d.qual == kVec || d.qual == kPtr || d.qual == kString ||
                        boxed_.contains(&d);
      if (!view || holds_alternative<Typedef>(*local_.at(type_of(d))))
        out.push_back(type_of(d));
    };
    if (auto *s = std::get_if<StructDecl>(&def))
      for (const auto &d : s->body)
        add(d);
    else if (auto *u = std::get_if<UnionDecl>(&def)) {
      Declaration disc{.type = u->body.switch_type};
      add(disc);
      for (const auto &a : u->body.arms)
        add(a.decl);
    }
    else if (auto *t = std::get_if<Typedef>(&def))
      add(*t);
    return out;
  };
  std::set<const Definition *> done, active;
  std::function<void(const Definition *)> visit = [&](const Definition *def) {
    if (done.contains(def) || !active.insert(def).second)
      return;
    for (const auto &n : deps(*def))
      if (const Definition *d = local_.at(n); !holds_alternative<EnumDecl>(*d))
        visit(d);
    active.erase(def);
    done.insert(def);
    type_order_.push_back(def);
  };
  for (const auto &def : defs_)
    if (holds_alternative<Typedef>(def) || holds_alternative<StructDecl>(def) ||
        holds_alternative<UnionDecl>(def))
      visit(&def);
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
    if (boxed_.contains(&d))
      out += std::format("{}{}_box {};\n", indent, c_type(t).prefix, id);
    else
      out += std::format("{}{} {};\n", indent, c_type(t).ctype, id);
    break;
  case kArray:
    if (t == "opaque")
      out += std::format("{}uint8_t {}[{}];\n", indent, id, d.bound);
    else
      out +=
          std::format("{}{} {}[{}];\n", indent, c_type(t).ctype, id, d.bound);
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
    out += std::format("{}{}_opt {};\n", indent, c_type(t).prefix, id);
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
    return std::format("{}X23_TRY({}_{}decode(r, &{}));\n", in,
                       c_type(t).prefix, boxed_.contains(&d) ? "box_" : "", lv);
  case kArray:
    if (t == "opaque")
      return std::format("{0}{{\n"
                         "{0}  const uint8_t *p_;\n"
                         "{0}  X23_TRY(x23_get_opaque(r, {1}, &p_));\n"
                         "{0}  x23_copy_bytes({2}, p_, {1});\n"
                         "{0}}}\n",
                         in, d.bound, lv);
    return std::format("{0}for (uint32_t i_ = 0; i_ < ({1}); ++i_)\n"
                       "{0}  X23_TRY({2}_decode(r, &{3}[i_]));\n",
                       in, d.bound, c_type(t).prefix, lv);
  case kVec:
    if (t == "opaque")
      return std::format("{}X23_TRY(x23_bytes_decode(r, &{}, {}));\n", in, lv,
                         map_bound(d.bound));
    return std::format("{}X23_TRY({}_vec_decode(r, &{}, {}));\n", in,
                       c_type(t).prefix, lv, map_bound(d.bound));
  case kString:
    return std::format("{}X23_TRY(x23_string_decode(r, &{}, {}));\n", in, lv,
                       map_bound(d.bound));
  case kPtr:
    return std::format("{}X23_TRY({}_opt_decode(r, &{}));\n", in,
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
    return std::format("{}X23_TRY({}_{}encode(w, &{}));\n", in,
                       c_type(t).prefix, boxed_.contains(&d) ? "box_" : "", rv);
  case kArray:
    if (t == "opaque")
      return std::format("{}X23_TRY(x23_put_opaque(w, {}, {}));\n", in, rv,
                         d.bound);
    return std::format("{0}for (uint32_t i_ = 0; i_ < ({1}); ++i_)\n"
                       "{0}  X23_TRY({2}_encode(w, &{3}[i_]));\n",
                       in, d.bound, c_type(t).prefix, rv);
  case kVec:
    if (t == "opaque")
      return std::format("{}X23_TRY(x23_bytes_encode(w, &{}, {}));\n", in, rv,
                         map_bound(d.bound));
    return std::format("{}X23_TRY({}_vec_encode(w, &{}, {}));\n", in,
                       c_type(t).prefix, rv, map_bound(d.bound));
  case kString:
    return std::format("{}X23_TRY(x23_string_encode(w, &{}, {}));\n", in, rv,
                       map_bound(d.bound));
  case kPtr:
    return std::format("{}X23_TRY({}_opt_encode(w, &{}));\n", in,
                       c_type(t).prefix, rv);
  }
  return {};
}

// A C constant expression for d's encoded size, or "0" if it varies.
string
GenC::size_expr(const Declaration &d)
{
  switch (d.qual) {
  case kVoid:
    return "0";
  case kScalar:
    return c_type(type_of(d)).prefix + "_XDR_SIZE";
  case kArray:
    if (type_of(d) == "opaque")
      return std::format("((({}) + 3u) & ~3u)", d.bound);
    return std::format("({}_XDR_SIZE * ({}))", c_type(type_of(d)).prefix,
                       d.bound);
  case kVec:
  case kString:
  case kPtr:
    return "0";
  }
  return "0";
}

// The sum of sizes, or 0 if any of them is 0 (varies).
string
sum_sizes(const vector<string> &sizes)
{
  vector<string> parts;
  for (const auto &s : sizes) {
    if (s == "0")
      return "0";
    parts.push_back(s);
  }
  if (parts.empty())
    return "0";
  if (parts.size() == 1)
    return parts[0];
  string all, sum;
  for (const auto &p : parts) {
    all += (all.empty() ? "" : " && ") + p;
    sum += (sum.empty() ? "" : " + ") + p;
  }
  return std::format("(({}) ? ({}) : 0)", all, sum);
}

void
GenC::gen_common(const string &name, const string &size)
{
  protos_ += std::format(
      "#define {0}_XDR_SIZE {1}\n"
      "static inline x23_status {0}_decode(x23_reader *r, {0} *out);\n"
      "static inline x23_status {0}_encode(x23_writer *w, const {0} *v);\n"
      "X23_VIEW_PROTOS({0}, {0})\n",
      name, size);
  funcs_ += std::format("X23_VIEW_FUNCS({0}, {0})\n"
                        "X23_BUF_FUNCS({0}, {0})\n\n",
                        name);
}

void
GenC::gen_const(const ConstDef &c)
{
  string val = c.val;
  std::erase(val, '\''); // C11 has no digit separators
  types_ += comment_block(c.comment, "");
  types_ += std::format("#define {} {}\n\n", c.id, val);
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
  fwd_ += std::format("typedef int32_t {0};\nX23_VIEW_TYPES({0}, {0})\n", e.id);

  types_ += comment_block(comment, "");
  types_ += "enum {\n";
  for (size_t i = 0; i < e.body.size(); ++i) {
    string val = e.body[i].val;
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
  protos_ += std::format("static inline bool {}_valid(int32_t v);\n", e.id);
  funcs_ += std::format(R"(static inline bool
{0}_valid(int32_t v)
{{
  return {1};
}}
static inline x23_status
{0}_decode(x23_reader *r, {0} *out)
{{
  X23_TRY(x23_i32_decode(r, out));
  return {0}_valid(*out) ? X23_OK : X23_ERR_ENUM;
}}
static inline x23_status
{0}_encode(x23_writer *w, const {0} *v)
{{
  if (!{0}_valid(*v))
    return X23_ERR_ENUM;
  return x23_i32_encode(w, v);
}}
)",
                        e.id, valid);
  gen_common(e.id, "4");
}

void
GenC::gen_struct(const StructDecl &s, const string &comment)
{
  fwd_ +=
      std::format("typedef struct {0} {0};\nX23_VIEW_TYPES({0}, {0})\n", s.id);

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

  string dec, enc;
  vector<string> sizes;
  for (const auto &d : s.body) {
    if (d.qual == kVoid)
      continue;
    dec += decode_stmt(d, "out->" + c_id(d.id), "  ");
    enc += encode_stmt(d, "v->" + c_id(d.id), "  ");
    sizes.push_back(size_expr(d));
  }
  if (dec.empty()) {
    dec = "  (void)r;\n  (void)out;\n";
    enc = "  (void)w;\n  (void)v;\n";
  }
  funcs_ += std::format(R"(static inline x23_status
{0}_decode(x23_reader *r, {0} *out)
{{
{1}{2}{3}  return X23_OK;
}}
static inline x23_status
{0}_encode(x23_writer *w, const {0} *v)
{{
{4}  return X23_OK;
}}
)",
                        s.id, depth_enter(s.id), dec, depth_leave(s.id), enc);
  // A struct with no fields encodes to nothing, which "0" (varies) still
  // handles correctly, if more slowly.
  gen_common(s.id, sum_sizes(sizes));
}

void
GenC::gen_union(const UnionDecl &u, const string &comment)
{
  const UnionBody &b = u.body;
  fwd_ +=
      std::format("typedef struct {0} {0};\nX23_VIEW_TYPES({0}, {0})\n", u.id);
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

  string dec, enc;
  bool has_default = false;
  vector<string> arm_sizes;
  bool any_void = false, any_value = false;
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
    dec += decode_stmt(a.decl, "out->" + c_id(a.decl.id), "    ");
    enc += encode_stmt(a.decl, "v->" + c_id(a.decl.id), "    ");
    dec += "    break;\n";
    enc += "    break;\n";
    if (a.decl.qual == kVoid)
      any_void = true;
    else {
      any_value = true;
      arm_sizes.push_back(size_expr(a.decl));
    }
  }
  if (!has_default) {
    dec += "  default:\n    return X23_ERR_UNION;\n";
    enc += "  default:\n    return X23_ERR_UNION;\n";
  }
  const string sw = b.switch_type == "bool" ? "(int)" : "";
  funcs_ += std::format(R"(static inline x23_status
{0}_decode(x23_reader *r, {0} *out)
{{
{1}  X23_TRY({2}_decode(r, &out->{3}));
  switch ({7}out->{3}) {{
{4}  }}
{5}  return X23_OK;
}}
static inline x23_status
{0}_encode(x23_writer *w, const {0} *v)
{{
  X23_TRY({2}_encode(w, &v->{3}));
  switch ({7}v->{3}) {{
{6}  }}
  return X23_OK;
}}
)",
                        u.id, depth_enter(u.id), disc.prefix, disc_id, dec,
                        depth_leave(u.id), enc, sw);

  // Fixed size only when every arm has the same fixed size.
  string size = "0";
  if (!any_value)
    size = disc.prefix + "_XDR_SIZE";
  else if (!any_void &&
           std::ranges::none_of(arm_sizes, [](auto &s) { return s == "0"; })) {
    string cond;
    for (const auto &s : arm_sizes)
      cond += std::format("{}({}) == ({})", cond.empty() ? "" : " && ", s,
                          arm_sizes[0]);
    size = std::format("(({}) ? {}_XDR_SIZE + ({}) : 0)", cond, disc.prefix,
                       arm_sizes[0]);
  }
  gen_common(u.id, size);
}

void
GenC::gen_typedef(const Typedef &t, const string &comment)
{
  const string &n = t.id;
  const string base = type_of(t);
  types_ += comment_block(comment, "");
  string dec, enc;
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
                      t.bound, n);
    else
      types_ += std::format("typedef struct {} {{\n  {} elems[{}];\n}} {};\n",
                            n, c_type(base).ctype, t.bound, n);
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
    types_ += std::format("typedef {}_opt {};\n", c_type(base).prefix, n);
    break;
  }
  types_ += std::format("X23_VIEW_TYPES({0}, {0})\n\n", n);

  if (t.qual == kArray) {
    Declaration d = t;
    d.id = base == "opaque" ? "bytes" : "elems";
    dec = decode_stmt(d, "out->" + d.id, "  ");
    enc = encode_stmt(d, "v->" + d.id, "  ");
  }
  else {
    // The typedef is the same C type as the declaration, so decode it as a
    // field would be.
    dec = decode_stmt(t, "*out", "  ");
    enc = encode_stmt(t, "*v", "  ");
    // "&*out" is just out.
    for (string *s : {&dec, &enc}) {
      for (auto [from, to] : {std::pair{"&*out", "out"}, {"&*v", "v"}})
        for (size_t p; (p = s->find(from)) != string::npos;)
          s->replace(p, std::string_view(from).size(), to);
    }
  }
  funcs_ += std::format(R"(static inline x23_status
{0}_decode(x23_reader *r, {0} *out)
{{
{1}  return X23_OK;
}}
static inline x23_status
{0}_encode(x23_writer *w, const {0} *v)
{{
{2}  return X23_OK;
}}
)",
                        n, dec, enc);
  gen_common(n, size_expr(t));
}

void
GenC::gen()
{
  find_recursive();
  find_boxed();
  order_types();
  // Constants and enums depend on nothing but other constants, so they keep
  // their source order and go first.
  for (const auto &def : defs_) {
    if (auto *c = std::get_if<ConstDef>(&def))
      gen_const(*c);
    else if (auto *l = std::get_if<Literal>(&def))
      gen_literal(*l);
    else if (auto *e = std::get_if<EnumDecl>(&def))
      gen_enum(*e, def.comment);
  }
  for (const Definition *def : type_order_) {
    if (auto *s = std::get_if<StructDecl>(def))
      gen_struct(*s, def->comment);
    else if (auto *u = std::get_if<UnionDecl>(def))
      gen_union(*u, def->comment);
    else if (auto *t = std::get_if<Typedef>(def))
      gen_typedef(*t, def->comment);
  }
}

} // namespace

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
    out += "/* Encoded sizes (0 if variable) and prototypes */\n\n" +
           g.protos_ + '\n';
  out += g.funcs_;
  out += "X23_C_LINKAGE_END\n";
  return out;
}
