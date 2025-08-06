
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <new>
#include <print>

#include <fcntl.h>
#include <getopt.h>
#include <set>
#include <stdio.h>
#include <string>
#include <sys/fcntl.h>
#include <unistd.h>

#include "x23/cleanup.h"
#include "x23/config.h"
#include "x23/err.h"
#include "x23c/defs.h"
#include "x23c/parse.hh"

using namespace x23;

std::filesystem::path progname;

YY_DECL;

struct Scanner {
  yyscan_t s;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wignored-attributes"
  ManagedPtr<fclose> f{nullptr};
#pragma GCC diagnostic pop

  Scanner()
  {
    if (yylex_init(&s))
      throw std::bad_alloc{};
  }
  Scanner(const Scanner &) = delete;
  ~Scanner() { yylex_destroy(s); }

  operator yyscan_t() { return s; }

  void set_in(FILE *in)
  {
    f.reset(in);
    yyset_in(f, s);
  }
};

struct TerminalInfo {
  int id;
  Terminal sym;
  YYLTYPE loc;
};

int
yylex(YYSTYPE *yylval_param, YYLTYPE *yylloc_param, size_t *pos, XdrCtx *xc)
{
  for (;;) {
    const auto &t = xc->syms.at((*pos)++);
    if (t.id == T_LITERAL) {
      xc->literals += t.sym.text.substr(1);
      xc->literals += '\n';
      continue;
    }
    yylval_param->emplace<Terminal>(t.sym);
    *yylloc_param = t.loc;
    return t.id;
  }
}

void
lex(XdrCtx *xc)
{
  FILE *in = fopen(xc->file.c_str(), "r");
  if (!in) {
    std::println(stderr, "{}: {}", xc->file, Errno{});
    exit(1);
  }

  Scanner s;
  s.set_in(in);

  YYLTYPE loc{
      .first_line = 1,
      .first_column = 1,
      .last_line = 1,
      .last_column = 1,
  };
  int sym;
  int comment_end_line = 0;
  do {
    string text;
    sym = real_yylex(&text, &loc, s, xc);
    if (sym == T_BLOCKCOMMENT) {
      if (!xc->block_comment.empty())
        xc->block_comment += '\n';
      xc->block_comment += text;
      comment_end_line = loc.last_line;
      continue;
    }
    else if (sym == T_LINECOMMENT) {
      assert(!xc->syms.empty());
      xc->syms.back().sym.eol_comment =
          string(loc.first_column - 1, ' ') + text + '\n';
      continue;
    }
    else {
      if (!xc->block_comment.empty() && loc.first_line > comment_end_line)
        xc->block_comment += '\n';
      xc->syms.push_back(TerminalInfo{
          .id = sym,
          .sym =
              Terminal{
                  .text = text,
                  .block_comment = std::exchange(xc->block_comment, {}),
              },
          .loc = loc,
      });
    }
  } while (sym != YYEOF);
}

[[noreturn]] static void
usage(bool error)
{
  std::println(error ? stderr : stdout,
               R"(usage: {0:} [OPTIONS] [-o FILE] file.x
  --help       print usage and exit
  --strict     fail if there are any warnings
  --version    print version and exit
  --rust       generate rust code
  --recursive_type <typename>  generate (rust) code for a recursive type)",
               progname.string());
  exit(error ? 2 : 0);
}

// In our code, we should use the kRustBoxPtr comment to indicate that a type
// should be wrapped in a Box<T> in rust.  However, for generating code
// for xdr we do not control (i.e. stellar's) we cannot add those annotations,
// and so need the --recursive_type option to indicate which types are
// recursive.

int
main(int argc, char **argv)
{
  progname = std::filesystem::path(argc > 0 ? argv[0] : "x23c").filename();

  bool strict = false;
  std::filesystem::path outfile;

  bool rust_module = false;
  std::set<std::string> recursive_types;

  int optind;
  for (optind = 1; optind < argc; ++optind) {
    std::string opt{argv[optind]};
    if (!opt.starts_with("-"))
      break;
    else if (opt == "--") {
      ++optind;
      break;
    }
    else if (opt == "--version") {
      std::println("{}", X23_PACKAGE_STRING);
      exit(0);
    }
    else if (opt == "--rust") {
      rust_module = true;
    }
    else if (opt == "--strict")
      strict = true;
    else if (opt == "--help")
      usage(false);
    else if (opt.starts_with("-o")) {
      if (opt.size() > 2)
        outfile = opt.substr(2);
      else {
        ++optind;
        if (optind >= argc) {
          std::println(stderr, "missing output file name");
          exit(2);
        }
        outfile = argv[optind];
      }
    }
    else if (opt == "--recursive_type") {
      ++optind;
      if (optind >= argc) {
        std::println(stderr, "missing recursive type name");
        exit(2);
      }
      recursive_types.insert(argv[optind]);
    }
    else {
      std::println(stderr, "invalid option {}", opt);
      usage(true);
    }
  }

  if (optind + 1 != argc)
    usage(true);

  std::filesystem::path infile = argv[optind];

  if (outfile.empty()) {
    outfile = infile.filename();
    if (outfile.extension() == ".x")
      if (rust_module) {
        outfile.replace_extension(".rs");
      }
      else {
        outfile.replace_extension(".hh");
      }
    else if (rust_module) {
      outfile += ".rs";
    }
    else {
      outfile += ".hh";
    }
  }

  XdrCtx ctx{.file = infile};
  lex(&ctx);

  size_t pos = 0;
  yyparse(&pos, &ctx);
  checkliterals(&ctx);
  if (ctx.error)
    exit(1);
  if (strict && ctx.warning) {
    std::println(stderr, "{}: strict mode enabled, warnings treated as errors.",
                 progname.string());
    exit(1);
  }

  std::string res;

  if (rust_module) {
    res = gen_rs(&ctx.defs, recursive_types);
  }
  else {
    res = gen_hh(&ctx.defs);
  }

  int outfd = outfile == "-"
                  ? 1
                  : open(outfile.c_str(),
                         O_CREAT | O_WRONLY | O_TRUNC | O_WRONLY, 0666);
  if (outfd == -1) {
    std::println(stderr, "{}: {}", outfile.string(), Errno{});
    exit(1);
  }
  write(outfd, res.data(), res.size());
  if (outfd != 1)
    close(outfd);
  return 0;
}
