#include <algorithm>
#include <cctype>
#include <cstddef>
#include <format>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include "x23c/defs.h"

#include "x23c/config.h"

struct GenTs {
  string top_;
  string bottom_;
  string program_meta_; // RPC program metadata output
  std::size_t indent_{};
  vector<string> codec_names_; // collect codec entries for namespace
  bool has_programs_{};
  std::set<string> primitive_codecs_; // primitive codecs needed in imports
  std::set<string> emitted_args_;     // dedup _args structs across all ProgDefs

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
  void show_def(const OpenNS & /*unused*/) {}
  void show_def(const CloseNS & /*unused*/) {}
  void show_def(const Literal & /*unused*/) {}
  void show_def(const ProgDef &prog);

  // Codec reference for a type in program metadata context.
  // VoidCodec for void, _TypeName for local types, proto.codec.X for external.
  string proc_codec_ref(const string &type);

  // Type generators
  void gen_enum_type(const EnumBody &body, const string &name);
  void gen_struct_type(const StructBody &body, const string &name);
  void gen_union_type(const UnionBody &body, const string &name);

  // Generate inline types from anonymous struct/enum/union bodies
  void gen_inline_type(const Declaration &d, const string &prefix);
  void gen_inline_types_struct(const StructBody &body, const string &prefix);
  void gen_inline_types_union(const UnionBody &body, const string &prefix);

  // Codec generators
  void gen_enum_codec(const string &name);
  void gen_struct_codec(const StructBody &body, const string &name);
  void gen_union_codec(const UnionBody &body, const string &name);

  // Encode a single field into bottom_
  void encode_field(const Declaration &d, const string &val_expr,
                    const string &parent_name, int sp);
  // Decode a single field, appending statements to bottom_
  void decode_field(const Declaration &d, const string &parent_name, int sp);

  // TypeScript type for a struct/union field
  static string ts_field_type(const Declaration &d, const string &parent_name);

  // Helpers
  static string map_type(const string &t);
  static string map_write_method(const string &t);
  static string map_read_method(const string &t);
  static string map_bound(const string &b);
  static string map_bound_type(const string &b);
  static string map_bound_runtime(const string &b);
  static bool const_fits_u32(const string &val);
  static string xdr_doc(const Declaration &d, const string &base,
                        bool is_typedef);
  static bool is_primitive_type(const string &t);
  static string to_camel_case(const string &name);

  // Return "proto.X" for user types, mapped name for primitives.
  // Used in type positions.
  static string ts_ref(const string &t)
  {
    string mapped = map_type(t);
    if (mapped != t)
      return mapped; // primitive → "bigint", "boolean", etc.
    return "proto." + t;
  }

  // Return the codec value reference for a type.
  // All codecs live in proto.codec.X (the codec namespace object).
  static string codec_ref(const string &t)
  {
    if (is_primitive_type(t))
      throw std::runtime_error(
          "codec_ref called on primitive type \"" + t +
          "\"; use proc_codec_ref for compound codecs that wrap primitives");
    return "proto.codec." + t;
  }

  // In XDR, union discriminants are either primitives (int/uint/bool) or
  // enum type names. Any non-primitive, non-bool name is an enum.
  static bool is_enum_discriminant(const string &switch_type)
  {
    return !is_primitive_type(switch_type) && switch_type != "bool";
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
    for (size_t i = 0; i < upper_run; ++i)
      result[i] = static_cast<char>(std::tolower(result[i]));
  }
  else {
    for (size_t i = 0; i < upper_run - 1; ++i)
      result[i] = static_cast<char>(std::tolower(result[i]));
  }
  return result;
}

string
GenTs::proc_codec_ref(const string &type)
{
  if (type == "void")
    return "VoidCodec";

  // Primitive types → named codecs from @groundhogchain/xdr
  static const std::map<string, string> primitive_codec_map{
      {"bool", "BoolCodec"},
      {"int", "Int32Codec"},
      {"unsigned int", "Uint32Codec"},
      {"hyper", "Int64Codec"},
      {"unsigned hyper", "Uint64Codec"},
  };
  if (auto it = primitive_codec_map.find(type);
      it != primitive_codec_map.end()) {
    primitive_codecs_.insert(it->second);
    return it->second;
  }

  // Local codec — reference the private const directly.
  // NOTE: This relies on XDR convention that type definitions precede program
  // definitions.  Types declared after a program block will not yet be in
  // codec_names_ and will fall through to the external-reference path below.
  if (std::ranges::find(codec_names_, type) != codec_names_.end())
    return "_" + type;
  // External type — reference via proto.codec with cast
  return std::format("proto.codec.{0} as XdrCodec<proto.{0}>", type);
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
      {"int", "bigint"},    {"unsigned int", "bigint"},
      {"hyper", "bigint"},  {"unsigned hyper", "bigint"},
      {"bool", "boolean"},  {"opaque", "Uint8Array"},
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
      {"int", "writeInt32"},   {"unsigned int", "writeUint32"},
      {"hyper", "writeInt64"}, {"unsigned hyper", "writeUint64"},
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
      {"int", "readInt32"},   {"unsigned int", "readUint32"},
      {"hyper", "readInt64"}, {"unsigned hyper", "readUint64"},
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
  // Numeric literals (e.g. "32", "0xFFFFFFFF") pass through.
  // Named constants (e.g. "kMaxSigners") need the proto. prefix.
  if (std::isdigit(b[0]))
    return b;
  return "proto." + b;
}

// Bound in a TYPE position (FixedBytes<N>, VarArray<T, N>, ...). Numeric
// literals pass through; named constants resolve via their @hidden same-name
// type companion on the proto barrel. Empty (unbounded vec/string) returns
// "" — callers fall back to the un-annotated type.
string
GenTs::map_bound_type(const string &b)
{
  if (b.empty())
    return {};
  if (std::isdigit(b[0]))
    return b;
  return "proto." + b;
}

// Bound as a runtime number expression. Named constants are bigints and need
// Number(); numeric literals pass through untouched.
string
GenTs::map_bound_runtime(const string &b)
{
  if (b.empty() || std::isdigit(b[0]))
    return map_bound(b);
  return "Number(proto." + b + ")";
}

// Whether a const's value fits in u32, and so may legally appear as an XDR
// array/vec bound. Only these get a type-level companion: number literals
// above 2^53 would silently lose precision.
bool
GenTs::const_fits_u32(const string &val)
{
  string v = val;
  std::erase(v, '\'');
  try {
    size_t pos = 0;
    unsigned long long n = std::stoull(v, &pos, 0);
    return pos == v.size() && n <= 0xFFFFFFFFULL;
  } catch (const std::exception &) {
    return false; // negative, overflowing, or malformed
  }
}

// One-line JSDoc echoing the XDR declaration for length-carrying decls
// (fixed arrays, bounded vecs/strings). Returns "" when the decl carries no
// length information worth surfacing.
string
GenTs::xdr_doc(const Declaration &d, const string &base, bool is_typedef)
{
  string decl;
  string suffix;
  bool is_opaque = (base == "opaque");
  string unit =
      is_opaque || d.qual == Declaration::kString ? "bytes" : "elements";
  // Named bounds link to the const's doc page; literal bounds inline.
  string linked_bound = d.bound;
  if (!d.bound.empty() && !std::isdigit(d.bound[0]))
    linked_bound = "{@link " + d.bound + "}";

  switch (d.qual) {
  case Declaration::kArray:
    decl = std::format("{} {}[{}]", base, d.id, d.bound);
    suffix = std::format(" — exactly {} {}", linked_bound, unit);
    break;
  case Declaration::kVec:
    decl = std::format("{} {}<{}>", base, d.id, d.bound);
    if (!d.bound.empty())
      suffix = std::format(" — max {} {}", linked_bound, unit);
    break;
  case Declaration::kString:
    decl = std::format("string {}<{}>", d.id, d.bound);
    if (!d.bound.empty())
      suffix = std::format(" — max {} {}", linked_bound, unit);
    break;
  default:
    return {};
  }

  return std::format("/** XDR: `{}{}`{}. */", is_typedef ? "typedef " : "",
                     decl, suffix);
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

  // Type-level companion: a same-name type alias carrying the value as a
  // numeric literal type, so the const can parameterize array-bound types
  // (e.g. FixedArray<T, proto.kMaxSigners>). Emitted only for values that
  // fit in u32 — XDR bounds are u32, and number literals at or above 2^53
  // silently lose precision. @hidden keeps the alias out of TypeDoc output.
  if (const_fits_u32(c.val)) {
    prtop("/**\n");
    prtop(" * Type-level value of {{@link {0}}}, for use in array-bound "
          "positions.\n",
          c.id);
    prtop(" * @hidden\n");
    prtop(" */\n");
    prtop("export type {} = {};\n", c.id, val);
  }
}

void
GenTs::show_def(const Typedef &d)
{
  if (d.qual == Declaration::kVoid)
    return;

  // Handle inline type bodies (typedef enum { ... } Foo)
  if (!holds_alternative<string>(d.type)) {
    visit(
        [this, &d](const auto &spec) {
          using T = std::decay_t<decltype(spec)>;
          if constexpr (std::is_same_v<T, EnumBody>) {
            gen_enum_type(spec, d.id);
            gen_enum_codec(d.id);
          }
          else if constexpr (std::is_same_v<T, StructBody>) {
            gen_struct_type(spec, d.id);
            gen_struct_codec(spec, d.id);
          }
          else if constexpr (std::is_same_v<T, UnionBody>) {
            gen_union_type(spec, d.id);
            gen_union_codec(spec, d.id);
          }
        },
        d.type);
    return;
  }

  string base = get<string>(d.type);

  switch (d.qual) {
  case Declaration::kScalar:
  {
    if (is_primitive_type(base)) {
      // Branded primitive type
      string ts_type = map_type(base);
      string factory = to_camel_case(d.id);

      prtop("export type {} = {} & {{ readonly __brand: {{ readonly {}: true "
            "}} }};\n",
            d.id, ts_type, d.id);
      prtop("export function {}(val: {}): {} {{\n", factory, ts_type, d.id);
      prtop("  return val as {};\n", d.id);
      prtop("}}\n");

      // Codec
      codec_names_.push_back(d.id);
      prbottom("\nconst _{0}: XdrCodec<{0}> = {{\n", d.id);
      prbottom("  encode(w, val) {{ w.{0}(val); }},\n", map_write_method(base));
      prbottom("  decode(r) {{ return {0}(r.{1}()); }},\n", factory,
               map_read_method(base));
      prbottom("}};\n");
    }
    else {
      // Branded alias of user type — intersect with base type, delegate
      // codec.  The base may itself be a nullable (pointer) typedef, possibly
      // from another file, so brand only the non-null part and carry the
      // base's null arm (Extract yields never for non-nullable bases).
      string tref = ts_ref(base);
      string cref = codec_ref(base);
      string factory = to_camel_case(d.id);
      prtop("export type {} = (NonNullable<{}> & {{ readonly __brand: {{ "
            "readonly {}: true }} }}) | Extract<{}, null>;\n",
            d.id, tref, d.id, tref);
      prtop("export function {}(val: {}): {} {{\n", factory, tref, d.id);
      prtop("  return val as {};\n", d.id);
      prtop("}}\n");

      codec_names_.push_back(d.id);
      prbottom("\nconst _{0}: XdrCodec<{0}> = {{\n", d.id);
      prbottom("  encode(w, val) {{ {0}.encode(w, val); }},\n", cref);
      prbottom("  decode(r) {{ return {0}({1}.decode(r)); }},\n", factory,
               cref);
      prbottom("}};\n");
    }
    break;
  }
  case Declaration::kArray:
  {
    string bound_t = map_bound_type(d.bound);
    string bound_rt = map_bound_runtime(d.bound);
    if (base == "opaque") {
      // Fixed-size opaque: branded, length-typed Uint8Array with length check
      string factory = to_camel_case(d.id);

      prtop("{}\n", xdr_doc(d, base, true));
      prtop("export type {} = FixedBytes<{}> & {{ readonly __brand: {{ "
            "readonly {}: true }} }};\n",
            d.id, bound_t, d.id);
      prtop("export function {}(data: Uint8Array): {} {{\n", factory, d.id);
      prtop("  if (data.length !== {}) throw new Error(`{} must be exactly {} "
            "bytes, got ${{data.length}}`);\n",
            bound_rt, d.id, d.bound);
      prtop("  return data as {};\n", d.id);
      prtop("}}\n");

      codec_names_.push_back(d.id);
      prbottom("\nconst _{0}: XdrCodec<{0}> = {{\n", d.id);
      prbottom("  encode(w, val) {{ w.writeFixedOpaque(val, {0}); }},\n",
               bound_rt);
      prbottom("  decode(r) {{ return {0}(r.readFixedOpaque({1})); }},\n",
               factory, bound_rt);
      prbottom("}};\n");
    }
    else {
      // Fixed-size array of user type — branded, length-typed
      string tref = ts_ref(base);
      string cref = codec_ref(base);
      string factory = to_camel_case(d.id);
      prtop("{}\n", xdr_doc(d, base, true));
      prtop("export type {} = FixedArray<{}, {}> & {{ readonly __brand: {{ "
            "readonly {}: true }} }};\n",
            d.id, tref, bound_t, d.id);
      prtop("export function {}(val: {}[]): {} {{\n", factory, tref, d.id);
      prtop("  if (val.length !== {}) throw new Error(`{} must have exactly "
            "{} elements, got ${{val.length}}`);\n",
            bound_rt, d.id, d.bound);
      prtop("  return val as {};\n", d.id);
      prtop("}}\n");

      codec_names_.push_back(d.id);
      prbottom("\nconst _{0} = FixedArrayCodec(() => {1}, Number({2})) as "
               "XdrCodec<{0}>;\n",
               d.id, cref, map_bound(d.bound));
    }
    break;
  }
  case Declaration::kVec:
  {
    string bound_t = map_bound_type(d.bound);
    if (base == "opaque") {
      // Variable-length opaque: branded Uint8Array carrying its max_length
      string factory = to_camel_case(d.id);
      string bound_str = map_bound(d.bound);
      string base_ts =
          bound_t.empty() ? "Uint8Array" : std::format("VarBytes<{}>", bound_t);

      prtop("{}\n", xdr_doc(d, base, true));
      prtop("export type {} = {} & {{ readonly __brand: {{ readonly "
            "{}: true }} }};\n",
            d.id, base_ts, d.id);
      prtop("export function {}(data: Uint8Array): {} {{\n", factory, d.id);
      if (!d.bound.empty()) {
        prtop("  if (data.length > {}) throw new Error(`{} exceeds "
              "max length {}, got ${{data.length}}`);\n",
              map_bound_runtime(d.bound), d.id, d.bound);
      }
      prtop("  return data as {};\n", d.id);
      prtop("}}\n");

      codec_names_.push_back(d.id);
      prbottom("\nconst _{0}: XdrCodec<{0}> = {{\n", d.id);
      prbottom("  encode(w, val) {{ w.writeVarOpaque(val, Number({0})); }},\n",
               bound_str);
      prbottom("  decode(r) {{ return {0}(r.readVarOpaque(Number({1}))); }},\n",
               factory, bound_str);
      prbottom("}};\n");
    }
    else {
      // Variable-length array of user type — branded, carrying its max_length
      string tref = ts_ref(base);
      string cref = codec_ref(base);
      string factory = to_camel_case(d.id);
      string base_ts = bound_t.empty()
                           ? tref + "[]"
                           : std::format("VarArray<{}, {}>", tref, bound_t);
      prtop("{}\n", xdr_doc(d, base, true));
      prtop("export type {} = {} & {{ readonly __brand: {{ readonly {}: "
            "true }} }};\n",
            d.id, base_ts, d.id);
      prtop("export function {}(val: {}[]): {} {{\n", factory, tref, d.id);
      if (!d.bound.empty()) {
        prtop("  if (val.length > {}) throw new Error(`{} exceeds max "
              "length {}, got ${{val.length}}`);\n",
              map_bound_runtime(d.bound), d.id, d.bound);
      }
      prtop("  return val as {};\n", d.id);
      prtop("}}\n");

      codec_names_.push_back(d.id);
      prbottom("\nconst _{0} = VarArrayCodec(() => {1}, Number({2})) as "
               "XdrCodec<{0}>;\n",
               d.id, cref, map_bound(d.bound));
    }
    break;
  }
  case Declaration::kString:
  {
    string factory = to_camel_case(d.id);
    string bound_str = map_bound(d.bound);
    string bound_t = map_bound_type(d.bound);
    string base_ts =
        bound_t.empty() ? "string" : std::format("VarString<{}>", bound_t);

    prtop("{}\n", xdr_doc(d, "string", true));
    prtop("export type {} = {} & {{ readonly __brand: {{ readonly {}: "
          "true }} }};\n",
          d.id, base_ts, d.id);
    prtop("export function {}(val: string): {} {{\n", factory, d.id);
    prtop("  return val as {};\n", d.id);
    prtop("}}\n");

    codec_names_.push_back(d.id);
    prbottom("\nconst _{0}: XdrCodec<{0}> = {{\n", d.id);
    prbottom("  encode(w, val) {{ w.writeString(val, Number({0})); }},\n",
             bound_str);
    prbottom("  decode(r) {{ return {0}(r.readString(Number({1}))); }},\n",
             factory, bound_str);
    prbottom("}};\n");
    break;
  }
  case Declaration::kPtr:
  {
    // Branded nullable
    string tref = ts_ref(base);
    string cref = proc_codec_ref(base);
    string factory = to_camel_case(d.id);
    prtop("export type {} = ({} & {{ readonly __brand: {{ readonly {}: true "
          "}} }}) | null;\n",
          d.id, tref, d.id);
    prtop("export function {}(val: {} | null): {} {{\n", factory, tref, d.id);
    prtop("  return val as {};\n", d.id);
    prtop("}}\n");

    codec_names_.push_back(d.id);
    prbottom("\nconst _{0} = OptionCodec(() => {1}) as XdrCodec<{0}>;\n", d.id,
             cref);
    break;
  }
  case Declaration::kVoid:
    break;
  }
}

void
GenTs::show_def(const EnumDecl &e)
{
  gen_enum_type(e.body, e.id);
  gen_enum_codec(e.id);
}

void
GenTs::show_def(const StructDecl &s)
{
  gen_inline_types_struct(s.body, s.id);
  gen_struct_type(s.body, s.id);
  gen_struct_codec(s.body, s.id);
}

void
GenTs::show_def(const UnionDecl &u)
{
  gen_inline_types_union(u.body, u.id);
  gen_union_type(u.body, u.id);
  gen_union_codec(u.body, u.id);
}

void
GenTs::show_def(const ProgDef &prog)
{
  bool multi_ver = prog.versions.size() > 1;

  for (const VersDef &vers : prog.versions) {
    // Build args struct name, qualified by version for multi-version programs
    auto args_struct_name = [&](const string &proc_id) -> string {
      return multi_ver ? vers.id + "_" + proc_id + "_args" : proc_id + "_args";
    };

    // Generate multi-arg request structs for procedures with >1 arg.
    // Save codec_names_ size so args structs don't leak into the public
    // codec namespace — they are internal to the RpcProgramMeta block.
    auto codec_names_before = codec_names_.size();
    for (const auto &proc : vers.procs) {
      vector<string> real_args;
      for (const auto &a : proc.arg_types)
        if (a != "void")
          real_args.push_back(a);
      if (real_args.size() <= 1)
        continue;

      string args_name = args_struct_name(proc.id);
      if (!emitted_args_.insert(args_name).second)
        continue; // already generated

      // Build a StructBody with positional field names (arg0, arg1, ...)
      StructBody args_body;
      for (size_t i = 0; i < real_args.size(); ++i) {
        Declaration d;
        d.type = TypeSpecifier{real_args[i]};
        d.id = "arg" + std::to_string(i);
        d.qual = Declaration::kScalar;
        args_body.push_back(d);
      }

      prtop("\n");
      gen_struct_type(args_body, args_name);
      gen_struct_codec(args_body, args_name);
    }
    codec_names_.resize(codec_names_before);

    has_programs_ = true;

    // Emit RpcProgramMeta export
    string export_name = multi_ver ? vers.id + "Program" : prog.id + "Program";
    program_meta_ += std::format("\nexport const {0}: RpcProgramMeta = {{\n"
                                 "  programId: {1},\n"
                                 "  version: {2},\n"
                                 "  methods: {{\n",
                                 export_name, prog.val, vers.val);

    for (const auto &proc : vers.procs) {
      // Request codec
      string req_codec;
      vector<string> real_args;
      for (const auto &a : proc.arg_types)
        if (a != "void")
          real_args.push_back(a);

      if (real_args.empty()) {
        req_codec = "VoidCodec";
      }
      else if (real_args.size() == 1) {
        req_codec = proc_codec_ref(real_args[0]);
      }
      else {
        req_codec = "_" + args_struct_name(proc.id);
      }

      // Response codec
      string resp_codec = proc_codec_ref(proc.return_type);

      program_meta_ += std::format("    {0}: {{ proc: {1}, request: () => {2}, "
                                   "response: () => {3} }},\n",
                                   proc.id, proc.val, req_codec, resp_codec);
    }

    program_meta_ += "  },\n"
                     "};\n";
  }
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
    if (d.qual == Declaration::kArray || d.qual == Declaration::kVec ||
        d.qual == Declaration::kString) {
      string base = base_type_name(d);
      if (!holds_alternative<string>(d.type))
        base = name + "_" + d.id;
      prtop("{}\n", xdr_doc(d, base, false));
    }
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
        disc_val = std::format("proto.{}.{}", body.switch_type, c);
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
      // Default arm: { disc: number } or { disc: number; data: T }
      // Overlaps with specific variants, but needed for forward compat.
      prtop("| {{ {}: number", body.switch_id);
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
GenTs::gen_inline_type(const Declaration &d, const string &prefix)
{
  if (holds_alternative<string>(d.type) || d.qual == Declaration::kVoid)
    return;

  string name = prefix + "_" + d.id;
  visit(
      [this, &name](const auto &spec) {
        using T = std::decay_t<decltype(spec)>;
        if constexpr (std::is_same_v<T, EnumBody>) {
          gen_enum_type(spec, name);
          gen_enum_codec(name);
          prtop("\n");
        }
        else if constexpr (std::is_same_v<T, StructBody>) {
          gen_inline_types_struct(spec, name);
          gen_struct_type(spec, name);
          gen_struct_codec(spec, name);
          prtop("\n");
        }
        else if constexpr (std::is_same_v<T, UnionBody>) {
          gen_inline_types_union(spec, name);
          gen_union_type(spec, name);
          gen_union_codec(spec, name);
          prtop("\n");
        }
      },
      d.type);
}

void
GenTs::gen_inline_types_struct(const StructBody &body, const string &prefix)
{
  for (const auto &d : body)
    gen_inline_type(d, prefix);
}

void
GenTs::gen_inline_types_union(const UnionBody &body, const string &prefix)
{
  for (const auto &arm : body.arms)
    gen_inline_type(arm.decl, prefix);
}

// ---- Codec generators ----

void
GenTs::gen_enum_codec(const string &name)
{
  // XDR enums are always serialized as int32 on the wire.
  codec_names_.push_back(name);
  prbottom("\nconst _{0}: XdrCodec<{0}> = {{\n", name);
  prbottom("  encode(w, val) {{ w.writeInt32(BigInt(val)); }},\n");
  prbottom("  decode(r) {{ return Number(r.readInt32()) as {0}; }},\n", name);
  prbottom("}};\n");
}

void
GenTs::gen_struct_codec(const StructBody &body, const string &name)
{
  codec_names_.push_back(name);
  prbottom("\nconst _{0}: XdrCodec<{0}> = {{\n", name);

  // encode
  prbottom("  encode(w, val) {{\n");
  for (const auto &d : body) {
    if (d.qual == Declaration::kVoid)
      continue;
    encode_field(d, "val." + d.id, name, 4);
  }
  prbottom("  }},\n");

  // decode
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
  bool is_bool = (body.switch_type == "bool");

  auto case_label = [&](const string &c) -> string {
    if (is_enum)
      return std::format("proto.{}.{}", body.switch_type, c);
    if (is_bool)
      return (c == "TRUE") ? "true" : "false";
    return c;
  };

  // Check if union has a default arm
  bool has_default_arm = false;
  for (const auto &arm : body.arms)
    if (arm.has_default) {
      has_default_arm = true;
      break;
    }

  codec_names_.push_back(name);
  prbottom("\nconst _{0}: XdrCodec<{0}> = {{\n", name);

  // ---- encode ----
  prbottom("  encode(w, val) {{\n");
  if (is_bool)
    prbottom("    w.writeBool(val.{0});\n", body.switch_id);
  else if (is_enum)
    prbottom("    w.writeInt32(BigInt(val.{0}));\n", body.switch_id);
  else
    prbottom("    w.{0}(BigInt(val.{1}));\n",
             map_write_method(body.switch_type), body.switch_id);

  prbottom("    switch (val.{0}) {{\n", body.switch_id);

  for (const auto &arm : body.arms) {
    if (arm.has_default) {
      // Default arm: only emit if it has data to encode
      if (arm.decl.qual != Declaration::kVoid) {
        string ftype = ts_field_type(arm.decl, name);
        prbottom("      default: {{\n");
        prbottom("        const v = val as {{ {0}: number; {1}: {2} }};\n",
                 body.switch_id, arm.decl.id, ftype);
        prbottom("        if (v.{0} === undefined) {{\n", arm.decl.id);
        prbottom("          throw new Error(\"{0} encode: default arm but {1} "
                 "is undefined\");\n",
                 name, arm.decl.id);
        prbottom("        }}\n");
        encode_field(arm.decl, "v." + arm.decl.id, name, 8);
        prbottom("        break;\n");
        prbottom("      }}\n");
      }
      // default: void — nothing to encode, omit from switch
      continue;
    }

    for (const auto &c : arm.cases)
      prbottom("      case {0}:\n", case_label(c));

    if (arm.decl.qual != Declaration::kVoid) {
      if (has_default_arm) {
        // { disc: number } overlaps — cast to inline variant type
        string ftype = ts_field_type(arm.decl, name);
        prbottom("      {{\n");
        prbottom("        const v = val as {{ {0}: number; {1}: {2} }};\n",
                 body.switch_id, arm.decl.id, ftype);
        prbottom("        if (v.{0} === undefined) {{\n", arm.decl.id);
        prbottom(
            "          throw new Error(\"{0} encode: {1} is undefined\");\n",
            name, arm.decl.id);
        prbottom("        }}\n");
        encode_field(arm.decl, "v." + arm.decl.id, name, 8);
        prbottom("        break;\n");
        prbottom("      }}\n");
      }
      else {
        encode_field(arm.decl, "val." + arm.decl.id, name, 8);
        prbottom("        break;\n");
      }
    }
    else {
      prbottom("        break;\n");
    }
  }

  prbottom("    }}\n");
  prbottom("  }},\n");

  // ---- decode ----
  prbottom("  decode(r) {{\n");
  if (is_bool)
    prbottom("    const {0} = r.readBool();\n", body.switch_id);
  else if (is_enum)
    prbottom("    const {0} = Number(r.readInt32());\n", body.switch_id);
  else
    prbottom("    const {0} = Number(r.{1}());\n", body.switch_id,
             map_read_method(body.switch_type));

  prbottom("    switch ({0}) {{\n", body.switch_id);

  bool has_default = false;
  for (const auto &arm : body.arms) {
    if (arm.has_default) {
      has_default = true;
      prbottom("      default:\n");
    }
    else {
      for (const auto &c : arm.cases)
        prbottom("      case {0}:\n", case_label(c));
    }

    if (arm.decl.qual != Declaration::kVoid) {
      decode_field(arm.decl, name, 8);
      if (has_default_arm && arm.cases.size() > 1) {
        // Multiple case labels narrow the discriminant to a union of literals,
        // which tsc cannot match against a single variant while the catch-all
        // { disc: number } variant also accepts those literals — cast instead.
        prbottom("        return {{ {0}, {1} }} as {2};\n", body.switch_id,
                 arm.decl.id, name);
      }
      else {
        prbottom("        return {{ {0}, {1} }};\n", body.switch_id,
                 arm.decl.id);
      }
    }
    else if (!has_default_arm) {
      // No catch-all variant — need cast for void arms
      prbottom("        return {{ {0} }} as {1};\n", body.switch_id, name);
    }
    else {
      // Catch-all { disc: number } absorbs — no cast needed
      prbottom("        return {{ {0} }};\n", body.switch_id);
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

// ---- Field type mapping ----

string
GenTs::ts_field_type(const Declaration &d, const string &parent_name)
{
  string base = base_type_name(d);

  // For inline type bodies, use the generated name
  if (!holds_alternative<string>(d.type))
    base = parent_name + "_" + d.id;

  string bound_t = map_bound_type(d.bound);

  switch (d.qual) {
  case Declaration::kScalar:
    return ts_ref(base);
  case Declaration::kArray:
    if (base == "opaque")
      return std::format("FixedBytes<{}>", bound_t);
    return std::format("FixedArray<{}, {}>", ts_ref(base), bound_t);
  case Declaration::kVec:
    if (base == "opaque")
      return bound_t.empty() ? "Uint8Array"
                             : std::format("VarBytes<{}>", bound_t);
    return bound_t.empty()
               ? ts_ref(base) + "[]"
               : std::format("VarArray<{}, {}>", ts_ref(base), bound_t);
  case Declaration::kString:
    return bound_t.empty() ? "string" : std::format("VarString<{}>", bound_t);
  case Declaration::kPtr:
    return ts_ref(base) + " | null";
  case Declaration::kVoid:
    return "void";
  }
  throw std::runtime_error(
      std::format("Unknown declaration qual: {}", std::to_underlying(d.qual)));
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
  auto ref = [&] { return codec_ref(base); };

  switch (d.qual) {
  case Declaration::kScalar:
    if (is_primitive_type(base))
      bottom_ += std::format("{}w.{}({});\n", indent, map_write_method(base),
                             val_expr);
    else
      bottom_ += std::format("{}{}.encode(w, {});\n", indent, ref(), val_expr);
    break;

  case Declaration::kArray:
  {
    string bound = map_bound(d.bound);
    if (base == "opaque")
      bottom_ += std::format("{}w.writeFixedOpaque({}, Number({}));\n", indent,
                             val_expr, bound);
    else if (is_primitive_type(base))
      bottom_ += std::format("{}for (const _item of {}) w.{}(_item);\n", indent,
                             val_expr, map_write_method(base));
    else
      bottom_ +=
          std::format("{}FixedArrayCodec({}, Number({})).encode(w, {});\n",
                      indent, ref(), bound, val_expr);
    break;
  }

  case Declaration::kVec:
    if (base == "opaque") {
      bottom_ += std::format("{}w.writeVarOpaque({}, Number({}));\n", indent,
                             val_expr, map_bound(d.bound));
    }
    else if (is_primitive_type(base)) {
      bottom_ += std::format("{}w.writeUint32(BigInt({}.length));\n", indent,
                             val_expr);
      bottom_ += std::format("{}for (const _item of {}) w.{}(_item);\n", indent,
                             val_expr, map_write_method(base));
    }
    else {
      bottom_ += std::format("{}VarArrayCodec({}, Number({})).encode(w, {});\n",
                             indent, ref(), map_bound(d.bound), val_expr);
    }
    break;

  case Declaration::kString:
    bottom_ += std::format("{}w.writeString({}, Number({}));\n", indent,
                           val_expr, map_bound(d.bound));
    break;

  case Declaration::kPtr:
    bottom_ += std::format("{}OptionCodec({}).encode(w, {});\n", indent,
                           proc_codec_ref(base), val_expr);
    break;

  case Declaration::kVoid:
    break;
  }
}

void
GenTs::decode_field(const Declaration &d, const string &parent_name, int sp)
{
  string base = base_type_name(d);
  if (!holds_alternative<string>(d.type))
    base = parent_name + "_" + d.id;

  string indent(sp, ' ');
  auto ref = [&] { return codec_ref(base); };

  switch (d.qual) {
  case Declaration::kScalar:
    if (is_primitive_type(base))
      bottom_ += std::format("{}const {} = r.{}();\n", indent, d.id,
                             map_read_method(base));
    else
      bottom_ +=
          std::format("{}const {} = {}.decode(r);\n", indent, d.id, ref());
    break;

  case Declaration::kArray:
  {
    string bound = map_bound(d.bound);
    // The runtime helpers return plain Uint8Array/T[]; cast to the
    // length-typed field type (the length is guaranteed by the codec).
    string cast = " as " + ts_field_type(d, parent_name);
    if (base == "opaque")
      bottom_ += std::format("{}const {} = r.readFixedOpaque(Number({})){};\n",
                             indent, d.id, bound, cast);
    else if (is_primitive_type(base))
      bottom_ +=
          std::format("{}const {} = Array.from({{ length: Number({}) }}, () => "
                      "r.{}()){};\n",
                      indent, d.id, bound, map_read_method(base), cast);
    else
      bottom_ += std::format(
          "{}const {} = FixedArrayCodec({}, Number({})).decode(r){};\n", indent,
          d.id, ref(), bound, cast);
    break;
  }

  case Declaration::kVec:
    if (base == "opaque") {
      bottom_ += std::format("{}const {} = r.readVarOpaque(Number({}));\n",
                             indent, d.id, map_bound(d.bound));
    }
    else if (is_primitive_type(base)) {
      string len_var = "_" + d.id + "_len";
      bottom_ += std::format("{}const {} = Number(r.readUint32());\n", indent,
                             len_var);
      bottom_ += std::format(
          "{}const {} = Array.from({{ length: {} }}, () => r.{}());\n", indent,
          d.id, len_var, map_read_method(base));
    }
    else {
      bottom_ +=
          std::format("{}const {} = VarArrayCodec({}, Number({})).decode(r);\n",
                      indent, d.id, ref(), map_bound(d.bound));
    }
    break;

  case Declaration::kString:
    bottom_ += std::format("{}const {} = r.readString(Number({}));\n", indent,
                           d.id, map_bound(d.bound));
    break;

  case Declaration::kPtr:
    bottom_ += std::format("{}const {} = OptionCodec({}).decode(r);\n", indent,
                           d.id, proc_codec_ref(base));
    break;

  case Declaration::kVoid:
    break;
  }
}

// ---- Entry point ----

std::string
gen_ts(vector<Definition> *defs)
{
  GenTs gs;
  gs.process(*defs);

  string preamble = "// -*- TypeScript -*-\n"
                    "// Generated Code: DO NOT CHECK IN\n"
                    "\n"
                    "// Generated from x23c commit: " +
                    x23::git_commit_hash() + "\n\n";

  {
    string imports = "import { type XdrCodec"
                     ", VoidCodec, OptionCodec, VarArrayCodec, FixedArrayCodec"
                     ", type FixedBytes, type FixedArray"
                     ", type VarBytes, type VarArray, type VarString";
    if (gs.has_programs_)
      imports += ", type RpcProgramMeta";
    for (const auto &pc : gs.primitive_codecs_)
      imports += ", " + pc;
    imports += " } from \"@groundhogchain/xdr\";\n";
    preamble += imports;
  }

  preamble += "import * as proto from \"./index.js\";\n";

  // Emit the codec namespace object
  string codec_ns = "\nexport const codec = {\n";
  for (const auto &name : gs.codec_names_)
    codec_ns += std::format("  {0}: _{0},\n", name);
  codec_ns += "} as const;\n";

  return preamble + gs.top_ + "\n//\n// XDR Codecs below this point\n//\n" +
         gs.bottom_ + codec_ns + gs.program_meta_ + "\n";
}
