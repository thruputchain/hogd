// -*-C++-*-

#pragma once

#include <concepts>
#include <format>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "x23/config.h"
#include "x23/utility.h"

using std::string;
using std::variant;
using std::vector;

struct Terminal {
  string text;
  string block_comment;
  string eol_comment;
};

struct Declaration;

struct ConstDef {
  string id;
  string val;
  string comment;
};

struct EnumBody : vector<ConstDef> {};

struct StructBody : vector<Declaration> {
  bool transparent = false;
};

struct UnionArm;
struct UnionBody {
  string switch_type; // Discriminant type
  string switch_id;   // Discriminant id
  vector<UnionArm> arms;
};

struct TypeSpecifier : variant<string, EnumBody, StructBody, UnionBody> {
  using super = variant<string, EnumBody, StructBody, UnionBody>;
  using super::super;
  string comment;
};

struct Declaration {
  enum Qual { kVoid, kScalar, kPtr, kArray, kVec };

  TypeSpecifier type;
  string id;
  Qual qual = kScalar;
  string bound;
  string comment;
  string attrs;
  bool is_private;
};

struct UnionArm {
  bool has_default = false;
  vector<string> cases;
  Declaration decl;
};

struct Typedef : Declaration {
  Typedef() noexcept = default;
  explicit Typedef(const Declaration &d) : Declaration(d) {}
  explicit Typedef(Declaration &&d) : Declaration(std::move(d)) {}
};

struct EnumDecl {
  string id;
  EnumBody body;
};

struct StructDecl {
  string id;
  StructBody body;
};

struct UnionDecl {
  string id;
  UnionBody body;
};

struct OpenNS {
  string id;
};

struct CloseNS {
  string id;
};

struct ProcDef {
  string id;
  string val;
  string return_type;
  vector<string> arg_types;
  string comment;
};

struct VersDef {
  string id;
  string val;
  vector<ProcDef> procs;
};

struct ProgDef {
  string id;
  string val;
  vector<VersDef> versions;
};

struct Literal {
  string lines;
};

struct Definition : variant<Typedef, EnumDecl, StructDecl, UnionDecl, ConstDef,
                            OpenNS, CloseNS, ProgDef, Literal> {
  using super = variant<Typedef, EnumDecl, StructDecl, UnionDecl, ConstDef,
                        OpenNS, CloseNS, ProgDef, Literal>;
  using super::super;
  string comment;
};

namespace detail {

template<typename T, typename V> struct VariantView {
  using variant_type = V;
  using element_type = T;

  V &v_;

  VariantView(variant_type &v) : v_(v) {}

  element_type &operator=(const element_type &e) const
  {
    return v_.template emplace<element_type>(e);
  }
  element_type &operator=(element_type &&e) const
  {
    return v_.template emplace<element_type>(std::move(e));
  }
  element_type &operator=(std::convertible_to<element_type> auto &&t) const
  {
    return v_.template emplace<element_type>(FORWARD(t));
  }
  element_type &operator=(const VariantView &other) const
  {
    return get() = other.get();
  }

  element_type &get() const
  {
    using std::get;
    return get<element_type>(v_);
  }
  element_type &&move() const { return std::move(get()); }
  element_type &operator*() const { return get(); }
  operator element_type &() const { return get(); }
  element_type *operator->() const { return std::addressof(get()); }
};

} // namespace detail

template<typename T, typename V>
class std::formatter<detail::VariantView<T, V>> : public std::formatter<T> {};

template<typename T, typename... Ts> requires x23::is_one_of<T, Ts...>
inline auto
variant_view(variant<Ts...> &v)
{
  return detail::VariantView<T, variant<Ts...>>{v};
}

struct Production
  : variant<std::monostate, Terminal, UnionArm, UnionBody, EnumBody, StructBody,
            Declaration, ConstDef, TypeSpecifier, Definition, ProcDef, VersDef,
            ProgDef> {
  template<typename T> auto get() requires requires { variant_view<T>(*this); }
  {
    return variant_view<T>(*this);
  }
};
#define YYSTYPE Production

struct TerminalInfo;

struct XdrCtx {
  string file;
  vector<TerminalInfo> syms;
  vector<Definition> defs;
  bool error = false;
  bool warning = false;
  string literals;
  string block_comment;
};
void checkliterals(XdrCtx *);

struct YYLTYPE;
void yyerror(YYLTYPE *loc, XdrCtx *, string, bool warn_only = false);

#define YY_DECL                                                                \
  int real_yylex(string *yylval_param, YYLTYPE *yylloc_param,                  \
                 yyscan_t yyscanner, XdrCtx *xc)

std::string gen_hh(vector<Definition> *defs);
std::string gen_rs(vector<Definition> *defs);

struct Join {
  string str_;
  bool first_ = true;

  Join(string str) : str_(std::move(str)) {}
  string get()
  {
    if (!first_)
      return str_;
    first_ = false;
    return {};
  }
  void reset() { first_ = true; }
};

#ifndef FLEX_SCANNER
#include "x23c/lex.hh"
#endif // !FLEX_SCANNER
