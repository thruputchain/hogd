
%{
#include <string_view>
#include "x23c/defs.h"
#include "x23c/parse.hh"

#pragma GCC diagnostic ignored "-Wsign-compare"

#define YY_USER_ACTION							\
do {									\
  const std::string &text =						\
    yylval->assign(yytext, yyleng);					\
  yylloc->first_line = yylloc->last_line;				\
  yylloc->first_column = yylloc->last_column;				\
  for (char c : text)							\
    switch (c) {							\
    case '\n':								\
      yylloc->last_column = 1;						\
      ++yylloc->last_line;						\
      break;								\
    case '\t':								\
      yylloc->last_column += 8 - ((yylloc->last_column - 1) % 8);	\
      break;								\
    default:								\
      ++yylloc->last_column;						\
      break;								\
    }									\
} while(0);

#undef YYSTYPE
#define YYSTYPE string
%}

%option noline
%option reentrant bison-bridge bison-locations
%option outfile="x23c/lex.cc" header-file="x23c/lex.hh"
%option batch never-interactive
%option noyywrap nounput noinput
%option nodefault

ID	[a-zA-Z_][a-zA-Z_0-9]*
NUM	[+-]?([0-9'_]+|0x[0-9a-fA-F'_]+)
BLOCK_COMMENT	^([ \t]*\/\*([^\*]|\*[^\/])*\*\/[ \t]*\n)+
LINE_COMMENTS	^([ \t]*\/\/.*\n)+


%x ERR

%%

<ERR>[^\n \t;]*	{ BEGIN(INITIAL); }

<*>\n		{ BEGIN(INITIAL); }
[ \t\r\f]+	/* discard */;

{BLOCK_COMMENT}	|
{LINE_COMMENTS}	return T_BLOCKCOMMENT;

^%.*		return T_LITERAL;

\/\*([^\*]|\*[^/])*\*\/	|
\/\/.*(\n[ \t]*\/\/.*)*	return T_LINECOMMENT;

const		return T_CONST;
struct		return T_STRUCT;
union		return T_UNION;
enum		return T_ENUM;
typedef		return T_TYPEDEF;
program		return T_PROGRAM;
namespace       return T_NAMESPACE;

bool		return T_BOOL;
unsigned	return T_UNSIGNED;
int		return T_INT;
hyper		return T_HYPER;
float		return T_FLOAT;
double		return T_DOUBLE;
quadruple	return T_QUADRUPLE;
void		return T_VOID;

version		return T_VERSION;
switch		return T_SWITCH;
case		return T_CASE;
default		return T_DEFAULT;

opaque		return T_OPAQUE;
string		return T_STRING;

char		|
short		|
inline		|
sizeof		{ yyerror(yylloc, xc, "illegal use of reserved word"); }

{ID}		return T_ID;
{NUM}		return T_NUM;

[=;{}<>\[\]*,:()] return yytext[0];

<*>.		{
		  yyerror(yylloc, xc, "lex error");
		  BEGIN(ERR);
		}

%%
