// json.h - minimal JSON encoding and decoding utilities
//
// Supports objects, arrays, strings, integers, booleans, and null.
// Floating-point numbers, schema validation, and surrogate-pair
// decoding are out of scope; for those, prefer a dedicated JSON
// library.

#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace hogd::json {

// Writer streams JSON output into a std::string. Object and array
// scopes are bounded by lambda callbacks so brace pairing is
// structurally enforced. String content is escaped per RFC 8259 and
// is expected to be valid UTF-8; non-ASCII bytes pass through.
//
// Example:
//   std::string buf;
//   hogd::json::Writer(buf).object([](hogd::json::Writer &w) {
//     w.field("status", "ready");
//     w.object_field("endpoints", [](hogd::json::Writer &w) {
//       w.field("scheduler", "127.0.0.1:9000");
//     });
//   });
class Writer {
public:
  // out is the destination buffer. The Writer borrows it; the caller
  // must keep it alive for the Writer's lifetime.
  explicit Writer(std::string &out);

  // Allow move; disable copy.
  Writer(Writer &&) noexcept = default;
  Writer &operator=(Writer &&) noexcept = default;
  Writer(const Writer &) = delete;
  Writer &operator=(const Writer &) = delete;

  // object emits a JSON object. body populates the fields by calling
  // field()/object_field()/array_field() on the writer it receives.
  template<std::invocable<Writer &> F> void object(F &&body)
  {
    enter_scope(Scope::kObject, '{');
    body(*this);
    exit_scope('}');
  }

  // array emits a JSON array. body populates the elements by calling
  // value()/object()/array()/null() on the writer it receives.
  template<std::invocable<Writer &> F> void array(F &&body)
  {
    enter_scope(Scope::kArray, '[');
    body(*this);
    exit_scope(']');
  }

  // value emits a primitive value at the current position. Use inside
  // an array body, or as the only top-level call.
  //
  // The bool overload is constrained so that pointer or integral
  // arguments do not silently bind to it via standard conversions.
  void value(std::string_view v);
  void value(std::int64_t v);
  template<std::same_as<bool> T> void value(T v) { emit_boolean(v); }
  void null();

  // field emits a key/value pair. Must be called inside an object
  // body.
  void field(std::string_view key, std::string_view value);
  void field(std::string_view key, std::int64_t value);
  template<std::same_as<bool> T> void field(std::string_view key, T value)
  {
    emit_field_key(key);
    this->value(value);
  }

  // null_field emits a key whose value is JSON null. Must be called
  // inside an object body.
  void null_field(std::string_view key);

  // object_field emits a key whose value is a nested object. Must be
  // called inside an object body.
  template<std::invocable<Writer &> F>
  void object_field(std::string_view key, F &&body)
  {
    emit_field_key(key);
    object(std::forward<F>(body));
  }

  // array_field emits a key whose value is a nested array. Must be
  // called inside an object body.
  template<std::invocable<Writer &> F>
  void array_field(std::string_view key, F &&body)
  {
    emit_field_key(key);
    array(std::forward<F>(body));
  }

private:
  enum class Scope { kTop, kObject, kArray };

  std::string *out_;
  std::vector<Scope> scope_stack_;
  bool need_separator_{false};
  bool expecting_field_value_{false};

  void enter_scope(Scope scope, char open);
  void exit_scope(char close);
  void emit_value_separator();
  void emit_field_key(std::string_view key);
  void emit_boolean(bool value);
};

// Value is a parsed JSON value. Construct via parse().
class Value {
public:
  // Null is the type tag for a JSON null value.
  struct Null {
    bool operator==(const Null &) const = default;
  };

  // Type discriminates the value kind.
  enum class Type {
    kNull,
    kBoolean,
    kInteger,
    kString,
    kArray,
    kObject,
  };

  // Value default-constructs to JSON null. Other kinds are normally
  // produced by parse().
  Value() = default;
  explicit Value(Null tag) : storage_(tag) {}
  explicit Value(bool b) : storage_(b) {}
  explicit Value(std::int64_t n) : storage_(n) {}
  explicit Value(const std::string &s) : storage_(s) {}
  explicit Value(std::string &&s) : storage_(std::move(s)) {}
  explicit Value(std::vector<Value> a) : storage_(std::move(a)) {}
  explicit Value(std::vector<std::pair<std::string, Value>> o)
    : storage_(std::move(o))
  {}

  // type returns the discriminator for this value.
  [[nodiscard]] Type type() const;

  // is_null is shorthand for type() == Type::kNull.
  [[nodiscard]] bool is_null() const { return type() == Type::kNull; }

  // as_<type> returns the value if it has the requested type, or
  // std::nullopt otherwise.
  [[nodiscard]] std::optional<bool> as_boolean() const;
  [[nodiscard]] std::optional<std::int64_t> as_integer() const;
  [[nodiscard]] std::optional<std::string_view> as_string() const;

  // as_array returns the array's elements, or an empty span if the
  // value is not an array. Use type() to disambiguate "not an array"
  // from "an empty array".
  [[nodiscard]] std::span<const Value> as_array() const;

  // as_object returns the object's fields in insertion order, or an
  // empty span if the value is not an object. Use type() to
  // disambiguate "not an object" from "an empty object".
  [[nodiscard]] std::span<const std::pair<std::string, Value>>
  as_object() const;

  // find returns a pointer to the named field's value, or nullptr if
  // this value is not an object or the key is absent.
  [[nodiscard]] const Value *find(std::string_view key) const;

private:
  using Storage =
      std::variant<Null, bool, std::int64_t, std::string, std::vector<Value>,
                   std::vector<std::pair<std::string, Value>>>;
  Storage storage_;
};

// ParseErrorKind is a stable machine-readable parse failure code.
//
// Non-exhaustive: new kinds may be added in future versions. Consumers
// switching on this enum must include a default arm to remain
// forward-compatible. Existing kinds will not be removed or renumbered.
enum class ParseErrorKind {
  // Input nested deeper than the parser's hard cap.
  kDepthLimit,
  // Input ended while a value was still being read.
  kUnexpectedEof,
  // Encountered a byte that cannot start any value (e.g. ']' at the
  // start of input, or 'x' where a literal was expected).
  kUnexpectedChar,
  // The document's value parsed cleanly, but non-whitespace bytes
  // remained.
  kTrailingContent,
  // Expected a quoted key inside an object body.
  kExpectedKey,
  // Expected ':' between an object key and its value.
  kExpectedColon,
  // Expected ',' or the matching closing brace/bracket between
  // siblings in a container.
  kExpectedSeparator,
  // Object lacked a closing '}'.
  kUnterminatedObject,
  // Array lacked a closing ']'.
  kUnterminatedArray,
  // String lacked a closing '"'.
  kUnterminatedString,
  // String escape was malformed (incomplete, unknown letter, or bad
  // hex digit in \uXXXX).
  kBadEscape,
  // \uXXXX escape decoded to a UTF-16 surrogate codepoint
  // (U+D800-U+DFFF). Surrogate-pair decoding is not supported.
  kSurrogate,
  // Number contained '.', 'e', or 'E'. Floating-point is not
  // supported.
  kFloat,
  // Integer literal was outside the int64 range.
  kIntOverflow,
  // Number had a syntactic problem other than overflow (e.g. lone
  // '-' with no digit).
  kInvalidInteger,
  // String content contained a malformed UTF-8 sequence (overlong,
  // truncated, surrogate range, or > U+10FFFF).
  kInvalidUtf8,
  // String content contained an unescaped 0x00-0x1F byte. JSON
  // requires these to be escaped.
  kControlCharacter,
};

// ParseError describes a parse failure.
struct ParseError {
  // Stable machine-readable failure code. Switch on this, not on
  // message text.
  ParseErrorKind kind;
  // Human-readable diagnostic. May include a byte drawn from the
  // failing input; treat as untrusted-content-derived and do not
  // surface it to metrics labels or logs that flow to untrusted
  // consumers.
  std::string message;
  // Byte position in the input where the parser stopped.
  std::size_t offset;
};

// parse parses a complete JSON document. Trailing whitespace is
// allowed; any other trailing content is an error.
//
// Strings are decoded as raw UTF-8 with the standard JSON escape
// sequences (\", \\, \/, \b, \f, \n, \r, \t, \uXXXX for BMP
// codepoints excluding surrogates). Malformed UTF-8 returns
// kInvalidUtf8.
//
// Inputs nested deeper than 64 levels are rejected with kDepthLimit
// so adversarial input cannot exhaust the C++ stack. parse() does
// not bound input length; callers feeding untrusted input must
// impose their own length limit before calling.
//
// Example:
//   auto parsed = hogd::json::parse(buf);
//   if (!parsed) {
//     // parsed.error().kind discriminates the failure mode.
//   }
//   const hogd::json::Value *status = parsed->find("status");
//   std::optional<std::string_view> ready = status->as_string();
[[nodiscard]] std::expected<Value, ParseError> parse(std::string_view input);

} // namespace hogd::json
