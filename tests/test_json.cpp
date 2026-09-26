#include "common/json.h"
#include "ixc_test.h"

#include <cmath>
#include <string>

using namespace ixc;

IXC_TEST(Json_ParsesScalars) {
    auto r = json::Parse("  {\"a\": 1, \"b\": -2.5e2, \"c\": true, \"d\": null, \"e\": \"x\"} ");
    IXC_REQUIRE(r);
    const auto& v = *r.value;
    IXC_CHECK_EQ(v.Find("a")->AsNumber(), 1.0);
    IXC_CHECK_EQ(v.Find("b")->AsNumber(), -250.0);
    IXC_CHECK(v.Find("c")->AsBool());
    IXC_CHECK(v.Find("d")->IsNull());
    IXC_CHECK_EQ(v.Find("e")->AsString(), std::string("x"));
    IXC_CHECK(v.Find("zzz") == nullptr);
}

IXC_TEST(Json_DecodesEscapesAndSurrogatePairs) {
    auto r = json::Parse(R"("a\"\\\/\b\f\n\r\t\u00e9\ud83d\ude00")");
    IXC_REQUIRE(r);
    IXC_CHECK_EQ(r.value->AsString(), std::string("a\"\\/\b\f\n\r\t\xC3\xA9\xF0\x9F\x98\x80"));
}

IXC_TEST(Json_RejectsMalformedInput) {
    const char* bad[] = {
        "",                    // empty
        "{",                   // unterminated
        "[1,]",                // trailing comma
        "{\"a\":1,}",          // trailing comma in object
        "// c\n1",             // comment
        "01",                  // leading zero
        "1.",                  // missing fraction digits
        "-",                   // lone minus
        "NaN",                 // not JSON
        "Infinity",            //
        "1e400",               // overflows double
        "\"\\x41\"",           // bad escape
        "\"\\ud800\"",         // lone high surrogate
        "\"\\udc00\"",         // lone low surrogate
        "\"a\nb\"",            // raw control char
        "{\"a\":1,\"a\":2}",   // duplicate key
        "[1] [2]",             // trailing content
        "{a:1}",               // unquoted key
        "'x'",                 // single quotes
        "tru",                 // truncated literal
        "\"\xC0\xAF\"",        // overlong UTF-8
        "\"\xED\xA0\x80\"",    // UTF-8 encoded surrogate
        "\"\xFF\"",            // invalid UTF-8 byte
    };
    for (const char* text : bad) {
        auto r = json::Parse(text);
        if (r) ixc::test::ReportFailure(__FILE__, __LINE__, std::string("accepted malformed JSON: ") + text);
    }
}

IXC_TEST(Json_EnforcesDepthLimit) {
    json::ParseLimits limits;
    limits.maxDepth = 8;
    IXC_CHECK(json::Parse("[[[[[[[[1]]]]]]]]", limits));        // depth 8 ok
    IXC_CHECK(!json::Parse("[[[[[[[[[1]]]]]]]]]", limits));     // depth 9 rejected

    // Deep nesting well beyond the default limit must fail cleanly, not overflow the stack.
    std::string deep(100000, '[');
    IXC_CHECK(!json::Parse(deep));
}

IXC_TEST(Json_EnforcesSizeLimit) {
    json::ParseLimits limits;
    limits.maxInputBytes = 16;
    IXC_CHECK(json::Parse("\"0123456789\"", limits));
    IXC_CHECK(!json::Parse("\"0123456789abcdefgh\"", limits));
}

IXC_TEST(Json_AcceptsUtf8Bom) {
    auto r = json::Parse("\xEF\xBB\xBF{\"a\":1}");
    IXC_REQUIRE(r);
    IXC_CHECK_EQ(r.value->Find("a")->AsNumber(), 1.0);
}

IXC_TEST(Json_RoundTripsThroughSerialize) {
    json::Object o = {
        {"int", 42},
        {"neg", -7},
        {"frac", 0.1},
        {"tiny", 1e-300},
        {"str", "line\nbreak \"quoted\" \x01 \xC3\xA9"},
        {"arr", json::Array{1, true, nullptr, "x"}},
        {"obj", json::Object{{"k", "v"}}},
        {"emptyArr", json::Array{}},
        {"emptyObj", json::Object{}},
    };
    const std::string text = json::Serialize(json::Value(o));
    auto r = json::Parse(text);
    IXC_REQUIRE(r);
    IXC_CHECK_EQ(json::Serialize(*r.value), text);  // stable output
    IXC_CHECK_EQ(r.value->Find("frac")->AsNumber(), 0.1);
    IXC_CHECK_EQ(r.value->Find("tiny")->AsNumber(), 1e-300);
    IXC_CHECK_EQ(r.value->Find("str")->AsString(), std::string("line\nbreak \"quoted\" \x01 \xC3\xA9"));
    IXC_CHECK(text.find("\"int\": 42,") != std::string::npos);  // integers print without fraction
}

IXC_TEST(Json_SerializesNonFiniteAsNull) {
    const std::string text = json::Serialize(json::Value(json::Array{std::nan(""), HUGE_VAL}));
    auto r = json::Parse(text);
    IXC_REQUIRE(r);
    IXC_CHECK(r.value->AsArray()[0].IsNull());
    IXC_CHECK(r.value->AsArray()[1].IsNull());
}

IXC_TEST(Json_ValueCopyIsDeep) {
    json::Value a(json::Object{{"x", json::Array{1, 2}}});
    json::Value b = a;
    b.MutableObject()[0].second.MutableArray().push_back(3);
    IXC_CHECK_EQ(a.Find("x")->AsArray().size(), size_t{2});
    IXC_CHECK_EQ(b.Find("x")->AsArray().size(), size_t{3});
}
