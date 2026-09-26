#pragma once

// Minimal strict RFC 8259 JSON reader/writer used for profiles and effect manifests.
//
// These files are untrusted input (users import profiles and effect packages), so the parser:
//   * rejects comments, trailing commas, NaN/Infinity, leading zeros and duplicate object keys;
//   * requires well-formed UTF-8 and rejects lone surrogate escapes;
//   * enforces a nesting-depth limit and an input-size limit;
//   * never throws: failures come back as a ParseError with a byte offset.

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ixc::json {

class Value;
using Array = std::vector<Value>;
using Member = std::pair<std::string, Value>;
using Object = std::vector<Member>;  // insertion order preserved for stable output

enum class Type { Null, Bool, Number, String, Array, Object };

class Value {
public:
    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : type_(Type::Bool), bool_(b) {}
    Value(double d) : type_(Type::Number), num_(d) {}
    Value(int i) : type_(Type::Number), num_(i) {}
    Value(const char* s) : type_(Type::String), str_(s) {}
    Value(std::string s) : type_(Type::String), str_(std::move(s)) {}
    Value(Array a);
    Value(Object o);

    Value(const Value& other);
    Value(Value&&) noexcept = default;
    Value& operator=(const Value& other);
    Value& operator=(Value&&) noexcept = default;
    ~Value() = default;

    Type type() const { return type_; }
    bool IsNull() const { return type_ == Type::Null; }
    bool IsBool() const { return type_ == Type::Bool; }
    bool IsNumber() const { return type_ == Type::Number; }
    bool IsString() const { return type_ == Type::String; }
    bool IsArray() const { return type_ == Type::Array; }
    bool IsObject() const { return type_ == Type::Object; }

    bool AsBool() const { return bool_; }
    double AsNumber() const { return num_; }
    const std::string& AsString() const { return str_; }
    const Array& AsArray() const;
    const Object& AsObject() const;
    Array& MutableArray();
    Object& MutableObject();

    // Object lookup; returns nullptr when this is not an object or the key is absent.
    const Value* Find(std::string_view key) const;
    // Appends or replaces a member (object values only; converts null to an empty object).
    void Set(std::string key, Value v);

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0.0;
    std::string str_;
    std::unique_ptr<Array> arr_;
    std::unique_ptr<Object> obj_;
};

struct ParseLimits {
    size_t maxInputBytes = 1u << 20;  // 1 MiB
    int maxDepth = 32;
};

struct ParseError {
    size_t offset = 0;
    std::string message;
};

struct ParseResult {
    std::optional<Value> value;
    ParseError error;
    explicit operator bool() const { return value.has_value(); }
};

ParseResult Parse(std::string_view text, const ParseLimits& limits = {});

// Pretty-printed, deterministic output with a trailing newline. Non-finite numbers become null.
std::string Serialize(const Value& v);

}  // namespace ixc::json
