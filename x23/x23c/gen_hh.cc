#include <cstddef>
#include <format>
#include <map>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>

#include "x23/cleanup.h"
#include "x23c/defs.h"

using namespace x23;

using std::string_literals::operator""s;

struct Gen {
  string top_;
  string bottom_;
  std::size_t indent_{};
  vector<string> namespaces_;
  vector<string> scopes_;

  void process(const vector<Definition> &defs);

  void show_def(const ConstDef &c)
  {
    show_comment(c.comment, false);
    prtop("constexpr auto {} = {};\n", c.id, c.val);
  }
  void show_def(const Typedef &d)
  {
    prtop("using {} = {};\n", d.id, type_name(d, true));
  }
  void show_def(const EnumDecl &t)
  {
    show_typespec(t.body, t.id);
    prtop(";\n");
  }
  void show_def(const StructDecl &t)
  {
    show_typespec(t.body, t.id);
    prtop(";\n");
  }
  void show_def(const UnionDecl &t)
  {
    show_typespec(t.body, t.id);
    prtop(";\n");
  }
  void show_def(const OpenNS &ns)
  {
    namespaces_.push_back(ns.id);
    prtop("namespace {} {{\n", ns.id);
  }
  void show_def(const CloseNS &ns)
  {
    namespaces_.pop_back();
    prtop("}} // namespace {}\n", ns.id);
  }
  void show_def(const Literal &l) { top_ += l.lines; }
  void show_def(const ProgDef &prog);

  void show_decl(const Declaration &d)
  {
    show_comment(d.comment, false);
    if (d.qual == Declaration::kVoid) {
      return;
    }
    prtop("{} {};\n", type_name(d, true), d.id);
  }

  void show_typespec(const TypeSpecifier &ts, string t)
  {
    visit([this, &t](const auto &v) { show_type(v, t); }, ts);
  }

  void show_type(const string &s, string) { prtop("{}", map_type(s)); }
  void show_type(const EnumBody &e, string);
  void show_type(const StructBody &s, string);
  void show_type(const UnionBody &b, string);

  void show_comment(std::string_view comment, bool chomp, string prefix = {});

  static const string &map_type(const string &);
  static const string &map_case(const string &);
  static const string &map_bound(const string &);
  string type_name(const Declaration &d, bool generate);

  template<typename... Args>
  void prtop(std::format_string<Args...> fmt, Args &&...args)
  {
    if (top_.empty() || top_.back() == '\n')
      top_ += std::string(2 * indent_, ' ');
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
Gen::process(const vector<Definition> &defs)
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
Gen::show_def(const ProgDef &prog)
{
  for (const VersDef &vers : prog.versions) {
    top_ += std::format(R"(
template<typename Server,
         template<typename> typename RetTrans,
         typename...ExtraArgs>
concept is_{0} =)",
                        vers.id);
    Join andand(" &&");
    for (const auto &proc : vers.procs) {
      top_ += std::format(
          R"({0}
  requires(Server __s, ExtraArgs...ea) {{
    {{ __s->{2}({3}{4}ea...) }}
      -> std::same_as<RetTrans<{1}>>;
  }})",
          andand.get(), map_type(proc.return_type), proc.id,
          join(proc.arg_types | std::views::transform(map_type) |
                   std::views::transform(
                       [](const string t) { return t + "{}"; }),
               ", "),
          proc.arg_types.empty() ? "" : ", ");
    }
    top_ += ";\n";

    top_ += std::format(R"(struct {3} {{
  static constexpr std::uint32_t program = {0};
  static constexpr const char *program_name = "{1}";
  static constexpr std::uint32_t version = {2};
  static constexpr const char *version_name = "{3}";
)",
                        prog.val, prog.id, vers.val, vers.id);

    string procnos;
    Join comma(", ");
    for (const auto &proc : vers.procs)
      procnos += comma.get(), procnos += proc.val;

    top_ += std::format(
        "  static constexpr std::array<std::uint32_t, {}> valid_procnos{{\n"
        "      {}\n"
        "  }};\n",
        vers.procs.size(), procnos);
    if (!namespaces_.empty())
      bottom_ += format("\nnamespace {} {{", ns());

    top_ += "  template<std::uint32_t ProcNo> struct proc_meta;\n";

    bottom_ += "\n";
    for (size_t i = 0, e = vers.procs.size(); i < e; ++i) {
      const auto &proc = vers.procs[i];
      comma.reset();
      string argtypes = "std::tuple<";
      for (const string &t : proc.arg_types)
        argtypes += std::format("{}{}", comma.get(), map_type(t));
      argtypes += ">";
      string dispatch = std::format(
          R"(return std::forward<Server>(server).{}(
)",
          proc.id);
      for (size_t j = 0; j < proc.arg_types.size(); ++j)
        dispatch += std::format("          std::move(get<{}>(args)),\n", j);
      dispatch += "          std::forward<ExtraArgs>(ea)...);";

      bottom_ += std::format(R"(template<> struct {0}::proc_meta<{1}> {{
    using version_meta = {6};
    static constexpr std::uint32_t proc = {1};
    static constexpr const char *proc_name = "{2}";
    using arg_type = {4};
    using return_type = {3};
    using return_wire_type = {7};
    template<typename Server, typename ...ExtraArgs>
    static decltype(auto)
    dispatch(Server &&server, arg_type &args, ExtraArgs &&...ea) {{
      {5}
    }}
}};
)",
                             vers.id, proc.val, proc.id, proc.return_type,
                             argtypes, dispatch, vers.id,
                             proc.return_type == "void"s
                                 ? "std::array<std::uint8_t, 0>"
                                 : proc.return_type);
    }

    top_ += R"(
  template<typename __SendCall, typename ...__ExtraArgs>
  struct client {
    __SendCall __send_call;
)";

    for (const auto &proc : vers.procs) {
      top_ += std::format("\n    decltype(auto) // {0}\n"
                          "    {1}(",
                          map_type(proc.return_type), proc.id);
      comma.reset();
      for (int i = 0, n = proc.arg_types.size(); i < n; ++i)
        top_ += std::format("{}const {} &__a{}", comma.get(),
                            map_type(proc.arg_types[i]), i);
      if (!proc.arg_types.empty())
        top_ += std::format(",\n{0:{1}}", "", proc.id.size() + 5);
      top_ += std::format("__ExtraArgs...__ea)\n", comma.get(),
                          map_type(proc.return_type));
      top_ += "    {\n";
      top_ += std::format(
          "      return __send_call->template send_call<proc_meta<{0}>>(\n"
          "          __ea...",
          proc.val);
      for (int i = 0, n = proc.arg_types.size(); i < n; ++i)
        top_ += std::format(", __a{}", i);
      top_ += ");\n";
      top_ += "    }\n";
    }

    top_ += R"(  };
};
)";

    bottom_ += std::format("static_assert(is_{0}<{0}::client<x23::"
                           "VacuousSendCall *>*, std::type_identity_t>);\n",
                           vers.id);
    if (!namespaces_.empty())
      bottom_ += std::format("}} // namespace {}\n", ns());
    bottom_ +=
        std::format(R"(
template<typename Server, template<typename> typename RetTrans,
         typename... ExtraArgs>
struct x23::rpc_compatible<Server, {0}, RetTrans, ExtraArgs...>
  : std::bool_constant<{1}<Server, RetTrans, ExtraArgs...>>
{{}};

)",
                    scope(true) + vers.id, scope(true) + "is_"s + vers.id);
  }
}

void
Gen::show_type(const EnumBody &e, string t)
{
  prtop("enum class {} : int32_t ", t);
  auto _close = open_brace();
  scopes_.push_back(t);
  bottom_ += std::format(R"(template<>
struct x23::xdr_meta<enum {0}> {{
  static constexpr x23::XdrCategory category = x23::XdrCategory::kEnum;
  using enum_type = enum {0};
  using enum enum_type;
  static constexpr const char *type_name = "{1}";
  static constexpr std::array tags{{
)",
                         scope(true), t);
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
    bottom_ += std::format("    std::pair{{ {0}, \"{0}\" }},\n", cd.id);
  }
  bottom_ += "  };\n};\n";
}

void
Gen::show_type(const StructBody &s, string t)
{
  prtop("struct {} ", t);
  auto _close = open_brace();
  scopes_.push_back(t);
  for (const auto &d : s)
    show_decl(d);
  top_ += '\n';
  prtop("bool operator==(const {} &) const = default;\n", t);
  if (scopes_.size() > 1)
    // This doesn't work for recursive structures (e.g., that contain
    // a Ptr or Vec of themselves).  But we know in-line structures
    // cannot be recursive, since they have no name in the XDR file.
    prtop("auto operator<=>(const {} &) const = default;\n", t);
  else {
    prtop("friend std::partial_ordering\n");
    prtop("operator<=> (const {0} &, const {0} &);\n", t);
  }

  size_t nonvoid_count = 0;
  for (auto const &decl : s) {
    if (decl.qual != Declaration::kVoid)
      ++nonvoid_count;
  }

  if (!namespaces_.empty())
    bottom_ += format("\nnamespace {} {{", ns());
  bottom_ += std::format(R"(
template<size_t I, x23::references<struct {1}> T>
requires (I < {0})
decltype(auto)
get(T &&t)
{{)",
                         nonvoid_count, scope());
  size_t nonvoid_idx = 0;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i].qual == Declaration::kVoid)
      continue;
    bottom_ += std::format(R"(
  if constexpr (I == {})
    return (std::forward<T>(t).{});)",
                           nonvoid_idx, s[i].id);
    ++nonvoid_idx;
  }
  bottom_ += "\n}\n";

  if (!namespaces_.empty())
    bottom_ += format("}} // namespace {}\n", ns());

  bottom_ += std::format(R"(template<>
struct std::tuple_size<struct {1}>
  : std::integral_constant<std::size_t, {0}> {{}};
template<std::size_t I> requires (I < {0})
struct std::tuple_element<I, struct {1}> {{
  using struct_type = struct {1};
  using type = decltype(auto(get<I>(struct_type{{}})));
}};
template<>
struct x23::xdr_meta<struct {1}> {{
  static constexpr x23::XdrCategory category = x23::XdrCategory::kStruct;
  static constexpr const char *type_name = "{2}";
  static constexpr std::array<std::string_view, {0}> field_names{{
)",
                         nonvoid_count, scope(true), t);
  for (const auto &d : s) {
    if (d.qual != Declaration::kVoid)
      bottom_ += std::format("    \"{}\",\n", d.id);
  }
  bottom_ += "  };\n";
  bottom_ += "};\n";

  if (!namespaces_.empty())
    bottom_ += format("\nnamespace {} {{\n", ns());

  if (scopes_.size() == 1)
    bottom_ += std::format(R"(inline std::partial_ordering
operator<=>(const struct {0} &a, const struct {0} &b)
{{
  return x23::xdr_cmp(a, b);
}}
)",
                           scope());

  if (!namespaces_.empty())
    bottom_ += format("}} // namespace {}\n", ns());
}

void
Gen::show_type(const UnionBody &b, string t)
{
  prtop("struct {} ", t);
  auto _close = open_brace();
  scopes_.push_back(t);
  string switch_type = map_type(b.switch_type);
  string first_case = b.arms[0].cases.empty() ? switch_type + "{}"
                                              : map_case(b.arms[0].cases[0]);
  // does not account for typedefs, but that's not the end of the world
  static std::set<std::string> non_enum_switch_types = {"int", "unsigned int",
                                                        "hyper",
                                                        "unsigned hyper",
                                                        "bool"};

  if (non_enum_switch_types.find(b.switch_type) ==
      non_enum_switch_types.end()) {
    prtop("using enum {};\n", switch_type);
  }
  prtop("{0}() noexcept : {0}({1}) {{}}\n", t, first_case);
  prtop("inline explicit {0}({1}) noexcept;\n", t, switch_type);
  prtop("inline {0}({0} &&) noexcept;\n", t);
  prtop("inline {0}(const {0} &);\n", t);
  prtop("inline ~{0}();\n", t);
  prtop("inline {0} &operator=({0} &&other) noexcept;\n", t);
  prtop("inline {0} &operator=(const {0} &other);\n", t);
  top_ += '\n';
  prtop("// get and set discriminant\n");
  prtop("{0} {1}() const noexcept "
        "{{ return __xdr_union_field_{1}; }}\n",
        switch_type, b.switch_id);
  prtop("inline {2} &{1}({0}) noexcept;\n", switch_type, b.switch_id, t);
  top_ += '\n';

  bool has_default = false;
  bool has_void = false;
  for (const auto &a : b.arms) {
    for (const auto &c : a.cases)
      prtop("// case {}:\n", c);
    if (a.has_default) {
      has_default = true;
      prtop("// default:\n");
    }
    show_comment(a.decl.comment, false, "//   ");
    if (a.decl.qual == Declaration::kVoid) {
      has_void = true;
      prtop("//   void;\n");
    }
    else
      prtop("//   {} &{}();\n", type_name(a.decl, false), a.decl.id);
  }
  top_ += '\n';

  std::size_t num_non_void = 0;
  struct arminfo {
    string type_prefix;
    string type_suffix;
    std::size_t variant_index;
    const UnionArm *arm;
  };
  vector<arminfo> aiv;

  for (const auto &a : b.arms) {
    if (a.decl.qual == Declaration::kVoid)
      continue;
    string tname = type_name(a.decl, true);
    arminfo info{
        .variant_index = ++num_non_void,
        .arm = &a,
    };
    for (const auto &prefix : {"enum "s, "struct "s})
      if (tname.starts_with(prefix)) {
        info.type_prefix = prefix;
        break;
      }
    info.type_suffix = tname.substr(info.type_prefix.size());

    prtop("template<typename Self> decltype(auto) {}(this Self &&self)\n",
          a.decl.id);
    prtop("{{\n");
    prtop("  return get<{}>(std::forward<Self>(self));\n", num_non_void);
    prtop("}}\n");
    aiv.push_back(std::move(info));
  }
  if (num_non_void)
    top_ += '\n';
  prtop("friend bool operator==(const {0} &, const {0} &);\n", t);
  prtop("friend {}\n", scopes_.size() > 1 ? "auto"s : "std::partial_ordering"s);
  prtop("operator<=>(const {0} &, const {0} &);\n", t);
  // Note __xdr_union_arm_number is used by the gdb python scripts to
  // print the active arm.  We tag it gnu::used so it doesn't get
  // optimized away.
  prtop("[[gnu::used]] static constexpr int __xdr_union_arm_number({});\n\n",
        switch_type);
  top_ += string(2 * (indent_ - 1), ' ') + "private:\n"s;
  prtop("{} __xdr_union_field_{};\n", switch_type, b.switch_id);
  if (num_non_void) {
    prtop("union ");
    auto uclose = open_brace();
    for (const auto &info : aiv)
      prtop("{}{} __xdr_union_field_{};\n", info.type_prefix, info.type_suffix,
            info.arm->decl.id);
    uclose.reset();
    prtop(";\n");
  }
  top_ += '\n';
  prtop("template<size_t I> requires (I <= {})\n", num_non_void);
  prtop("static constexpr auto __xdr_union_fieldptr();\n\n");
  prtop("template<x23::is_xdr_union T> friend struct x23::union_ops;\n\n", t);
  prtop("template<std::size_t I, x23::references<{}> U>"
        " requires (I <= {})\n",
        t, num_non_void);
  prtop("friend decltype(auto) get(U &&);\n");

  bottom_ += '\n';

  bottom_ += std::format(R"(template<>
struct x23::xdr_meta<struct {0}> {{
  static constexpr x23::XdrCategory category = x23::XdrCategory::kUnion;
  static constexpr const char *type_name = "{2}";
  static constexpr int default_field = {3};
  static constexpr bool has_void = {4:s};
  static constexpr std::array field_names{{
    "{1}", // discriminant
)",
                         scope(true), b.switch_id, t,
                         has_default ? int(num_non_void) : -1, has_void);
  for (const auto &info : aiv)
    bottom_ += std::format("    \"{}\",\n", info.arm->decl.id);
  bottom_ += "  };\n";
  // Record the cases, as they coud be useful for fuzzing tools
  size_t ncases = 0;
  for (const auto &a : b.arms)
    ncases += a.cases.size();
  bottom_ += std::format(
      R"(  using discriminant_type =
    decltype(std::declval<struct {0}>().{1}());
  using enum x23::AlwaysEnum<discriminant_type>::type;
  static constexpr
  std::array<std::pair<discriminant_type, int>, {2}> cases{{
)",
      scope(true), b.switch_id, ncases);
  {
    int nonvoidcount = 0;
    for (const auto &a : b.arms) {
      int armno;
      if (a.decl.qual != Declaration::kVoid)
        armno = ++nonvoidcount;
      else
        armno = 0;
      for (const auto &c : a.cases)
        bottom_ += format("    std::pair{{{}, {}}},\n", map_case(c), armno);
    }
  }
  bottom_ += "  };\n";
  bottom_ += "};\n";

  if (!namespaces_.empty())
    bottom_ += std::format("namespace {} {{\n", ns());

  bottom_ += std::format(R"(constexpr int
{1}::__xdr_union_arm_number({0} discriminant)
{{
  using enum x23::AlwaysEnum<{0}>::type;
  switch (x23::unbool(discriminant)) {{
)",
                         switch_type, scope(false));

  int ctr = 0;
  for (const auto &a : b.arms) {
    for (const auto &c : a.cases) {
      bottom_ += std::format("  case {}:\n", map_case(c));
    }
    if (a.has_default)
      bottom_ += "  default:\n";
    bottom_ += std::format("    return {};\n",
                           a.decl.qual == Declaration::kVoid ? 0 : ++ctr);
  }
  if (!has_default)
    bottom_ += "  default:\n    return -1;\n";

  bottom_ += "  }\n}\n";

  bottom_ += std::format(R"(template<std::size_t I> requires (I <= {0})
constexpr auto
{1}::__xdr_union_fieldptr()
{{
  if constexpr (I == 0)
    return &{2}::__xdr_union_field_{3};)",
                         num_non_void, scope(false), t, b.switch_id);
  for (const auto &info : aiv)
    bottom_ += std::format(R"(
  else if constexpr (I == {0})
    return &{1}::__xdr_union_field_{2};)",
                           info.variant_index, t, info.arm->decl.id);
  bottom_ += "\n}\n"s;

  bottom_ +=
      std::format(R"(inline
{1}::{0}({2} d) noexcept
{{
  using ops = x23::union_ops<struct {0}>;
  ops::construct(*this, d);
}}
inline
{1}::{0}({0} &&other) noexcept
{{
  using ops = x23::union_ops<struct {0}>;
  ops::construct(*this, std::move(other));
}}
inline
{1}::{0}(const {0} &other)
{{
  using ops = x23::union_ops<struct {0}>;
  ops::construct(*this, other);
}}
inline
{1}::~{0}()
{{
  using ops = x23::union_ops<struct {0}>;
  ops::destroy(*this);
}}
inline struct {1} &
{1}::operator=(struct {1} &&other) noexcept
{{
  using ops = x23::union_ops<struct {0}>;
  ops::assign(*this, std::move(other));
  return *this;
}}
inline struct {1} &
{1}::operator=(const struct {1} &other)
{{
  using ops = x23::union_ops<struct {0}>;
  ops::assign(*this, other);
  return *this;
}}
inline struct {1} &
{1}::{3}({2} d) noexcept
{{
  using ops = x23::union_ops<struct {0}>;
  ops::set_discriminant(*this, d);
  return *this;
}}
template<size_t I, x23::references<struct {1}> U>
requires (I <= {4})
inline decltype(auto)
get(U &&u)
{{
  using ops = x23::union_ops<struct {1}>;
  if constexpr (I == 0)
    return ops::get_discriminant(u);
  else
    return ops::get<I>(std::forward<U>(u));
}}
inline bool
operator==(const struct {1} &a, const struct {1} &b)
{{
  return x23::union_ops<struct {1}>::eq(a, b);
}}
inline {5}
operator<=>(const struct {1} &a, const struct {1} &b)
{{
  return x23::union_ops<struct {1}>::cmp(a, b);
}}
)",
                  t, scope(false), switch_type, b.switch_id, num_non_void,
                  scopes_.size() > 1 ? "auto"s : "std::partial_ordering"s);

  if (!namespaces_.empty())
    bottom_ += std::format("}} // namespace {}\n", ns());
}

void
Gen::show_comment(std::string_view comment, bool chomp, string prefix)
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

const string &
Gen::map_type(const string &t)
{
  static const std::map<string, string> type_map{
      {"int", "std::int32_t"},    {"unsigned int", "std::uint32_t"},
      {"hyper", "std::int64_t"},  {"unsigned hyper", "std::uint64_t"},
      {"opaque", "std::uint8_t"},
  };
  if (auto it = type_map.find(t); it != type_map.end())
    return it->second;
  else
    return t;
}

const string &
Gen::map_case(const string &c)
{
  static const std::map<string, string> case_map{
      {"TRUE", "true"},
      {"FALSE", "false"},
  };
  if (auto it = case_map.find(c); it != case_map.end())
    return it->second;
  else
    return c;
}

const string &
Gen::map_bound(const string &b)
{
  static const string defbound = "0xffffffff";
  if (b.empty())
    return defbound;
  return b;
}

string
Gen::type_name(const Declaration &d, bool generate)
{
  using enum Declaration::Qual;

  if (d.qual == kVoid)
    return "void";

  string tname;
  if (holds_alternative<string>(d.type))
    tname = map_type(get<string>(d.type));
  else {
    if (holds_alternative<EnumBody>(d.type))
      tname = "enum " + d.id;
    else
      tname = "struct " + d.id;
    if (generate) {
      show_typespec(d.type, d.id);
      prtop(";\n");
    }
  }

  switch (d.qual) {
  case kScalar:
    return tname;
  case kPtr:
    return std::format("x23::Ptr<{}>", tname);
  case kArray:
    return std::format("std::array<{}, {}>", tname, d.bound);
  case kVec:
    if (tname == "string"s)
      return std::format("x23::BoundedStr<{}>", map_bound(d.bound));
    return std::format("x23::Vec<{}, {}>", tname, map_bound(d.bound));
  default:
    return {}; // suppress warnings
  }
}

string
gen_hh(vector<Definition> *defs)
{
  Gen gs;
  gs.prtop(R"(// -*-C++-*-

#pragma once

#include <algorithm>
#include <array>
#include <compare>
#include <concepts>
#include <cstdint>
#include <cstddef>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include <x23/vec.h>
#include <x23/utility.h>
#include <x23/x23.h>

)");
  gs.process(*defs);
  return gs.top_ + R"(
//
// XDR internals below this point
//

)" + gs.bottom_;
}
