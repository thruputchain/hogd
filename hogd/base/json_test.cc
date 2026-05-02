#include "hogd/base/json.h"

#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hogd::json {

namespace {

std::string
encode(auto &&body)
{
  std::string buf;
  Writer writer(buf);
  body(writer);
  return buf;
}

void
expect_parse_error(std::string_view input, ParseErrorKind kind)
{
  auto value = parse(input);
  ASSERT_FALSE(value.has_value());
  EXPECT_EQ(value.error().kind, kind);
}

} // namespace

TEST(JsonWriter, EncodesPrimitives)
{
  EXPECT_EQ(encode([](Writer &w) { w.value("hello"); }), R"("hello")");
  EXPECT_EQ(encode([](Writer &w) { w.value(std::int64_t{42}); }), "42");
  EXPECT_EQ(encode([](Writer &w) { w.value(std::int64_t{-7}); }), "-7");
  EXPECT_EQ(encode([](Writer &w) { w.value(true); }), "true");
  EXPECT_EQ(encode([](Writer &w) { w.value(false); }), "false");
  EXPECT_EQ(encode([](Writer &w) { w.null(); }), "null");
}

TEST(JsonWriter, EscapesStrings)
{
  EXPECT_EQ(encode([](Writer &w) { w.value("a\"b"); }), R"("a\"b")");
  EXPECT_EQ(encode([](Writer &w) { w.value("a\\b"); }), R"("a\\b")");
  EXPECT_EQ(encode([](Writer &w) { w.value("\b\f\n\r\t"); }),
            R"("\b\f\n\r\t")");
  // Control characters outside the named-escape set become \u00XX.
  EXPECT_EQ(encode([](Writer &w) { w.value(std::string_view("\x01\x1f", 2)); }),
            "\"\\u0001\\u001f\"");
  EXPECT_EQ(encode([](Writer &w) { w.value(std::string_view("\0", 1)); }),
            "\"\\u0000\"");
  // Printable ASCII passes through.
  EXPECT_EQ(encode([](Writer &w) { w.value("abc 123!"); }), R"("abc 123!")");
  // 0x7F (DEL) is not in JSON's escape range and passes through.
  EXPECT_EQ(encode([](Writer &w) { w.value(std::string_view("\x7f", 1)); }),
            "\"\x7f\"");
}

TEST(JsonWriter, EncodesEmptyContainers)
{
  EXPECT_EQ(encode([](Writer &w) { w.object([](Writer &) {}); }), "{}");
  EXPECT_EQ(encode([](Writer &w) { w.array([](Writer &) {}); }), "[]");
}

TEST(JsonWriter, EncodesObjectFields)
{
  std::string out = encode([](Writer &w) {
    w.object([](Writer &w) {
      w.field("name", "ready");
      w.field("count", std::int64_t{3});
      w.field("ok", true);
      w.null_field("note");
    });
  });
  EXPECT_EQ(out, R"({"name":"ready","count":3,"ok":true,"note":null})");
}

TEST(JsonWriter, EncodesArrayElements)
{
  std::string out = encode([](Writer &w) {
    w.array([](Writer &w) {
      w.value("a");
      w.value(std::int64_t{1});
      w.value(false);
      w.null();
    });
  });
  EXPECT_EQ(out, R"(["a",1,false,null])");
}

TEST(JsonWriter, EncodesNestedStructures)
{
  std::string out = encode([](Writer &w) {
    w.object([](Writer &w) {
      w.field("status", "ready");
      w.object_field("endpoints", [](Writer &w) {
        w.field("scheduler", "127.0.0.1:9000");
        w.field("read", "127.0.0.1:9001");
      });
      w.array_field("tags", [](Writer &w) {
        w.value("alpha");
        w.value("beta");
      });
    });
  });
  EXPECT_EQ(
      out,
      R"({"status":"ready","endpoints":{"scheduler":"127.0.0.1:9000","read":"127.0.0.1:9001"},"tags":["alpha","beta"]})");
}

TEST(JsonWriter, EscapesObjectKeys)
{
  std::string out = encode(
      [](Writer &w) { w.object([](Writer &w) { w.field("a\"b", "c"); }); });
  EXPECT_EQ(out, R"({"a\"b":"c"})");
}

TEST(JsonWriter, RejectsValueDirectlyInObject)
{
  EXPECT_DEATH(encode([](Writer &w) {
                 w.object([](Writer &w) { w.value(true); });
               }),
               "");
}

TEST(JsonWriter, RejectsMultipleTopLevelValues)
{
  EXPECT_DEATH(encode([](Writer &w) {
                 w.value("first");
                 w.value("second");
               }),
               "");
}

TEST(JsonParser, ParsesPrimitives)
{
  auto null_value = parse("null");
  ASSERT_TRUE(null_value.has_value());
  EXPECT_EQ(null_value->type(), Value::Type::kNull);

  auto true_value = parse("true");
  ASSERT_TRUE(true_value.has_value());
  EXPECT_EQ(true_value->as_boolean(), true);

  auto false_value = parse("false");
  ASSERT_TRUE(false_value.has_value());
  EXPECT_EQ(false_value->as_boolean(), false);

  auto integer = parse("42");
  ASSERT_TRUE(integer.has_value());
  EXPECT_EQ(integer->as_integer(), 42);

  auto negative = parse("-7");
  ASSERT_TRUE(negative.has_value());
  EXPECT_EQ(negative->as_integer(), -7);

  auto zero = parse("0");
  ASSERT_TRUE(zero.has_value());
  EXPECT_EQ(zero->as_integer(), 0);

  auto str = parse(R"("hello")");
  ASSERT_TRUE(str.has_value());
  EXPECT_EQ(str->as_string(), "hello");
}

TEST(JsonParser, AllowsLeadingAndTrailingWhitespace)
{
  auto value = parse("   42\n");
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(value->as_integer(), 42);
}

TEST(JsonParser, ParsesIntegerBoundsAndRejectsOverflow)
{
  auto min = parse("-9223372036854775808");
  ASSERT_TRUE(min.has_value());
  EXPECT_EQ(min->as_integer(), std::numeric_limits<std::int64_t>::min());

  auto max = parse("9223372036854775807");
  ASSERT_TRUE(max.has_value());
  EXPECT_EQ(max->as_integer(), std::numeric_limits<std::int64_t>::max());

  expect_parse_error("9223372036854775808", ParseErrorKind::kIntOverflow);
  expect_parse_error("-9223372036854775809", ParseErrorKind::kIntOverflow);
}

TEST(JsonParser, DecodesStringEscapes)
{
  auto value = parse(R"("a\"b\\c\/d\b\f\n\r\te")");
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(value->as_string(), "a\"b\\c/d\b\f\n\r\te");
}

TEST(JsonParser, DecodesUnicodeEscapesForBmpNonSurrogates)
{
  auto ascii = parse(R"("\u0041")");
  ASSERT_TRUE(ascii.has_value());
  EXPECT_EQ(ascii->as_string(), "A");

  // U+00E9 (é): C3 A9 in UTF-8.
  auto two_byte = parse(R"("\u00E9")");
  ASSERT_TRUE(two_byte.has_value());
  EXPECT_EQ(two_byte->as_string(), "\xC3\xA9");

  // U+20AC (€): E2 82 AC in UTF-8.
  auto three_byte = parse(R"("\u20AC")");
  ASSERT_TRUE(three_byte.has_value());
  EXPECT_EQ(three_byte->as_string(), "\xE2\x82\xAC");
}

TEST(JsonParser, AcceptsRawUtf8Strings)
{
  auto two_byte = parse(R"("é")");
  ASSERT_TRUE(two_byte.has_value());
  EXPECT_EQ(two_byte->as_string(), "\xC3\xA9");

  auto three_byte = parse(R"("€")");
  ASSERT_TRUE(three_byte.has_value());
  EXPECT_EQ(three_byte->as_string(), "\xE2\x82\xAC");

  auto four_byte = parse("\"😀\"");
  ASSERT_TRUE(four_byte.has_value());
  EXPECT_EQ(four_byte->as_string(), "\xF0\x9F\x98\x80");
}

TEST(JsonParser, RoundTripsEmbeddedNullFromUnicodeEscape)
{
  auto value = parse(R"("a\u0000b")");
  ASSERT_TRUE(value.has_value());
  std::string_view text =
      value->as_string().value_or(std::string_view("<missing>"));
  EXPECT_EQ(text, std::string_view("a\0b", 3));

  std::string encoded = encode([&](Writer &w) { w.value(text); });
  EXPECT_EQ(encoded, "\"a\\u0000b\"");
  auto reparsed = parse(encoded);
  ASSERT_TRUE(reparsed.has_value());
  EXPECT_EQ(reparsed->as_string(), std::string_view("a\0b", 3));
}

TEST(JsonParser, RejectsSurrogateEscapes)
{
  // 😀 is the UTF-16 surrogate pair for U+1F600 (the
  // grinning-face emoji). Surrogate-pair decoding is not supported.
  // The escape sequences are spelled out in a regular literal so the
  // parser sees the textual \u, not a transcoded UTF-8 codepoint.
  // NOLINTNEXTLINE(modernize-raw-string-literal)
  auto value = parse("\"\\uD83D\\uDE00\"");
  ASSERT_FALSE(value.has_value());
  EXPECT_EQ(value.error().kind, ParseErrorKind::kSurrogate);
}

TEST(JsonParser, RejectsInvalidUtf8Strings)
{
  const char lone_continuation[] = {'"', 'a', static_cast<char>(0x80), 'b',
                                    '"'};
  expect_parse_error(std::string_view(lone_continuation,
                                      sizeof(lone_continuation)),
                     ParseErrorKind::kInvalidUtf8);

  const char truncated_two_byte[] = {'"', static_cast<char>(0xC2), '"'};
  expect_parse_error(std::string_view(truncated_two_byte,
                                      sizeof(truncated_two_byte)),
                     ParseErrorKind::kInvalidUtf8);

  const char overlong_slash[] = {'"', static_cast<char>(0xC0),
                                 static_cast<char>(0xAF), '"'};
  expect_parse_error(std::string_view(overlong_slash, sizeof(overlong_slash)),
                     ParseErrorKind::kInvalidUtf8);

  // 3-byte overlong: encodes U+0000 in three bytes.
  const char overlong_three_byte[] = {'"', static_cast<char>(0xE0),
                                      static_cast<char>(0x80),
                                      static_cast<char>(0x80), '"'};
  expect_parse_error(std::string_view(overlong_three_byte,
                                      sizeof(overlong_three_byte)),
                     ParseErrorKind::kInvalidUtf8);

  // 4-byte overlong: encodes U+0000 in four bytes.
  const char overlong_four_byte[] = {'"',
                                     static_cast<char>(0xF0),
                                     static_cast<char>(0x80),
                                     static_cast<char>(0x80),
                                     static_cast<char>(0x80),
                                     '"'};
  expect_parse_error(std::string_view(overlong_four_byte,
                                      sizeof(overlong_four_byte)),
                     ParseErrorKind::kInvalidUtf8);

  // U+D800 expressed as raw UTF-8 bytes: ED A0 80. JSON allows the
  // surrogate range only via paired \uD8XX\uDCXX escapes, never as raw
  // UTF-8 (RFC 3629 forbids it).
  const char raw_surrogate[] = {'"', static_cast<char>(0xED),
                                static_cast<char>(0xA0),
                                static_cast<char>(0x80), '"'};
  expect_parse_error(std::string_view(raw_surrogate, sizeof(raw_surrogate)),
                     ParseErrorKind::kInvalidUtf8);

  // Codepoint above U+10FFFF: F4 90 80 80 (would decode to U+110000).
  const char above_max[] = {'"',
                            static_cast<char>(0xF4),
                            static_cast<char>(0x90),
                            static_cast<char>(0x80),
                            static_cast<char>(0x80),
                            '"'};
  expect_parse_error(std::string_view(above_max, sizeof(above_max)),
                     ParseErrorKind::kInvalidUtf8);
}

TEST(JsonParser, ParsesObjects)
{
  auto value = parse(R"({"name":"ready","count":3,"ok":true})");
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(value->type(), Value::Type::kObject);
  EXPECT_EQ(value->find("name")->as_string(), "ready");
  EXPECT_EQ(value->find("count")->as_integer(), 3);
  EXPECT_EQ(value->find("ok")->as_boolean(), true);
  EXPECT_EQ(value->find("missing"), nullptr);
}

TEST(JsonParser, ParsesArrays)
{
  auto value = parse(R"([1,"two",true,null])");
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(value->type(), Value::Type::kArray);
  auto elements = value->as_array();
  ASSERT_EQ(elements.size(), 4U);
  EXPECT_EQ(elements[0].as_integer(), 1);
  EXPECT_EQ(elements[1].as_string(), "two");
  EXPECT_EQ(elements[2].as_boolean(), true);
  EXPECT_EQ(elements[3].type(), Value::Type::kNull);
}

TEST(JsonParser, ParsesNestedStructures)
{
  auto value = parse(
      R"({"status":"ready","endpoints":{"scheduler":"127.0.0.1:9000"},"tags":["a","b"]})");
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(value->find("status")->as_string(), "ready");
  EXPECT_EQ(value->find("endpoints")->find("scheduler")->as_string(),
            "127.0.0.1:9000");
  auto tags = value->find("tags")->as_array();
  ASSERT_EQ(tags.size(), 2U);
  EXPECT_EQ(tags[0].as_string(), "a");
  EXPECT_EQ(tags[1].as_string(), "b");
}

TEST(JsonParser, AcceptsWhitespaceBetweenTokens)
{
  auto value = parse(R"({  "a"  :  [ 1 , 2 ]  })");
  ASSERT_TRUE(value.has_value());
  auto items = value->find("a")->as_array();
  ASSERT_EQ(items.size(), 2U);
  EXPECT_EQ(items[0].as_integer(), 1);
  EXPECT_EQ(items[1].as_integer(), 2);
}

TEST(JsonParser, ParsesEmptyContainers)
{
  auto obj = parse("{}");
  ASSERT_TRUE(obj.has_value());
  EXPECT_EQ(obj->type(), Value::Type::kObject);
  EXPECT_TRUE(obj->as_object().empty());

  auto arr = parse("[]");
  ASSERT_TRUE(arr.has_value());
  EXPECT_EQ(arr->type(), Value::Type::kArray);
  EXPECT_TRUE(arr->as_array().empty());
}

TEST(JsonParser, RejectsTrailingContent)
{
  auto value = parse("42 garbage");
  ASSERT_FALSE(value.has_value());
}

TEST(JsonParser, RejectsFloats)
{
  auto value = parse("1.5");
  ASSERT_FALSE(value.has_value());
  EXPECT_EQ(value.error().kind, ParseErrorKind::kFloat);
}

TEST(JsonParser, RejectsTruncatedInput)
{
  EXPECT_FALSE(parse("").has_value());
  EXPECT_FALSE(parse("{").has_value());
  EXPECT_FALSE(parse(R"({"a")").has_value());
  EXPECT_FALSE(parse(R"({"a":)").has_value());
  EXPECT_FALSE(parse("[1,").has_value());
}

TEST(JsonParser, ReportsErrorOffset)
{
  // Object with a missing colon at offset 4: {"a" 1}.
  auto value = parse(R"({"a" 1})");
  ASSERT_FALSE(value.has_value());
  EXPECT_EQ(value.error().offset, 5U);
}

TEST(JsonParser, RejectsUnescapedControlCharacterInString)
{
  auto value = parse(std::string_view("\"a\nb\"", 5));
  ASSERT_FALSE(value.has_value());
}

TEST(JsonParser, RejectsLeadingZero)
{
  auto value = parse("01");
  // 0 parses successfully, then trailing "1" is rejected.
  ASSERT_FALSE(value.has_value());
  EXPECT_EQ(value.error().kind, ParseErrorKind::kTrailingContent);
}

TEST(JsonParser, RejectsDeepNestingWithoutStackOverflow)
{
  std::string input(10'000, '[');
  input.append(10'000, ']');

  auto value = parse(input);
  ASSERT_FALSE(value.has_value());
  EXPECT_EQ(value.error().kind, ParseErrorKind::kDepthLimit);
}

TEST(JsonParser, AcceptsExactlyMaxNestingDepth)
{
  // Pin the depth contract: 64 nested arrays parse, 65 do not.
  constexpr std::size_t kMax = 64;

  std::string just_under(kMax, '[');
  just_under.append(kMax, ']');
  EXPECT_TRUE(parse(just_under).has_value());

  std::string just_over(kMax + 1, '[');
  just_over.append(kMax + 1, ']');
  expect_parse_error(just_over, ParseErrorKind::kDepthLimit);
}

TEST(Value, FindReturnsNullForNonObjects)
{
  Value array_value(std::vector<Value>{});
  EXPECT_EQ(array_value.find("anything"), nullptr);

  Value string_value(std::string("plain"));
  EXPECT_EQ(string_value.find("anything"), nullptr);
}

TEST(Value, AccessorsReturnNulloptForWrongType)
{
  Value n(Value::Null{});
  EXPECT_FALSE(n.as_boolean().has_value());
  EXPECT_FALSE(n.as_integer().has_value());
  EXPECT_FALSE(n.as_string().has_value());
}

TEST(Value, AsObjectExposesFieldsInInsertionOrder)
{
  auto value = parse(R"({"first":1,"second":2,"third":3})");
  ASSERT_TRUE(value.has_value());
  auto fields = value->as_object();
  ASSERT_EQ(fields.size(), 3U);
  EXPECT_EQ(fields[0].first, "first");
  EXPECT_EQ(fields[1].first, "second");
  EXPECT_EQ(fields[2].first, "third");
}

TEST(JsonRoundTrip, WriterOutputParsesBackToInput)
{
  std::string buf;
  Writer(buf).object([](Writer &w) {
    w.field("status", "ready");
    w.field("count", std::int64_t{42});
    w.field("ok", true);
    w.null_field("note");
    w.object_field("endpoints",
                   [](Writer &w) { w.field("scheduler", "127.0.0.1:9000"); });
    w.array_field("tags", [](Writer &w) {
      w.value("alpha");
      w.value(std::int64_t{1});
    });
  });

  auto parsed = parse(buf);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->find("status")->as_string(), "ready");
  EXPECT_EQ(parsed->find("count")->as_integer(), 42);
  EXPECT_EQ(parsed->find("ok")->as_boolean(), true);
  EXPECT_EQ(parsed->find("note")->type(), Value::Type::kNull);
  EXPECT_EQ(parsed->find("endpoints")->find("scheduler")->as_string(),
            "127.0.0.1:9000");
  auto tags = parsed->find("tags")->as_array();
  ASSERT_EQ(tags.size(), 2U);
  EXPECT_EQ(tags[0].as_string(), "alpha");
  EXPECT_EQ(tags[1].as_integer(), 1);
}

} // namespace hogd::json
