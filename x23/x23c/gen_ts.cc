#include <algorithm>
#include <cctype>
#include <cstddef>
#include <format>
#include <map>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

#include "x23c/defs.h"

#include "x23c/config.h"

using std::string_view;

struct GenTs {
  string top_;
  string bottom_;
  std::size_t indent_{};
  std::set<string> local_types_;
  std::set<string> enum_types_;

  static constexpr int kIndentWidth = 2;

  void process(const vector<Definition> &defs);

  void show_def(const Definition &d)
  {
    prtop("\n");
    visit([this](const auto &d) { show_def(d); }, d);
  }

  void show_def(const ConstDef &c);
  void show_def(const Typedef &d);
  void show_def(const EnumDecl &e);
  void show_def(const StructDecl &s);
  void show_def(const UnionDecl &u);
  void show_def(const OpenNS &) {}
  void show_def(const CloseNS &) {}
  void show_def(const Literal &l);
  void show_def(const ProgDef &) {}

  // Type generators
  void gen_enum_type(const EnumBody &body, const string &name);
  void gen_struct_type(const StructBody &body, const string &name);
  void gen_union_type(const UnionBody &body, const string &name);

  // Generate inline types from anonymous struct/enum/union bodies
  void gen_inline_types_struct(const StructBody &body, const string &prefix);
  void gen_inline_types_union(const UnionBody &body, const string &prefix);

  // Codec generators
  void gen_typedef_codec(const Typedef &d);
  void gen_struct_codec(const StructBody &body, const string &name);
  void gen_union_codec(const UnionBody &body, const string &name);

  // Encode a single field into bottom_
  void encode_field(const Declaration &d, const string &val_expr,
                    const string &parent_name, int indent_spaces);
  // Decode a single field, appending statements to bottom_
  // Returns the variable name holding the decoded value
  string decode_field(const Declaration &d, const string &parent_name,
                      int indent_spaces);

  // TypeScript type for a struct/union field
  string ts_field_type(const Declaration &d, const string &parent_name);

  // Helpers
  static string map_type(const string &t);
  static string map_write_method(const string &t);
  static string map_read_method(const string &t);
  static string map_bound(const string &b);
  static bool is_primitive_type(const string &t);
  static string to_camel_case(const string &name);

  bool is_enum_discriminant(const string &switch_type)
  {
    return enum_types_.contains(switch_type) ||
           (!is_primitive_type(switch_type) && switch_type != "bool");
  }

  // Get the base type name from a Declaration's TypeSpecifier
  static string base_type_name(const Declaration &d)
  {
    if (holds_alternative<string>(d.type))
      return get<string>(d.type);
    return {};
  }

  template<typename... Args>
  void prtop(std::format_string<Args...> fmt, Args &&...args)
  {
    if (top_.empty() || top_.back() == '\n')
      top_ += std::string(kIndentWidth * indent_, ' ');
    top_ += std::vformat(fmt.get(), std::make_format_args(args...));
  }

  template<typename... Args>
  void prbottom(std::format_string<Args...> fmt, Args &&...args)
  {
    bottom_ += std::vformat(fmt.get(), std::make_format_args(args...));
  }

  // Append an indented line to bottom_
  void bottom_line(int indent_spaces, const string &line)
  {
    bottom_ += std::string(indent_spaces, ' ');
    bottom_ += line;
    bottom_ += '\n';
  }
};

// Convert PascalCase to camelCase
// SHA256Hash -> sha256Hash, Ed25519Signature -> ed25519Signature
string
GenTs::to_camel_case(const string &name)
{
  if (name.empty())
    return name;

  size_t upper_run = 0;
  while (upper_run < name.size() && std::isupper(name[upper_run]))
    ++upper_run;

  if (upper_run == 0)
    return name;

  string result = name;
  if (upper_run == 1 || upper_run == name.size() ||
      !std::islower(name[upper_run])) {
    // Lowercase the entire uppercase run
    for (size_t i = 0; i < upper_run; ++i)
      result[i] = static_cast<char>(std::tolower(result[i]));
  }
  else {
    // Lowercase all but the last (which starts the next word)
    for (size_t i = 0; i < upper_run - 1; ++i)
      result[i] = static_cast<char>(std::tolower(result[i]));
  }
  return result;
}

bool
GenTs::is_primitive_type(const string &t)
{
  static const std::set<string> primitives = {
      "int", "unsigned int", "hyper", "unsigned hyper", "bool",
  };
  return primitives.contains(t);
}

string
GenTs::map_type(const string &t)
{
  static const std::map<string, string> type_map{
      {"int", "bigint"},
      {"unsigned int", "bigint"},
      {"hyper", "bigint"},
      {"unsigned hyper", "bigint"},
      {"bool", "boolean"},
      {"opaque", "Uint8Array"},
      {"string", "string"},
  };
  if (auto it = type_map.find(t); it != type_map.end())
    return it->second;
  return t;
}

string
GenTs::map_write_method(const string &t)
{
  static const std::map<string, string> write_map{
      {"int", "writeInt32"},
      {"unsigned int", "writeUint32"},
      {"hyper", "writeInt64"},
      {"unsigned hyper", "writeUint64"},
      {"bool", "writeBool"},
  };
  if (auto it = write_map.find(t); it != write_map.end())
    return it->second;
  return "/* unknown write for " + t + " */";
}

string
GenTs::map_read_method(const string &t)
{
  static const std::map<string, string> read_map{
      {"int", "readInt32"},
      {"unsigned int", "readUint32"},
      {"hyper", "readInt64"},
      {"unsigned hyper", "readUint64"},
      {"bool", "readBool"},
  };
  if (auto it = read_map.find(t); it != read_map.end())
    return it->second;
  return "/* unknown read for " + t + " */";
}

string
GenTs::map_bound(const string &b)
{
  if (b.empty())
    return "0xFFFFFFFF";
  return b;
}

// ---- Process all definitions ----

void
GenTs::process(const vector<Definition> &defs)
{
  for (const auto &d : defs) {
    show_def(d);
  }
}

// ---- Definition handlers ----

void
GenTs::show_def(const ConstDef &c)
{
  string val = c.val;
  std::ranges::replace(val, '\'', '_');
  prtop("export const {} = {}n;\n", c.id, val);
}

void
GenTs::show_def(const Literal &l)
{
  // Parse #include directives and emit as comments
  // The user will need to add proper TS imports
  string_view lines = l.lines;
  while (!lines.empty()) {
    auto nl = lines.find('\n');
    string_view line =
        (nl == string_view::npos) ? lines : lines.substr(0, nl);
    lines = (nl == string_view::npos) ? string_view{} : lines.substr(nl + 1);

    if (line.find("#include") != string_view::npos) {
      // Extract filename from #include "foo.hh"
      auto q1 = line.find('"');
      auto q2 = line.find('"', q1 + 1);
      if (q1 != string_view::npos && q2 != string_view::npos) {
        string filename(line.substr(q1 + 1, q2 - q1 - 1));
        // Strip .hh extension
        if (filename.ends_with(".hh"))
          filename = filename.substr(0, filename.size() - 3);
        prtop("// import {{ ... }} from \"./{}.js\";\n", filename);
      }
    }
  }
}

void
GenTs::show_def(const Typedef &d)
{
  local_types_.insert(d.id);

  if (d.qual == Declaration::kVoid)
    return;

  // Handle inline type bodies (typedef enum { ... } Foo)
  if (!holds_alternative<string>(d.type)) {
    visit(
        [this, &d](const auto &spec) {
          using T = std::decay_t<decltype(spec)>;
          if constexpr (std::is_same_v<T, EnumBody>) {
            enum_types_.insert(d.id);
            gen_enum_type(spec, d.id);
          }
          else if constexpr (std::is_same_v<T, StructBody>) {
            gen_struct_type(spec, d.id);
            gen_struct_codec(spec, d.id);
          }
          else if constexpr (std::is_same_v<T, UnionBody>) {
            gen_union_type(spec, d.id);
            gen_union_codec(spec, d.id);
          }
          else {
            // string case handled above
          }
        },
        d.type);
    return;
  }

  string base = get<string>(d.type);

  switch (d.qual) {
  case Declaration::kScalar: {
    if (is_primitive_type(base)) {
      // Branded primitive type
      string ts_type = map_type(base);
      string factory = to_camel_case(d.id);

      prtop("export type {} = {} & {{ readonly __brand: \"{}\" }};\n", d.id,
            ts_type, d.id);
      prtop("export function {}(val: {}): {} {{\n", factory, ts_type, d.id);
      prtop("  return val as {};\n", d.id);
      prtop("}}\n");

      // Codec
      prbottom("\nexport const {0}: XdrCodec<{0}> = {{\n", d.id);
      prbottom("  encode(w, val) {{ w.{0}(val); }},\n",
               map_write_method(base));
      prbottom("  decode(r) {{ return {0}(r.{1}()); }},\n", factory,
               map_read_method(base));
      prbottom("}};\n");
    }
    else {
      // Alias of user type
      prtop("export type {} = {};\n", d.id, base);
      // Delegate codec
      prbottom("\nexport const {0}: XdrCodec<{0}> = {1} as XdrCodec<{0}>;\n",
               d.id, base);
    }
    break;
  }
  case Declaration::kArray: {
    if (base == "opaque") {
      // Fixed-size opaque: branded Uint8Array with length check
      string factory = to_camel_case(d.id);

      prtop("export type {} = Uint8Array & {{ readonly __brand: \"{}\" }};\n",
            d.id, d.id);
      prtop("export function {}(data: Uint8Array): {} {{\n", factory, d.id);
      prtop("  if (data.length !== {}) throw new Error(`{} must be exactly {} "
            "bytes, got ${{data.length}}`);\n",
            d.bound, d.id, d.bound);
      prtop("  return data as {};\n", d.id);
      prtop("}}\n");

      // Codec
      prbottom("\nexport const {0}: XdrCodec<{0}> = {{\n", d.id);
      prbottom("  encode(w, val) {{ w.writeFixedOpaque(val, {0}); }},\n",
               d.bound);
      prbottom("  decode(r) {{ return {0}(r.readFixedOpaque({1})); }},\n",
               factory, d.bound);
      prbottom("}};\n");
    }
    else {
      // Fixed-size array of user type
      prtop("export type {} = {}[];\n", d.id, map_type(base));
    }
    break;
  }
  case Declaration::kVec: {
    if (base == "opaque") {
      // Variable-length opaque: branded Uint8Array
      string factory = to_camel_case(d.id);
      string bound_str = map_bound(d.bound);

      prtop("export type {} = Uint8Array & {{ readonly __brand: \"{}\" }};\n",
            d.id, d.id);
      prtop("export function {}(data: Uint8Array): {} {{\n", factory, d.id);
      if (!d.bound.empty()) {
        prtop("  if (data.length > Number({})) throw new Error(`{} exceeds "
              "max length {}, got ${{data.length}}`);\n",
              d.bound, d.id, d.bound);
      }
      prtop("  return data as {};\n", d.id);
      prtop("}}\n");

      // Codec
      prbottom("\nexport const {0}: XdrCodec<{0}> = {{\n", d.id);
      prbottom("  encode(w, val) {{ w.writeVarOpaque(val, Number({0})); }},\n",
               bound_str);
      prbottom("  decode(r) {{ return {0}(r.readVarOpaque(Number({1}))); }},\n",
               factory, bound_str);
      prbottom("}};\n");
    }
    else {
      // Variable-length array of user type: Type[]
      prtop("export type {} = {}[];\n", d.id, map_type(base));

      // Vec codec using inline varArray-style
      prbottom("\nexport const {0}: XdrCodec<{0}> = {{\n", d.id);
      prbottom("  encode(w, val) {{\n");
      prbottom("    w.writeUint32(BigInt(val.length));\n");
      if (is_primitive_type(base)) {
        prbottom("    for (const _item of val) w.{0}(_item);\n",
                 map_write_method(base));
      }
      else {
        prbottom("    for (const _item of val) {0}.encode(w, _item);\n",
                 map_type(base));
      }
      prbottom("  }},\n");
      prbottom("  decode(r) {{\n");
      prbottom("    const _len = Number(r.readUint32());\n");
      if (is_primitive_type(base)) {
        prbottom("    return Array.from({{ length: _len }}, () => "
                 "r.{0}());\n",
                 map_read_method(base));
      }
      else {
        prbottom("    return Array.from({{ length: _len }}, () => "
                 "{0}.decode(r));\n",
                 map_type(base));
      }
      prbottom("  }},\n");
      prbottom("}};\n");
    }
    break;
  }
  case Declaration::kString: {
    // String typedef: branded string
    string factory = to_camel_case(d.id);
    string bound_str = map_bound(d.bound);

    prtop("export type {} = string & {{ readonly __brand: \"{}\" }};\n", d.id,
          d.id);
    prtop("export function {}(val: string): {} {{\n", factory, d.id);
    prtop("  return val as {};\n", d.id);
    prtop("}}\n");

    // Codec
    prbottom("\nexport const {0}: XdrCodec<{0}> = {{\n", d.id);
    prbottom("  encode(w, val) {{ w.writeString(val, Number({0})); }},\n",
             bound_str);
    prbottom("  decode(r) {{ return {0}(r.readString(Number({1}))); }},\n",
             factory, bound_str);
    prbottom("}};\n");
    break;
  }
  case Declaration::kPtr: {
    prtop("export type {} = {} | null;\n", d.id, map_type(base));

    // Nullable codec
    prbottom("\nexport const {0}: XdrCodec<{0}> = {{\n", d.id);
    prbottom("  encode(w, val) {{\n");
    prbottom("    if (val !== null) {{\n");
    prbottom("      w.writeUint32(1n);\n");
    if (is_primitive_type(base)) {
      prbottom("      w.{0}(val);\n", map_write_method(base));
    }
    else {
      prbottom("      {0}.encode(w, val);\n", map_type(base));
    }
    prbottom("    }} else {{\n");
    prbottom("      w.writeUint32(0n);\n");
    prbottom("    }}\n");
    prbottom("  }},\n");
    prbottom("  decode(r) {{\n");
    if (is_primitive_type(base)) {
      prbottom("    return Number(r.readUint32()) ? r.{0}() : null;\n",
               map_read_method(base));
    }
    else {
      prbottom("    return Number(r.readUint32()) ? {0}.decode(r) : null;\n",
               map_type(base));
    }
    prbottom("  }},\n");
    prbottom("}};\n");
    break;
  }
  case Declaration::kVoid:
    break;
  }
}

void
GenTs::show_def(const EnumDecl &e)
{
  local_types_.insert(e.id);
  enum_types_.insert(e.id);
  gen_enum_type(e.body, e.id);
}

void
GenTs::show_def(const StructDecl &s)
{
  local_types_.insert(s.id);
  gen_inline_types_struct(s.body, s.id);
  gen_struct_type(s.body, s.id);
  gen_struct_codec(s.body, s.id);
}

void
GenTs::show_def(const UnionDecl &u)
{
  local_types_.insert(u.id);
  gen_inline_types_union(u.body, u.id);
  gen_union_type(u.body, u.id);
  gen_union_codec(u.body, u.id);
}

// ---- Type generators ----

void
GenTs::gen_enum_type(const EnumBody &body, const string &name)
{
  prtop("export enum {} {{\n", name);
  ++indent_;
  for (const auto &cd : body) {
    prtop("{} = {},\n", cd.id, cd.val);
  }
  --indent_;
  prtop("}}\n");
}

void
GenTs::gen_struct_type(const StructBody &body, const string &name)
{
  prtop("export interface {} {{\n", name);
  ++indent_;
  for (const auto &d : body) {
    if (d.qual == Declaration::kVoid)
      continue;
    prtop("{}: {};\n", d.id, ts_field_type(d, name));
  }
  --indent_;
  prtop("}}\n");
}

void
GenTs::gen_union_type(const UnionBody &body, const string &name)
{
  bool is_enum = is_enum_discriminant(body.switch_type);

  prtop("export type {} =\n", name);
  ++indent_;

  for (const auto &arm : body.arms) {
    for (const auto &c : arm.cases) {
      string disc_val;
      if (is_enum) {
        disc_val = std::format("{}.{}", body.switch_type, c);
      }
      else if (body.switch_type == "bool") {
        disc_val = (c == "TRUE") ? "true" : "false";
      }
      else {
        disc_val = c;
      }

      prtop("| {{ {}: {}", body.switch_id, disc_val);
      if (arm.decl.qual != Declaration::kVoid) {
        prtop("; {}: {}", arm.decl.id, ts_field_type(arm.decl, name));
      }
      prtop(" }}\n");
    }

    if (arm.has_default) {
      string disc_type;
      if (is_enum) {
        disc_type = body.switch_type;
      }
      else if (body.switch_type == "bool") {
        disc_type = "boolean";
      }
      else {
        disc_type = "number";
      }

      prtop("| {{ {}: {}", body.switch_id, disc_type);
      if (arm.decl.qual != Declaration::kVoid) {
        prtop("; {}: {}", arm.decl.id, ts_field_type(arm.decl, name));
      }
      prtop(" }}\n");
    }
  }

  --indent_;
  prtop(";\n");
}

// ---- Inline type generators ----

void
GenTs::gen_inline_types_struct(const StructBody &body, const string &prefix)
{
  for (const auto &d : body) {
    if (holds_alternative<string>(d.type))
      continue;

    string inline_name = prefix + "_" + d.id;
    visit(
        [this, &inline_name](const auto &spec) {
          using T = std::decay_t<decltype(spec)>;
          if constexpr (std::is_same_v<T, EnumBody>) {
            enum_types_.insert(inline_name);
            gen_enum_type(spec, inline_name);
            prtop("\n");
          }
          else if constexpr (std::is_same_v<T, StructBody>) {
            gen_inline_types_struct(spec, inline_name);
            gen_struct_type(spec, inline_name);
            gen_struct_codec(spec, inline_name);
            prtop("\n");
          }
          else if constexpr (std::is_same_v<T, UnionBody>) {
            gen_inline_types_union(spec, inline_name);
            gen_union_type(spec, inline_name);
            gen_union_codec(spec, inline_name);
            prtop("\n");
          }
        },
        d.type);
    local_types_.insert(inline_name);
  }
}

void
GenTs::gen_inline_types_union(const UnionBody &body, const string &prefix)
{
  for (const auto &arm : body.arms) {
    const auto &d = arm.decl;
    if (holds_alternative<string>(d.type))
      continue;
    if (d.qual == Declaration::kVoid)
      continue;

    string inline_name = prefix + "_" + d.id;
    visit(
        [this, &inline_name](const auto &spec) {
          using T = std::decay_t<decltype(spec)>;
          if constexpr (std::is_same_v<T, EnumBody>) {
            enum_types_.insert(inline_name);
            gen_enum_type(spec, inline_name);
            prtop("\n");
          }
          else if constexpr (std::is_same_v<T, StructBody>) {
            gen_inline_types_struct(spec, inline_name);
            gen_struct_type(spec, inline_name);
            gen_struct_codec(spec, inline_name);
            prtop("\n");
          }
          else if constexpr (std::is_same_v<T, UnionBody>) {
            gen_inline_types_union(spec, inline_name);
            gen_union_type(spec, inline_name);
            gen_union_codec(spec, inline_name);
            prtop("\n");
          }
        },
        d.type);
    local_types_.insert(inline_name);
  }
}

// ---- Codec generators ----

void
GenTs::gen_struct_codec(const StructBody &body, const string &name)
{
  prbottom("\nexport const {0}: XdrCodec<{0}> = {{\n", name);

  // encode
  prbottom("  encode(w, val) {{\n");
  for (const auto &d : body) {
    if (d.qual == Declaration::kVoid)
      continue;
    encode_field(d, "val." + d.id, name, 4);
  }
  prbottom("  }},\n");

  // decode — always use sequential const declarations
  prbottom("  decode(r) {{\n");

  vector<string> field_names;
  for (const auto &d : body) {
    if (d.qual == Declaration::kVoid)
      continue;
    decode_field(d, name, 4);
    field_names.push_back(d.id);
  }
  prbottom("    return {{ ");
  Join comma(", ");
  for (const auto &f : field_names) {
    prbottom("{}{}", comma.get(), f);
  }
  prbottom(" }};\n");

  prbottom("  }},\n");
  prbottom("}};\n");
}

void
GenTs::gen_union_codec(const UnionBody &body, const string &name)
{
  bool is_enum = is_enum_discriminant(body.switch_type);

  prbottom("\nexport const {0}: XdrCodec<{0}> = {{\n", name);

  // encode
  prbottom("  encode(w, val) {{\n");
  prbottom("    w.writeInt32(BigInt(val.{0}));\n", body.switch_id);
  prbottom("    switch (val.{0}) {{\n", body.switch_id);

  for (const auto &arm : body.arms) {
    for (const auto &c : arm.cases) {
      if (is_enum) {
        prbottom("      case {0}.{1}:\n", body.switch_type, c);
      }
      else {
        prbottom("      case {0}:\n", c);
      }
    }
    if (arm.has_default) {
      prbottom("      default:\n");
    }

    if (arm.decl.qual != Declaration::kVoid) {
      encode_field(arm.decl, "val." + arm.decl.id, name, 8);
    }
    prbottom("        break;\n");
  }

  prbottom("    }}\n");
  prbottom("  }},\n");

  // decode
  prbottom("  decode(r) {{\n");
  prbottom("    const {0} = Number(r.readInt32());\n", body.switch_id);
  prbottom("    switch ({0}) {{\n", body.switch_id);

  bool has_default = false;
  for (const auto &arm : body.arms) {
    for (const auto &c : arm.cases) {
      if (is_enum) {
        prbottom("      case {0}.{1}:\n", body.switch_type, c);
      }
      else {
        prbottom("      case {0}:\n", c);
      }
    }
    if (arm.has_default) {
      has_default = true;
      prbottom("      default:\n");
    }

    if (arm.decl.qual != Declaration::kVoid) {
      decode_field(arm.decl, name, 8);
      prbottom("        return {{ {0}, {1} }};\n", body.switch_id,
               arm.decl.id);
    }
    else {
      prbottom("        return {{ {0} }} as {1};\n", body.switch_id, name);
    }
  }

  if (!has_default) {
    prbottom("      default:\n");
    prbottom("        throw new Error(`Unknown {0} discriminant: ${{{1}}}`);\n",
             name, body.switch_id);
  }

  prbottom("    }}\n");
  prbottom("  }},\n");
  prbottom("}};\n");
}

void
GenTs::gen_typedef_codec(const Typedef &d)
{
  // Handled inline in show_def(Typedef)
}

// ---- Field type mapping ----

string
GenTs::ts_field_type(const Declaration &d, const string &parent_name)
{
  string base = base_type_name(d);

  // For inline type bodies, use the generated name
  if (!holds_alternative<string>(d.type)) {
    base = parent_name + "_" + d.id;
  }

  switch (d.qual) {
  case Declaration::kScalar:
    return map_type(base);
  case Declaration::kArray:
    if (base == "opaque")
      return "Uint8Array";
    return map_type(base) + "[]";
  case Declaration::kVec:
    if (base == "opaque")
      return "Uint8Array";
    return map_type(base) + "[]";
  case Declaration::kString:
    return "string";
  case Declaration::kPtr:
    return map_type(base) + " | null";
  case Declaration::kVoid:
    return "void";
  }
  return "unknown";
}

// ---- Field encode/decode ----

void
GenTs::encode_field(const Declaration &d, const string &val_expr,
                    const string &parent_name, int sp)
{
  string base = base_type_name(d);
  if (!holds_alternative<string>(d.type))
    base = parent_name + "_" + d.id;

  string indent(sp, ' ');

  switch (d.qual) {
  case Declaration::kScalar:
    if (is_primitive_type(base)) {
      bottom_ += std::format("{}w.{}({});\n", indent, map_write_method(base),
                             val_expr);
    }
    else {
      bottom_ +=
          std::format("{}{}.encode(w, {});\n", indent, base, val_expr);
    }
    break;

  case Declaration::kArray:
    if (base == "opaque") {
      bottom_ += std::format("{}w.writeFixedOpaque({}, {});\n", indent,
                             val_expr, d.bound);
    }
    else {
      if (is_primitive_type(base)) {
        bottom_ += std::format("{}for (const _item of {}) w.{}(_item);\n",
                               indent, val_expr, map_write_method(base));
      }
      else {
        bottom_ +=
            std::format("{}for (const _item of {}) {}.encode(w, _item);\n",
                        indent, val_expr, map_type(base));
      }
    }
    break;

  case Declaration::kVec:
    if (base == "opaque") {
      bottom_ += std::format("{}w.writeVarOpaque({}, Number({}));\n", indent,
                             val_expr, map_bound(d.bound));
    }
    else {
      bottom_ += std::format("{}w.writeUint32(BigInt({}.length));\n", indent,
                             val_expr);
      if (is_primitive_type(base)) {
        bottom_ += std::format("{}for (const _item of {}) w.{}(_item);\n",
                               indent, val_expr, map_write_method(base));
      }
      else {
        bottom_ +=
            std::format("{}for (const _item of {}) {}.encode(w, _item);\n",
                        indent, val_expr, map_type(base));
      }
    }
    break;

  case Declaration::kString:
    bottom_ += std::format("{}w.writeString({}, Number({}));\n", indent,
                           val_expr, map_bound(d.bound));
    break;

  case Declaration::kPtr:
    bottom_ += std::format("{}if ({} !== null) {{\n", indent, val_expr);
    bottom_ += std::format("{}  w.writeUint32(1n);\n", indent);
    if (is_primitive_type(base)) {
      bottom_ += std::format("{}  w.{}({});\n", indent,
                             map_write_method(base), val_expr);
    }
    else {
      bottom_ +=
          std::format("{}  {}.encode(w, {});\n", indent, base, val_expr);
    }
    bottom_ += std::format("{}}} else {{\n", indent);
    bottom_ += std::format("{}  w.writeUint32(0n);\n", indent);
    bottom_ += std::format("{}}}\n", indent);
    break;

  case Declaration::kVoid:
    break;
  }
}

string
GenTs::decode_field(const Declaration &d, const string &parent_name,
                    int sp)
{
  string base = base_type_name(d);
  if (!holds_alternative<string>(d.type))
    base = parent_name + "_" + d.id;

  string indent(sp, ' ');

  switch (d.qual) {
  case Declaration::kScalar:
    if (is_primitive_type(base)) {
      bottom_ += std::format("{}const {} = r.{}();\n", indent, d.id,
                             map_read_method(base));
    }
    else {
      bottom_ += std::format("{}const {} = {}.decode(r);\n", indent, d.id,
                             base);
    }
    return d.id;

  case Declaration::kArray:
    if (base == "opaque") {
      bottom_ += std::format("{}const {} = r.readFixedOpaque({});\n", indent,
                             d.id, d.bound);
    }
    else {
      if (is_primitive_type(base)) {
        bottom_ += std::format(
            "{}const {} = Array.from({{ length: {} }}, () => r.{}());\n",
            indent, d.id, d.bound, map_read_method(base));
      }
      else {
        bottom_ += std::format(
            "{}const {} = Array.from({{ length: {} }}, () => {}.decode(r));\n",
            indent, d.id, d.bound, map_type(base));
      }
    }
    return d.id;

  case Declaration::kVec:
    if (base == "opaque") {
      bottom_ += std::format("{}const {} = r.readVarOpaque(Number({}));\n",
                             indent, d.id, map_bound(d.bound));
    }
    else {
      string len_var = "_" + d.id + "_len";
      bottom_ += std::format("{}const {} = Number(r.readUint32());\n", indent,
                             len_var);
      if (is_primitive_type(base)) {
        bottom_ += std::format(
            "{}const {} = Array.from({{ length: {} }}, () => r.{}());\n",
            indent, d.id, len_var, map_read_method(base));
      }
      else {
        bottom_ += std::format(
            "{}const {} = Array.from({{ length: {} }}, () => {}.decode(r));\n",
            indent, d.id, len_var, map_type(base));
      }
    }
    return d.id;

  case Declaration::kString:
    bottom_ += std::format("{}const {} = r.readString(Number({}));\n", indent,
                           d.id, map_bound(d.bound));
    return d.id;

  case Declaration::kPtr:
    if (is_primitive_type(base)) {
      bottom_ += std::format(
          "{}const {} = Number(r.readUint32()) ? r.{}() : null;\n", indent,
          d.id, map_read_method(base));
    }
    else {
      bottom_ += std::format(
          "{}const {} = Number(r.readUint32()) ? {}.decode(r) : null;\n",
          indent, d.id, base);
    }
    return d.id;

  case Declaration::kVoid:
    return {};
  }
  return {};
}

// ---- Entry point ----

std::string
gen_ts(vector<Definition> *defs)
{
  GenTs gs;

  string preamble = "// -*- TypeScript -*-\n"
                    "// Generated Code: DO NOT CHECK IN\n"
                    "\n"
                    "// Generated from x23c commit: " +
                    x23::git_commit_hash() +
                    "\n\n"
                    "import type { XdrCodec } from \"./xdr_codec.js\";\n";

  gs.process(*defs);

  return preamble + gs.top_ +
         "\n//\n// XDR Codecs below this point\n//\n" + gs.bottom_ + "\n";
}
