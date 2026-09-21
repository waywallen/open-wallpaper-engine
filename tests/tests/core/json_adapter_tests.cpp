#include <rstd/test/gtest.hpp>

import rstd.cppstd;
import owe.user_property;
import wescene.json;
import wescene.testing.json_builder;

using namespace rstd::literals;
using namespace rstd::prelude;

TEST(JsonAdapter, ParsesDumpsAndReportsMembers) {
    auto parsed = owe::ParseJson(R"({"z":[true,null],"a":1.0})"_str);
    ASSERT_TRUE(parsed.is_ok());
    auto value = parsed.unwrap();
    EXPECT_TRUE(value.get("a"_str).is_some());
    EXPECT_TRUE(value.get("missing"_str).is_none());
    const std::string dynamic_key = "a";
    EXPECT_TRUE(value.get(rstd::cppstd::as_str(dynamic_key).unwrap()).is_some());
    const std::string_view mutable_key = "z";
    EXPECT_TRUE(value.get_mut(rstd::cppstd::as_str(mutable_key).unwrap()).is_some());
    auto z = value.get("z"_str);
    ASSERT_TRUE(z.is_some());
    EXPECT_TRUE((*z)->is_array());
    EXPECT_EQ(rstd::cppstd::to_string(owe::DumpString(value).as_str()),
              R"({"a":1.0,"z":[true,null]})");
    EXPECT_EQ(rstd::cppstd::to_string(
                  owe::DumpString(value, rstd::Some(rstd::usize(std::size_t { 2 }))).as_str()),
              "{\n  \"a\": 1.0,\n  \"z\": [\n    true,\n    null\n  ]\n}");
}

TEST(JsonAdapter, CommentsRequireExplicitOption) {
    EXPECT_TRUE(owe::ParseJson("/* comment */ null"_str).is_err());
    auto parsed =
        owe::ParseJson("{/* comment */ \"value\": 1 // line\n}"_str, { .allow_comments = true });
    ASSERT_TRUE(parsed.is_ok());
    auto value  = parsed.unwrap();
    auto member = value.get("value"_str);
    ASSERT_TRUE(member.is_some());
    EXPECT_EQ((*member)->as_i64().unwrap_or(rstd::i64()).to_primitive(), 1);
}

TEST(JsonAdapter, ClonesSubtreesExplicitly) {
    auto parsed = owe::ParseJson(R"({"nested":{"value":1}})"_str);
    ASSERT_TRUE(parsed.is_ok());
    auto original = parsed.unwrap();
    auto clone    = original.clone();
    auto nested   = clone.get_mut("nested"_str);
    ASSERT_TRUE(nested.is_some());
    auto object = (*nested)->as_object_mut();
    ASSERT_TRUE(object.is_some());
    (*object)->insert(::alloc::string::String::make("value"_str),
                      rstd::into<owe::Json>(rstd::i32(2)));
    EXPECT_EQ(rstd::cppstd::to_string(owe::DumpString(original).as_str()),
              R"({"nested":{"value":1}})");
    EXPECT_EQ(rstd::cppstd::to_string(owe::DumpString(clone).as_str()),
              R"({"nested":{"value":2}})");
}

TEST(UserProperty, TextInputWireValuesStayStrings) {
    auto schema =
        owe::ParseJson(R"({"type":"textinput","text":"Text","order":7,"value":"default"})"_str)
            .unwrap();

    for (const auto& raw :
         { std::string("12"), std::string("true"), std::string("提醒喝水"), std::string() }) {
        auto patch  = owe::MakeUserPropertyWirePatch(rstd::cppstd::as_str(raw).unwrap());
        auto merged = owe::MergeUserPropertyDescriptor(schema, patch);
        auto value  = merged.get("value"_str);
        ASSERT_TRUE(value.is_some());
        ASSERT_TRUE((**value).is_string());
        EXPECT_EQ(rstd::cppstd::as_string_view(*(**value).as_str()), raw);
        EXPECT_TRUE(merged.get("text"_str).is_some());
        EXPECT_TRUE(merged.get("order"_str).is_some());
    }
}

TEST(UserProperty, NonTextWireValuesKeepExistingJsonCoercion) {
    auto schema = owe::ParseJson(R"({"type":"slider","value":0})"_str).unwrap();
    auto patch  = owe::MakeUserPropertyWirePatch("1.5"_str);
    auto merged = owe::MergeUserPropertyDescriptor(schema, patch);
    auto value  = merged.get("value"_str);
    ASSERT_TRUE(value.is_some());
    EXPECT_DOUBLE_EQ((**value).as_f64().unwrap_or(rstd::f64()).to_primitive(), 1.5);
}

TEST(UserProperty, UnknownTypeDefersWireValueCoercion) {
    auto patch  = owe::MakeUserPropertyWirePatch("12"_str);
    auto merged = owe::MergeUserPropertyDescriptor(
        rstd::into<owe::Json>(rstd::string::String::make(""_str)), patch);
    auto value = merged.get("value"_str);
    ASSERT_TRUE(value.is_some());
    ASSERT_TRUE((**value).is_string());
    EXPECT_EQ(rstd::cppstd::as_string_view(*(**value).as_str()), "12");
}

TEST(JsonAdapter, NativeProjectionsPreserveOptions) {
    auto parsed = owe::ParseJson(R"({"number":1.75,"bool":true,"text":"value"})"_str);
    ASSERT_TRUE(parsed.is_ok());
    auto value  = parsed.unwrap();
    auto number = value.get("number"_str);
    ASSERT_TRUE(number.is_some());
    EXPECT_DOUBLE_EQ((*number)->as_f64().unwrap_or(rstd::f64()).to_primitive(), 1.75);
    auto boolean = value.get("bool"_str);
    ASSERT_TRUE(boolean.is_some());
    EXPECT_TRUE((*boolean)->as_bool().unwrap_or(false));
    EXPECT_TRUE((*boolean)->as_i64().is_none());
    auto text = value.get("text"_str);
    ASSERT_TRUE(text.is_some());
    EXPECT_EQ(rstd::cppstd::as_string_view(*(*text)->as_str()), "value");
    EXPECT_TRUE(value.get("missing"_str).is_none());
}

TEST(JsonAdapter, BuildsObjectsArraysAndIteratesWithoutKeyCopies) {
    auto array  = owe::MakeArray(1, true, "text");
    auto object = owe::MakeObject();
    ASSERT_TRUE(owe::SetMember(object, "items", std::move(array)));
    ASSERT_TRUE(owe::SetMember(object, "name", std::string("demo")));

    std::vector<std::string> keys;
    auto                     object_values = object.as_object();
    ASSERT_TRUE(object_values.is_some());
    (*object_values)->iter().for_each([&](auto entry) {
        auto [entry_key, entry_value] = entry;
        keys.push_back(rstd::cppstd::to_string(entry_key->as_str()));
    });
    EXPECT_EQ(keys, (std::vector<std::string> { "items", "name" }));
    EXPECT_EQ(rstd::cppstd::to_string(owe::DumpString(object).as_str()),
              R"({"items":[1,true,"text"],"name":"demo"})");
}

TEST(JsonAdapter, ProductionOutputContractsRoundTrip) {
    auto property = owe::MakeObject();
    ASSERT_TRUE(owe::SetMember(property, "dynamic\"\\", "line\n\t"));

    const auto compact = rstd::cppstd::to_string(owe::DumpString(property).as_str());
    EXPECT_EQ(compact, R"({"dynamic\"\\":"line\n\t"})");

    auto parsed = owe::ParseJson(rstd::cppstd::as_str(compact).unwrap());
    ASSERT_TRUE(parsed.is_ok());
    auto reparsed = parsed.unwrap();
    EXPECT_EQ(rstd::cppstd::to_string(owe::DumpString(reparsed).as_str()), compact);
}

TEST(JsonAdapter, LegacyGetJsonValueReadsScalarAndNamedValues) {
    auto parsed = owe::ParseJson(
        R"({"bound":{"value":12.75},"plain":7,"text":"hello","flag":true,"null":null})"_str);
    ASSERT_TRUE(parsed.is_ok());
    auto json = parsed.unwrap();

    float bound = 0.0f;
    EXPECT_TRUE(owe::GetJsonValue(json, "bound"_str, bound));
    EXPECT_FLOAT_EQ(bound, 12.75f);

    std::int32_t plain = 0;
    EXPECT_TRUE(owe::GetJsonValue(json, "plain"_str, plain));
    EXPECT_EQ(plain, 7);

    String text;
    EXPECT_TRUE(owe::GetJsonValue(json, "text"_str, text));
    EXPECT_EQ(text, "hello"_str);

    bool flag = false;
    EXPECT_TRUE(owe::GetJsonValue(json, "flag"_str, flag));
    EXPECT_TRUE(flag);

    std::int32_t unchanged = 41;
    EXPECT_FALSE(owe::GetJsonValue(json, "missing"_str, unchanged, false));
    EXPECT_EQ(unchanged, 41);
    EXPECT_FALSE(owe::GetJsonValue(json, "null"_str, unchanged, false));
    EXPECT_EQ(unchanged, 41);
}

TEST(JsonAdapter, LegacyGetJsonValuePreservesNumericConversions) {
    auto floating = owe::ParseJson("3.75"_str);
    ASSERT_TRUE(floating.is_ok());
    std::int32_t integer = 0;
    EXPECT_TRUE(owe::GetJsonValue(floating.unwrap(), integer));
    EXPECT_EQ(integer, 3);

    auto negative = owe::ParseJson("-1"_str);
    ASSERT_TRUE(negative.is_ok());
    std::uint32_t unsigned_integer = 0;
    EXPECT_TRUE(owe::GetJsonValue(negative.unwrap(), unsigned_integer));
    EXPECT_EQ(unsigned_integer, std::numeric_limits<std::uint32_t>::max());

    auto boolean = owe::ParseJson("true"_str);
    ASSERT_TRUE(boolean.is_ok());
    double numeric_boolean = 0.0;
    EXPECT_TRUE(owe::GetJsonValue(boolean.unwrap(), numeric_boolean));
    EXPECT_DOUBLE_EQ(numeric_boolean, 1.0);
}

TEST(JsonAdapter, LegacyGetJsonValueReadsArrayFormats) {
    auto parsed =
        owe::ParseJson(R"({"vector":"1.5 2.5 3.5","pair":"8 9","single":4,"ints":"1 -2 3"})"_str);
    ASSERT_TRUE(parsed.is_ok());
    auto json = parsed.unwrap();

    rstd::array<float, 3> fixed {};
    EXPECT_TRUE(owe::GetJsonValue(json, "vector"_str, fixed));
    EXPECT_EQ(fixed, (rstd::array<float, 3> { 1.5f, 2.5f, 3.5f }));

    Vec<float> dynamic;
    for (float value : { 9.0f, 8.0f, 7.0f, 6.0f }) dynamic.push(rstd::move(value));
    EXPECT_TRUE(owe::GetJsonValue(json, "vector"_str, dynamic));
    EXPECT_EQ(dynamic.as_slice(), (rstd::array<float, 4> { 1.5f, 2.5f, 3.5f, 6.0f }).as_slice());

    EXPECT_TRUE(owe::GetJsonValue(json, "single"_str, fixed));
    EXPECT_EQ(fixed, (rstd::array<float, 3> { 4.0f, 0.0f, 0.0f }));

    rstd::array<float, 2> pair {};
    EXPECT_TRUE(owe::GetJsonValue(json, "pair"_str, pair));
    EXPECT_EQ(pair, (rstd::array<float, 2> { 8.0f, 9.0f }));

    rstd::array<int, 3> fixed_integers {};
    EXPECT_TRUE(owe::GetJsonValue(json, "ints"_str, fixed_integers));
    EXPECT_EQ(fixed_integers, (rstd::array<int, 3> { 1, -2, 3 }));

    Vec<rstd::i32> integers;
    EXPECT_TRUE(owe::GetJsonValue(json, "ints"_str, integers));
    EXPECT_EQ(integers.as_slice(),
              (rstd::array<rstd::i32, 3> { rstd::i32(1), rstd::i32(-2), rstd::i32(3) }).as_slice());
}

TEST(JsonAdapter, LegacyGetJsonValueReportsConversionFailure) {
    auto wrong_size = owe::ParseJson(R"("1 2")"_str);
    ASSERT_TRUE(wrong_size.is_ok());
    rstd::array<float, 3> fixed { 7.0f, 8.0f, 9.0f };
    EXPECT_FALSE(owe::GetJsonValue(wrong_size.unwrap(), fixed));
    EXPECT_EQ(fixed, (rstd::array<float, 3> { 7.0f, 8.0f, 9.0f }));

    auto wrong_type = owe::ParseJson(R"("not a number")"_str);
    ASSERT_TRUE(wrong_type.is_ok());
    double number = 2.0;
    EXPECT_FALSE(owe::GetJsonValue(wrong_type.unwrap(), number));
    EXPECT_DOUBLE_EQ(number, 2.0);
}

TEST(JsonAdapter, ReadsOwnedNativeStringAndPreservesOnTypeError) {
    auto value  = owe::ParseJson(R"({"text":"a\u0000b","wrong":false})"_str).unwrap();
    auto output = "old"_Str;
    ASSERT_TRUE(owe::GetJsonValue(value, "text"_str, output, false));
    EXPECT_EQ(output, "a\0b"_str);
    EXPECT_FALSE(owe::GetJsonValue(value, "wrong"_str, output, false));
    EXPECT_EQ(output, "a\0b"_str);
    EXPECT_FALSE(owe::GetJsonValue(value, "missing"_str, output, false));
    EXPECT_EQ(output, "a\0b"_str);
    ASSERT_TRUE(
        owe::GetJsonValue(rstd::into<owe::Json>(rstd::string::String::make(""_str)), output));
    EXPECT_TRUE(output.is_empty());
}

TEST(JsonAdapter, NativeNumericVectorsPreserveExistingSlotsAndFailureState) {
    using namespace rstd::prelude;
    struct Case {
        ref<str>        source;
        bool            success;
        usize           empty_len;
        array<float, 4> empty;
        usize           filled_len;
        array<float, 4> filled;
    };
    const Case cases[] {
        { R"("1.5 2.5")"_str,
          true,
          usize(2),
          { 1.5f, 2.5f, 0.0f, 0.0f },
          usize(4),
          { 1.5f, 2.5f, 7.0f, 6.0f } },
        { R"("1 bad 3")"_str,
          false,
          usize(3),
          { 1.0f, 0.0f, 0.0f, 0.0f },
          usize(4),
          { 1.0f, 8.0f, 7.0f, 6.0f } },
        { R"("1  3")"_str,
          false,
          usize(3),
          { 1.0f, 0.0f, 0.0f, 0.0f },
          usize(4),
          { 1.0f, 8.0f, 7.0f, 6.0f } },
        { R"(" 1")"_str, false, usize(2), {}, usize(4), { 9.0f, 8.0f, 7.0f, 6.0f } },
        { R"("1 ")"_str,
          false,
          usize(2),
          { 1.0f, 0.0f, 0.0f, 0.0f },
          usize(4),
          { 1.0f, 8.0f, 7.0f, 6.0f } },
        { R"("")"_str, false, usize(1), {}, usize(4), { 9.0f, 8.0f, 7.0f, 6.0f } },
        { R"("1e999 2")"_str, false, usize(2), {}, usize(4), { 9.0f, 8.0f, 7.0f, 6.0f } },
        { R"("2suffix 3")"_str, false, usize(2), {}, usize(4), { 9.0f, 8.0f, 7.0f, 6.0f } },
        { R"("2\u0000suffix 3")"_str, false, usize(2), {}, usize(4), { 9.0f, 8.0f, 7.0f, 6.0f } },
        { R"([1,2])"_str,
          true,
          usize(2),
          { 1.0f, 2.0f, 0.0f, 0.0f },
          usize(2),
          { 1.0f, 2.0f, 0.0f, 0.0f } },
        { R"([1,"bad",3])"_str,
          false,
          usize(1),
          { 1.0f, 0.0f, 0.0f, 0.0f },
          usize(1),
          { 1.0f, 0.0f, 0.0f, 0.0f } },
        { "4"_str,
          true,
          usize(1),
          { 4.0f, 0.0f, 0.0f, 0.0f },
          usize(1),
          { 4.0f, 0.0f, 0.0f, 0.0f } },
    };
    for (const auto& input : cases) {
        SCOPED_TRACE(input.source);
        auto json = owe::ParseJson(input.source).unwrap();
        for (bool prefilled : { false, true }) {
            Vec<float> values;
            if (prefilled)
                for (float value : { 9.0f, 8.0f, 7.0f, 6.0f }) values.push(rstd::move(value));
            EXPECT_EQ(owe::GetJsonValue(json, values), input.success);
            ASSERT_EQ(values.len(), prefilled ? input.filled_len : input.empty_len);
            const auto& expected = prefilled ? input.filled : input.empty;
            for (usize i {}; i < values.len(); ++i) EXPECT_FLOAT_EQ(values[i], expected[i]);
        }
    }
}

TEST(JsonAdapter, NativeFixedArraysPreservePartialWritesAndNumericGrammar) {
    using namespace rstd::prelude;
    struct Case {
        const char*     source;
        bool            success;
        array<float, 3> expected;
    };
    const Case cases[] {
        { R"("1 2")", false, { 7.0f, 8.0f, 9.0f } },
        { R"("1 2 3 4")", false, { 7.0f, 8.0f, 9.0f } },
        { R"("1 bad 3")", false, { 1.0f, 8.0f, 9.0f } },
        { R"("1  3")", false, { 1.0f, 8.0f, 9.0f } },
        { R"("1 2 ")", false, { 1.0f, 2.0f, 9.0f } },
        { R"("1 1e999 3")", false, { 1.0f, 8.0f, 9.0f } },
        { R"("2suffix 3 4")", false, { 7.0f, 8.0f, 9.0f } },
        { R"("2\u0000suffix 3 4")", false, { 7.0f, 8.0f, 9.0f } },
        { R"([1,2])", false, { 1.0f, 2.0f, 9.0f } },
        { R"([1,2,3,4])", false, { 1.0f, 2.0f, 3.0f } },
        { R"([1,"bad",3])", false, { 1.0f, 8.0f, 9.0f } },
        { R"(4)", true, { 4.0f, 0.0f, 0.0f } },
    };
    for (const auto& input : cases) {
        SCOPED_TRACE(input.source);
        auto            json = owe::ParseJson(rstd::cppstd::as_str(input.source).unwrap()).unwrap();
        array<float, 3> target { 7.0f, 8.0f, 9.0f };
        EXPECT_EQ(owe::GetJsonValue(json, target), input.success);
        EXPECT_EQ(target, input.expected);
    }
    array<i32, 3> signs {};
    EXPECT_TRUE(owe::GetJsonValue(owe::ParseJson(R"("1 -2 3")"_str).unwrap(), signs));
    EXPECT_EQ(signs, (array<i32, 3> { i32(1), i32(-2), i32(3) }));
}

TEST(JsonAdapter, CommaArraysUseStrictNumbersAndPreserveFailureWrites) {
    using namespace rstd::prelude;
    struct Case {
        ref<str>        source;
        bool            success;
        array<float, 3> expected;
    };
    for (const auto& item : array<Case, 10> {
             Case { "1,2,3"_str, true, { 1.0f, 2.0f, 3.0f } },
             Case { " 1 ,\t-2, 3e-1\n"_str, true, { 1.0f, -2.0f, 0.3f } },
             Case { "1,,3"_str, false, { 1.0f, 8.0f, 9.0f } },
             Case { ",2,3"_str, false, { 7.0f, 8.0f, 9.0f } },
             Case { "1,2,"_str, false, { 1.0f, 2.0f, 9.0f } },
             Case { "1,2suffix,3"_str, false, { 1.0f, 8.0f, 9.0f } },
             Case { "1,2\0suffix,3"_str, false, { 1.0f, 8.0f, 9.0f } },
             Case { "1,1e999,3"_str, false, { 1.0f, 8.0f, 9.0f } },
             Case { "1,2 3,4"_str, false, { 1.0f, 8.0f, 9.0f } },
             Case { "1, ,3"_str, false, { 1.0f, 8.0f, 9.0f } },
         }) {
        SCOPED_TRACE(item.source);
        auto            json = owe::Json::String(String::make(item.source));
        array<float, 3> fixed { 7.0f, 8.0f, 9.0f };
        auto            dynamic = Vec<float>::from(fixed.as_slice());
        dynamic.push(10.0f);
        EXPECT_EQ(owe::GetJsonValue(json, fixed), item.success);
        EXPECT_EQ(owe::GetJsonValue(json, dynamic), item.success);
        EXPECT_EQ(fixed, item.expected);
        ASSERT_EQ(dynamic.len(), usize(4));
        for (usize i; i < usize(3); ++i) EXPECT_FLOAT_EQ(dynamic[i], item.expected[i]);
        EXPECT_FLOAT_EQ(dynamic[usize(3)], 10.0f);
    }
    array<float, 3> wrong_size { 7.0f, 8.0f, 9.0f };
    EXPECT_FALSE(owe::GetJsonValue(owe::Json::String("1,2"_Str), wrong_size));
    EXPECT_EQ(wrong_size, (array<float, 3> { 7.0f, 8.0f, 9.0f }));
    Vec<i32> integers;
    EXPECT_TRUE(owe::GetJsonValue(owe::Json::String("1, -2, 3"_Str), integers));
    EXPECT_EQ(integers.as_slice(), (array<i32, 3> { i32(1), i32(-2), i32(3) }).as_slice());
    Vec<float> empty;
    EXPECT_FALSE(owe::GetJsonValue(owe::Json::String("1,,3"_Str), empty));
    EXPECT_EQ(empty.as_slice(), (array<float, 3> { 1.0f, 0.0f, 0.0f }).as_slice());
    EXPECT_TRUE(owe::ParseJsonFloat("1,2"_str).is_err());
}

TEST(JsonAdapter, NativeFieldNamesBorrowExactBytesAndPreserveFailures) {
    using namespace rstd::prelude;
    auto json =
        owe::ParseJson(R"({"a":1,"a\u0000b":2,"\u5b57\u6bb5":3,"wrong":false,"null":null})"_str)
            .unwrap();
    i32  value {};
    auto key = "a\0b"_Str;
    EXPECT_TRUE(owe::GetJsonValue(json, key.as_str(), value));
    EXPECT_EQ(value, i32(2));
    EXPECT_TRUE(owe::GetJsonValue(json, "\u5b57\u6bb5"_str, value));
    EXPECT_EQ(value, i32(3));
    EXPECT_FALSE(owe::GetJsonValue(json, "missing"_str, value, false));
    EXPECT_FALSE(owe::GetJsonValue(json, "null"_str, value, false));
    EXPECT_EQ(value, i32(3));
    String text = "unchanged"_Str;
    EXPECT_FALSE(owe::GetJsonValue(json, "wrong"_str, text, false));
    EXPECT_EQ(text, "unchanged"_str);
}

TEST(JsonAdapter, NativeNumbersUseRstdGrammarAndRange) {
    for (auto input : { ""_str,
                        "invalid"_str,
                        " 1.25"_str,
                        "1.25 "_str,
                        "2suffix"_str,
                        "0x1.8p2"_str,
                        "1e+"_str,
                        "-INFINITY"_str,
                        "nan(payload)"_str,
                        "2.5\0suffix"_str }) {
        SCOPED_TRACE(input);
        auto result = owe::ParseJsonFloat(input);
        ASSERT_TRUE(result.is_err());
        EXPECT_EQ(result.unwrap_err(), owe::JsonValueError::InvalidNumber);
    }
    for (auto input : { "1e1000"_str, "-1e1000"_str, "3.4028236e38"_str }) {
        SCOPED_TRACE(input);
        auto result = owe::ParseJsonFloat(input);
        ASSERT_TRUE(result.is_err());
        EXPECT_EQ(result.unwrap_err(), owe::JsonValueError::NumberOutOfRange);
    }
    EXPECT_EQ(owe::ParseJsonFloat("+1.25"_str).unwrap(), 1.25f);
    EXPECT_TRUE(f32(owe::ParseJsonFloat("-0"_str).unwrap()).is_sign_negative());
    EXPECT_TRUE(f32(owe::ParseJsonFloat("NaN"_str).unwrap()).is_nan());
    EXPECT_EQ(owe::ParseJsonFloat("inf"_str).unwrap(), f32::INFINITY_.to_primitive());
    EXPECT_EQ(owe::ParseJsonFloat("-infinity"_str).unwrap(), (-f32::INFINITY_).to_primitive());
    EXPECT_EQ(owe::ParseJsonFloat("1e-1000"_str).unwrap(), 0.0f);
    EXPECT_GT(owe::ParseJsonFloat("1e-40"_str).unwrap(), 0.0f);
    EXPECT_EQ(owe::ParseJsonFloat("3.4028235e38"_str).unwrap(), f32::MAX.to_primitive());

    array<i32, 3> signed_values { i32(7), i32(8), i32(9) };
    EXPECT_TRUE(owe::GetJsonValue(owe::ParseJson(R"("+2147483647 -2147483648 0")"_str).unwrap(),
                                  signed_values));
    EXPECT_EQ(signed_values, (array<i32, 3> { i32::MAX, i32::MIN, i32() }));
    for (auto input :
         { R"("1 2147483648 3")"_str, R"("1 -2147483649 3")"_str, R"("1 2suffix 3")"_str }) {
        signed_values = { i32(7), i32(8), i32(9) };
        EXPECT_FALSE(owe::GetJsonValue(owe::ParseJson(input).unwrap(), signed_values));
        EXPECT_EQ(signed_values, (array<i32, 3> { i32(1), i32(8), i32(9) }));
    }
    Vec<u32> unsigned_values;
    EXPECT_TRUE(
        owe::GetJsonValue(owe::ParseJson(R"("+4294967295 0")"_str).unwrap(), unsigned_values));
    ASSERT_EQ(unsigned_values.len(), usize(2));
    EXPECT_EQ(unsigned_values[usize()], u32::MAX);
    for (auto input : { R"("4294967296")"_str, R"("-1")"_str }) {
        EXPECT_FALSE(owe::GetJsonValue(owe::ParseJson(input).unwrap(), unsigned_values));
        EXPECT_EQ(unsigned_values[usize()], u32::MAX);
    }
}
