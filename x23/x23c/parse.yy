
%require "3.8"

%code requires {
#if defined(__GNUC__) && !defined(__clang__) && !defined(__OPTIMIZE__)
// https://gcc.gnu.org/bugzilla/show_bug.cgi?id=98753
#pragma GCC diagnostic ignored "-Wfree-nonheap-object"
#endif

#include <print>

#include "x23c/defs.h"

YY_DECL;

using enum Declaration::Qual;
}

%define parse.trace
%output "x23c/parse.cc"
%header
%locations
%no-lines /**/
%param {size_t *pos}
%param {XdrCtx *xc}
%define api.pure full

%code {
inline void
yyerror(YYLTYPE *loc, size_t *, XdrCtx *xc, const char *msg)
{
  yyerror(loc, xc, msg);
}

inline string
combine(const string s1, const string s2)
{
  if (s1.empty())
    return s2;
  auto pos = s2.find_first_not_of(" \t");
  return pos == s2.npos ? s1 : s1 + s2.substr(pos);
}

int yylex(YYSTYPE *yylval_param, YYLTYPE *yylloc_param,
	  size_t *pos, XdrCtx *xc);
}

%token T_BLOCKCOMMENT T_LINECOMMENT T_LITERAL

%token <get<Terminal>()> T_ID T_NUM

%token <get<Terminal>()> T_CONST T_STRUCT T_UNION T_ENUM T_TYPEDEF
%token <get<Terminal>()> T_PROGRAM T_NAMESPACE
%token <get<Terminal>()> T_UNSIGNED T_BOOL T_INT T_HYPER T_VOID
%token <get<Terminal>()> T_FLOAT T_DOUBLE T_QUADRUPLE
%token <get<Terminal>()> T_VERSION T_SWITCH T_CASE T_DEFAULT
%token <get<Terminal>()> T_OPAQUE T_STRING
%token <get<Terminal>()> ',' ';'

%type <get<Terminal>()> value case_value opt_value scalar_type void_scalar_type
%type <get<UnionArm>()> case_list case_spec default_case_spec
%type <get<UnionBody>()> opt_case_specs case_specs union_body
%type <get<Declaration>()> declaration void_declaration
%type <get<ConstDef>()> constant_def
%type <get<EnumBody>()> enum_tag_list enum_body
%type <get<StructBody>()> declaration_list struct_body
%type <get<TypeSpecifier>()> type_specifier
%type <get<Definition>()> definition type_def prog_def
%type <get<ProcDef>()> proc_def arg_list
%type <get<VersDef>()> proc_def_list vers_def
%type <get<ProgDef>()> vers_def_list

%%

specification:
  %empty {
    checkliterals(xc);
    (void) &yynerrs;  // avoid unused warning
  }
| specification definition {
    xc->defs.push_back($definition);
    checkliterals(xc);
  }
| specification T_NAMESPACE T_ID[ns] {
    checkliterals(xc);
    if (!$T_NAMESPACE->block_comment.empty())
      xc->defs.push_back(Literal{ .lines = $T_NAMESPACE->block_comment });
    xc->defs.push_back(OpenNS{$ns->text});
  } '{' specification '}' { xc->defs.push_back(CloseNS{$ns->text}); }
;

definition:
  type_def
| constant_def { $$ = $1; }
| prog_def
;

type_def:
  T_TYPEDEF declaration ';'[semicolon] {
    checkliterals(xc);
    $$ = Typedef($declaration.get());
    $$->comment = combine($1->block_comment, $semicolon->eol_comment);
  }
| T_ENUM T_ID[id] enum_body[type] ';'[semicolon] {
    checkliterals(xc);
    $$ = EnumDecl{ .id = $id->text, .body = $type};
    $$->comment = $1->block_comment;
  }
| T_STRUCT T_ID[id] struct_body[type] ';'[semicolon] {
    checkliterals(xc);
    $$ = StructDecl{ .id = $id->text, .body = $type};
    $$->comment = $1->block_comment;
  }
| T_UNION T_ID[id] union_body[type] ';'[semicolon] {
    checkliterals(xc);
    $$ = UnionDecl{ .id = $id->text, .body = $type};
    $$->comment = $1->block_comment;
  }
;

constant_def:
  T_CONST T_ID '=' T_NUM ';'[semicolon] {
    $$ = ConstDef{ .id = $T_ID->text, .val = $T_NUM->text,
    .comment = combine($1->block_comment, $semicolon->eol_comment), };
  }
;

case_list:
  T_CASE case_value ':' { $$ = UnionArm{ .cases = { $case_value->text } }; }
| case_list T_CASE case_value ':' {
    $$ = $1.move();
    $$->cases.push_back($case_value->text);
  }
;

case_value:
  value {
    $$ = $value.move();
    if ($$->text == "true" || $$->text == "false")
      yyerror(&@value, xc,
	      "RFC4506 specifies use of capital \"TRUE\" and \"FALSE\".", true);
  }
;

case_spec: case_list void_declaration ';'[semicolon] {
    $$ = $case_list.move();
    $$->decl = $void_declaration.move();
    $$->decl.comment = combine($$->decl.comment, $semicolon->eol_comment);
  }
;

default_case_spec:
  T_DEFAULT ':' void_declaration ';'[semicolon] {
    $$ = UnionArm{ .has_default = true };
    $$->decl = $void_declaration.move();
    $$->decl.comment = combine($$->decl.comment, $semicolon->eol_comment);
  }
| case_list T_DEFAULT ':' void_declaration ';'[semicolon] {
    yyerror(&@case_list, xc,
    	    "RFC4506 prohibits grouping cases with default.", true);
    ($$ = $case_list.move()).has_default = true;
    $$->decl = $void_declaration.move();
    $$->decl.comment = combine($$->decl.comment, $semicolon->eol_comment);
  }
;

opt_case_specs:
  %empty { $$ = UnionBody{}; }
| opt_case_specs[old] case_spec {
    $$ = $old.move();
    $$->arms.push_back($case_spec.move());
  }
;

case_specs:
  opt_case_specs[list] case_spec[final] {
    $$ = $list.move();
    $$->arms.push_back($final.move());
  }
| opt_case_specs[list] default_case_spec[final] {
    $$ = $list.move();
    $$->arms.push_back($final.move());
  }
;

union_body: T_SWITCH '(' scalar_type[type] T_ID[id] ')' '{' case_specs '}' {
    $$ = UnionBody{ .switch_type = $type->text, .switch_id = $id->text,
       	 	    .arms = $case_specs.move().arms };
  }
;

enum_tag_list:
  T_ID[id] '=' value {
    $$ = EnumBody{{{.id = $id->text, .val = $value->text,
    .comment = combine($id->block_comment, $value->eol_comment) }}};
  }
| enum_tag_list[old] ','[comma] T_ID[id] '=' value {
    $$ = $old.move();
    $$->back().comment = combine($$->back().comment, $comma->eol_comment);
    $$->push_back(ConstDef{.id = $id->text, .val = $value->text,
      .comment = combine($id->block_comment, $value->eol_comment) });
  }
;

opt_comma:
  %empty
| ',' {
    yyerror(&@1, xc, "RFC4506 prohibits trailing comma.", true);
  }
;

enum_body: '{' enum_tag_list[list] opt_comma '}' { $$ = $list; }
;

declaration_list:
  declaration ';'[semicolon] {
    $$ = StructBody{{$1.move()}};
    $$->back().comment = combine($$->back().comment,
                                 $semicolon->eol_comment);
  }
| declaration_list[old] declaration ';'[semicolon] {
    $$ = $old.move();
    $$->push_back($declaration.move());
    $$->back().comment = combine($$->back().comment,
                                 $semicolon->eol_comment);
  }
;

struct_body: '{' declaration_list '}' { $$ = $declaration_list; }
;

void_declaration:
  declaration
| T_VOID { $$ = Declaration{ .qual = kVoid, .comment = $1->block_comment }; }
;

declaration:
  type_specifier T_ID[id] {
    $$ = Declaration { .type = $1, .id = $id->text, .comment = $1->comment };
  }
| type_specifier '*' T_ID[id] {
    $$ = Declaration { .type = $1, .id = $id->text, .qual = kPtr,
       	 	       .comment = $1->comment };
  }
| type_specifier T_ID[id] '[' value[bound] ']' {
    $$ = Declaration{ .type = $1, .id = $id->text,
       	 	      .qual = kArray, .bound = $bound->text,
		      .comment = $1->comment };
  }
| T_OPAQUE T_ID[id] '[' value[bound] ']' {
    $$ = Declaration{ .type = $1->text, .id = $id->text,
       	 	      .qual = kArray, .bound = $bound->text,
		      .comment = $1->block_comment };
  }
| type_specifier T_ID[id] '<' opt_value[bound] '>' {
    $$ = Declaration{ .type = $1, .id = $id->text,
       	 	      .qual = kVec, .bound = $bound->text,
		      .comment = $1->comment };
  }
| T_OPAQUE T_ID[id] '<' opt_value[bound] '>' {
    $$ = Declaration{ .type = $1->text, .id = $id->text,
       	 	      .qual = kVec, .bound = $bound->text,
		      .comment = $1->block_comment };
  }
| T_STRING T_ID[id] '<' opt_value[bound] '>' {
    $$ = Declaration{ .type = $1->text, .id = $id->text,
       	 	      .qual = kVec, .bound = $bound->text,
		      .comment = $1->block_comment };
  }
;

scalar_type:
  T_ID
| T_BOOL
| T_INT
| T_HYPER
| T_FLOAT
| T_DOUBLE
| T_QUADRUPLE
| T_UNSIGNED T_INT { $$ = $1; $$->text = "unsigned int"; }
| T_UNSIGNED T_HYPER { $$ = $1; $$->text = "unsigned hyper"; }
| T_UNSIGNED {
    yyerror(&@1, xc,
    	    "RFC4506 requires 'int' or 'hyper' after unsigned (assuming int).",
	    true);
    $$ = $1;
    $$->text = "unsigned int";
  }
;

type_specifier:
  scalar_type {
    $$ = $1->text;
    $$->comment = $1->block_comment;
  }
| T_ENUM enum_body {
    $$ = $2.move();
    $$->comment = $1->block_comment;
  }
| T_STRUCT struct_body {
    $$ = $2.move();
    $$->comment = $1->block_comment;
  }
| T_UNION union_body {
    $$ = $2.move();
    $$->comment = $1->block_comment;
  }
;

value:
  T_ID
| T_NUM;
;

opt_value:
  %empty { $$ = Terminal{}; }
| value
;

prog_def: T_PROGRAM T_ID[name] '{' vers_def_list '}' '=' value ';' {
    $vers_def_list->id = $name->text;
    $vers_def_list->val = $value->text;
    $$ = $vers_def_list.move();
  }
;

vers_def_list:
  vers_def { $$ = ProgDef { .versions = {{ $vers_def.move() }} }; }
| vers_def_list vers_def { $$->versions.push_back($vers_def.move()); }
;

vers_def: T_VERSION T_ID[name] '{' proc_def_list '}' '=' value ';' {
    $$ = $proc_def_list.move();
    $$->id = $name->text;
    $$->val = $value->text;
  }
;

proc_def_list:
  proc_def { $$ = VersDef{ .procs = {{ $proc_def.move() }} }; }
| proc_def_list proc_def { $$->procs.push_back($proc_def.move()); }
;

proc_def: void_scalar_type[ret] T_ID[name] '(' T_VOID ')' '='
                                               value ';'[semicolon] {
  $$ = ProcDef{ .id = $name->text, .val = $value->text,
                .return_type = $ret->text,
		.comment = combine($ret->block_comment,
                                   $semicolon->eol_comment),
		};
  }
| void_scalar_type[ret] T_ID[name] '(' arg_list ')' '=' value ';'[semicolon] {
    $$ = $arg_list.move();
    $$->id = $name->text;
    $$->val = $value->text;
    $$->return_type = $ret->text;
    $$->comment = combine($ret->block_comment, $semicolon->eol_comment);
  }
;

void_scalar_type:
  scalar_type
| T_VOID
;

arg_list:
  scalar_type { $$ = ProcDef{ .arg_types = { $scalar_type->text } }; }
| arg_list[old] ',' scalar_type {
    $$ = $old.move();
    $$->arg_types.push_back($scalar_type->text);
  }
;

%%

void
yyerror(YYLTYPE *loc, XdrCtx *xc, string s, bool warn_only)
{
  std::println(stderr, "{}:{}:{}: {}{}", xc->file,
	       loc->first_line, loc->first_column,
	       warn_only ? "warning: " : "", s);
  (warn_only ? xc->warning : xc->error) = true;
}

void
checkliterals(XdrCtx *xc)
{
  if (!xc->literals.empty())
    xc->defs.push_back(Literal{.lines = std::exchange(xc->literals, {})});
}
