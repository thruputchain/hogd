#include "hogd/base/json.h"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "hogd/base/log.h"

namespace hogd::json {

namespace {

constexpr std::array<char, 16> kHexDigits = {'0', '1', '2', '3', '4', '5',
                                             '6', '7', '8', '9', 'a', 'b',
                                             'c', 'd', 'e', 'f'};
constexpr std::size_t kMaxNestingDepth = 64;

void
escape_string(std::string &out, std::string_view value)
{
  out += '"';
  for (char c : value) {
    auto byte = static_cast<unsigned char>(c);
    switch (byte) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      // JSON requires escapes for U+0000 through U+001F. Other bytes
      // pass through; valid UTF-8 input produces valid UTF-8 output.
      if (byte < 0x20) {
        out += "\\u00";
        out += kHexDigits[byte >> 4];
        out += kHexDigits[byte & 0x0F];
      }
      else {
        out += c;
      }
    }
  }
  out += '"';
}

// Recursive-descent parser. parse_value, parse_object, and
// parse_array form a recursive call chain by design; the depth is
// bounded by the input's nesting depth.
// NOLINTBEGIN(misc-no-recursion)
class Parser {
public:
  Parser(std::string_view input) : input_(input) {}

  std::expected<Value, ParseError> parse_document()
  {
    skip_whitespace();
    auto value = parse_value();
    if (!value)
      return std::unexpected(value.error());
    skip_whitespace();
    if (pos_ != input_.size())
      return error(ParseErrorKind::kTrailingContent,
                   "unexpected trailing content");
    return value;
  }

private:
  struct DepthGuard {
    explicit DepthGuard(Parser *parser) : parser_(parser) {}
    DepthGuard(const DepthGuard &) = delete;
    DepthGuard &operator=(const DepthGuard &) = delete;
    DepthGuard(DepthGuard &&other) noexcept
      : parser_(std::exchange(other.parser_, nullptr))
    {}
    DepthGuard &operator=(DepthGuard &&) = delete;
    ~DepthGuard()
    {
      if (parser_ != nullptr) {
        CHECK_GT(parser_->depth_, 0U);
        --parser_->depth_;
      }
    }

  private:
    Parser *parser_;
  };

  std::expected<Value, ParseError> parse_value()
  {
    if (pos_ == input_.size())
      return error(ParseErrorKind::kUnexpectedEof, "unexpected end of input");

    char c = input_[pos_];
    switch (c) {
    case '{':
      return parse_object();
    case '[':
      return parse_array();
    case '"':
      return parse_string_value();
    case 't':
      return parse_literal("true", Value(true));
    case 'f':
      return parse_literal("false", Value(false));
    case 'n':
      return parse_literal("null", Value(Value::Null{}));
    default:
      if (c == '-' || (c >= '0' && c <= '9'))
        return parse_integer_value();
      return error(ParseErrorKind::kUnexpectedChar,
                   std::format("unexpected character: {}", c));
    }
  }

  std::expected<Value, ParseError> parse_object()
  {
    CHECK_EQ(input_[pos_], '{');
    auto depth = enter_container();
    if (!depth)
      return std::unexpected(depth.error());
    ++pos_;
    skip_whitespace();

    std::vector<std::pair<std::string, Value>> fields;
    if (pos_ < input_.size() && input_[pos_] == '}') {
      ++pos_;
      return Value(std::move(fields));
    }

    for (;;) {
      skip_whitespace();
      if (pos_ == input_.size() || input_[pos_] != '"')
        return error(ParseErrorKind::kExpectedKey,
                     "expected string key in object");
      auto key = parse_string();
      if (!key)
        return std::unexpected(key.error());

      skip_whitespace();
      if (pos_ == input_.size() || input_[pos_] != ':')
        return error(ParseErrorKind::kExpectedColon,
                     "expected ':' after object key");
      ++pos_;

      skip_whitespace();
      auto value = parse_value();
      if (!value)
        return std::unexpected(value.error());
      fields.emplace_back(std::move(*key), std::move(*value));

      skip_whitespace();
      if (pos_ == input_.size())
        return error(ParseErrorKind::kUnterminatedObject,
                     "unterminated object");
      if (input_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (input_[pos_] == '}') {
        ++pos_;
        return Value(std::move(fields));
      }
      return error(ParseErrorKind::kExpectedSeparator,
                   "expected ',' or '}' in object");
    }
  }

  std::expected<Value, ParseError> parse_array()
  {
    CHECK_EQ(input_[pos_], '[');
    auto depth = enter_container();
    if (!depth)
      return std::unexpected(depth.error());
    ++pos_;
    skip_whitespace();

    std::vector<Value> elements;
    if (pos_ < input_.size() && input_[pos_] == ']') {
      ++pos_;
      return Value(std::move(elements));
    }

    for (;;) {
      skip_whitespace();
      auto value = parse_value();
      if (!value)
        return std::unexpected(value.error());
      elements.push_back(std::move(*value));

      skip_whitespace();
      if (pos_ == input_.size())
        return error(ParseErrorKind::kUnterminatedArray, "unterminated array");
      if (input_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (input_[pos_] == ']') {
        ++pos_;
        return Value(std::move(elements));
      }
      return error(ParseErrorKind::kExpectedSeparator,
                   "expected ',' or ']' in array");
    }
  }

  std::expected<Value, ParseError> parse_string_value()
  {
    auto s = parse_string();
    if (!s)
      return std::unexpected(s.error());
    return Value(std::move(*s));
  }

  std::expected<std::string, ParseError> parse_string()
  {
    CHECK_EQ(input_[pos_], '"');
    ++pos_;

    std::string out;
    while (pos_ < input_.size()) {
      char c = input_[pos_];
      if (c == '"') {
        ++pos_;
        return out;
      }
      if (static_cast<unsigned char>(c) < 0x20)
        return error(ParseErrorKind::kControlCharacter,
                     "unescaped control character in string");
      if (c != '\\') {
        if (static_cast<unsigned char>(c) >= 0x80) {
          auto utf8 = append_utf8(out);
          if (!utf8)
            return std::unexpected(utf8.error());
          continue;
        }
        out += c;
        ++pos_;
        continue;
      }
      // Escape sequence.
      ++pos_;
      if (pos_ == input_.size())
        return error(ParseErrorKind::kBadEscape, "incomplete escape sequence");
      char esc = input_[pos_++];
      switch (esc) {
      case '"':
        out += '"';
        break;
      case '\\':
        out += '\\';
        break;
      case '/':
        out += '/';
        break;
      case 'b':
        out += '\b';
        break;
      case 'f':
        out += '\f';
        break;
      case 'n':
        out += '\n';
        break;
      case 'r':
        out += '\r';
        break;
      case 't':
        out += '\t';
        break;
      case 'u':
      {
        auto codepoint = parse_unicode_escape();
        if (!codepoint)
          return std::unexpected(codepoint.error());
        encode_utf8(out, *codepoint);
        break;
      }
      default:
        return error(ParseErrorKind::kBadEscape,
                     std::format("invalid escape: \\{}", esc));
      }
    }
    return error(ParseErrorKind::kUnterminatedString, "unterminated string");
  }

  std::expected<std::uint32_t, ParseError> parse_unicode_escape()
  {
    if (pos_ + 4 > input_.size())
      return error(ParseErrorKind::kBadEscape, "incomplete \\u escape");
    std::uint32_t codepoint = 0;
    for (std::size_t i = 0; i < 4; ++i) {
      char c = input_[pos_ + i];
      std::uint32_t digit;
      if (c >= '0' && c <= '9')
        digit = static_cast<std::uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f')
        digit = static_cast<std::uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        digit = static_cast<std::uint32_t>(c - 'A' + 10);
      else
        return error(ParseErrorKind::kBadEscape,
                     "invalid hex digit in \\u escape");
      codepoint = (codepoint << 4) | digit;
    }
    pos_ += 4;
    if (codepoint >= 0xD800 && codepoint <= 0xDFFF)
      return error(ParseErrorKind::kSurrogate,
                   "surrogate pair escapes are not supported");
    return codepoint;
  }

  std::expected<void, ParseError> append_utf8(std::string &out)
  {
    std::size_t start = pos_;
    auto lead = static_cast<unsigned char>(input_[pos_]);

    auto append = [&](std::size_t len) -> std::expected<void, ParseError> {
      out.append(input_.substr(start, len));
      pos_ += len;
      return {};
    };
    auto valid_byte = [&](std::size_t offset, unsigned char min,
                          unsigned char max) {
      if (pos_ + offset >= input_.size())
        return false;
      auto byte = static_cast<unsigned char>(input_[pos_ + offset]);
      return byte >= min && byte <= max;
    };
    auto continuation = [&](std::size_t offset) {
      return valid_byte(offset, 0x80, 0xBF);
    };

    if (lead >= 0xC2 && lead <= 0xDF) {
      if (!continuation(1))
        return invalid_utf8();
      return append(2);
    }
    if (lead == 0xE0) {
      if (!valid_byte(1, 0xA0, 0xBF) || !continuation(2))
        return invalid_utf8();
      return append(3);
    }
    if ((lead >= 0xE1 && lead <= 0xEC) || (lead >= 0xEE && lead <= 0xEF)) {
      if (!continuation(1) || !continuation(2))
        return invalid_utf8();
      return append(3);
    }
    if (lead == 0xED) {
      if (!valid_byte(1, 0x80, 0x9F) || !continuation(2))
        return invalid_utf8();
      return append(3);
    }
    if (lead == 0xF0) {
      if (!valid_byte(1, 0x90, 0xBF) || !continuation(2) || !continuation(3))
        return invalid_utf8();
      return append(4);
    }
    if (lead >= 0xF1 && lead <= 0xF3) {
      if (!continuation(1) || !continuation(2) || !continuation(3))
        return invalid_utf8();
      return append(4);
    }
    if (lead == 0xF4) {
      if (!valid_byte(1, 0x80, 0x8F) || !continuation(2) || !continuation(3))
        return invalid_utf8();
      return append(4);
    }

    return invalid_utf8();
  }

  static void encode_utf8(std::string &out, std::uint32_t codepoint)
  {
    if (codepoint < 0x80) {
      out += static_cast<char>(codepoint);
    }
    else if (codepoint < 0x800) {
      out += static_cast<char>(0xC0 | (codepoint >> 6));
      out += static_cast<char>(0x80 | (codepoint & 0x3F));
    }
    else {
      out += static_cast<char>(0xE0 | (codepoint >> 12));
      out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (codepoint & 0x3F));
    }
  }

  std::expected<Value, ParseError> parse_integer_value()
  {
    std::size_t start = pos_;
    if (input_[pos_] == '-')
      ++pos_;
    if (pos_ == input_.size() || input_[pos_] < '0' || input_[pos_] > '9')
      return error(ParseErrorKind::kInvalidInteger, "expected digit");
    if (input_[pos_] == '0') {
      ++pos_;
    }
    else {
      while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9')
        ++pos_;
    }
    if (pos_ < input_.size() &&
        (input_[pos_] == '.' || input_[pos_] == 'e' || input_[pos_] == 'E'))
      return error(ParseErrorKind::kFloat,
                   "floating-point numbers are not supported");

    std::int64_t value = 0;
    auto [_, ec] =
        std::from_chars(input_.data() + start, input_.data() + pos_, value);
    if (ec == std::errc::result_out_of_range)
      return error(ParseErrorKind::kIntOverflow, "integer out of range");
    // The manual scan above only forwards a leading '-' followed by
    // digits to from_chars, so this fallthrough should not be
    // reachable for well-formed inputs; keep it as defensive
    // belt-and-suspenders rather than a CHECK.
    if (ec != std::errc())
      return error(ParseErrorKind::kInvalidInteger, "invalid integer");
    return Value(value);
  }

  std::expected<Value, ParseError> parse_literal(std::string_view text,
                                                 Value value)
  {
    if (input_.substr(pos_, text.size()) != text)
      return error(ParseErrorKind::kUnexpectedChar,
                   std::format("expected '{}'", text));
    pos_ += text.size();
    return value;
  }

  void skip_whitespace()
  {
    while (pos_ < input_.size()) {
      char c = input_[pos_];
      if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
        return;
      ++pos_;
    }
  }

  std::expected<DepthGuard, ParseError> enter_container()
  {
    if (depth_ >= kMaxNestingDepth) {
      return error(ParseErrorKind::kDepthLimit,
                   "maximum JSON nesting depth exceeded");
    }
    ++depth_;
    return DepthGuard(this);
  }

  std::unexpected<ParseError> invalid_utf8() const
  {
    return error(ParseErrorKind::kInvalidUtf8, "invalid UTF-8 in string");
  }

  std::unexpected<ParseError> error(ParseErrorKind kind,
                                    std::string message) const
  {
    return std::unexpected(ParseError{.kind = kind,
                                      .message = std::move(message),
                                      .offset = pos_});
  }

  std::string_view input_;
  std::size_t pos_{0};
  std::size_t depth_{0};
};
// NOLINTEND(misc-no-recursion)

} // namespace

Writer::Writer(std::string &out) : out_(&out)
{}

void
Writer::value(std::string_view v)
{
  emit_value_separator();
  escape_string(*out_, v);
}

void
Writer::value(std::int64_t v)
{
  emit_value_separator();
  *out_ += std::format("{}", v);
}

void
Writer::null()
{
  emit_value_separator();
  *out_ += "null";
}

void
Writer::emit_boolean(bool value)
{
  emit_value_separator();
  *out_ += value ? "true" : "false";
}

void
Writer::field(std::string_view key, std::string_view value)
{
  emit_field_key(key);
  this->value(value);
}

void
Writer::field(std::string_view key, std::int64_t value)
{
  emit_field_key(key);
  this->value(value);
}

void
Writer::null_field(std::string_view key)
{
  emit_field_key(key);
  null();
}

void
Writer::enter_scope(Scope scope, char open)
{
  emit_value_separator();
  *out_ += open;
  scope_stack_.push_back(scope);
  need_separator_ = false;
}

void
Writer::exit_scope(char close)
{
  CHECK(!scope_stack_.empty());
  *out_ += close;
  scope_stack_.pop_back();
  // Returning to the parent scope: we just emitted a value, so the
  // next sibling needs a separator.
  need_separator_ = true;
}

void
Writer::emit_value_separator()
{
  if (scope_stack_.empty()) {
    CHECK(!need_separator_);
  }
  else if (scope_stack_.back() == Scope::kObject) {
    CHECK(expecting_field_value_);
    expecting_field_value_ = false;
  }
  else if (need_separator_) {
    *out_ += ',';
  }
  need_separator_ = true;
}

void
Writer::emit_field_key(std::string_view key)
{
  CHECK(!scope_stack_.empty() && scope_stack_.back() == Scope::kObject);
  if (need_separator_)
    *out_ += ',';
  escape_string(*out_, key);
  *out_ += ':';
  // The next emission is the value of this field, not a sibling, so
  // it must not be preceded by a comma.
  need_separator_ = false;
  expecting_field_value_ = true;
}

Value::Type
Value::type() const
{
  struct Visitor {
    Type operator()(const Null & /*tag*/) const { return Type::kNull; }
    Type operator()(bool /*b*/) const { return Type::kBoolean; }
    Type operator()(std::int64_t /*n*/) const { return Type::kInteger; }
    Type operator()(const std::string & /*s*/) const { return Type::kString; }
    Type operator()(const std::vector<Value> & /*a*/) const
    {
      return Type::kArray;
    }
    Type
    operator()(const std::vector<std::pair<std::string, Value>> & /*o*/) const
    {
      return Type::kObject;
    }
  };
  return std::visit(Visitor{}, storage_);
}

std::optional<bool>
Value::as_boolean() const
{
  if (const auto *b = std::get_if<bool>(&storage_))
    return *b;
  return std::nullopt;
}

std::optional<std::int64_t>
Value::as_integer() const
{
  if (const auto *n = std::get_if<std::int64_t>(&storage_))
    return *n;
  return std::nullopt;
}

std::optional<std::string_view>
Value::as_string() const
{
  if (const auto *s = std::get_if<std::string>(&storage_))
    return std::string_view(*s);
  return std::nullopt;
}

std::span<const Value>
Value::as_array() const
{
  if (const auto *a = std::get_if<std::vector<Value>>(&storage_))
    return *a;
  return {};
}

std::span<const std::pair<std::string, Value>>
Value::as_object() const
{
  if (const auto *o =
          std::get_if<std::vector<std::pair<std::string, Value>>>(&storage_))
    return *o;
  return {};
}

const Value *
Value::find(std::string_view key) const
{
  const auto *o =
      std::get_if<std::vector<std::pair<std::string, Value>>>(&storage_);
  if (o == nullptr)
    return nullptr;
  for (const auto &[k, v] : *o) {
    if (k == key)
      return &v;
  }
  return nullptr;
}

std::expected<Value, ParseError>
parse(std::string_view input)
{
  return Parser(input).parse_document();
}

} // namespace hogd::json
