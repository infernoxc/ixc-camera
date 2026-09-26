#include "common/json.h"

#include "common/strings.h"

#include <charconv>
#include <cmath>
#include <string>
#include <unordered_set>

namespace ixc::json {

namespace {
const Array kEmptyArray;
const Object kEmptyObject;
}  // namespace

Value::Value(Array a) : type_(Type::Array), arr_(std::make_unique<Array>(std::move(a))) {}
Value::Value(Object o) : type_(Type::Object), obj_(std::make_unique<Object>(std::move(o))) {}

Value::Value(const Value& other)
    : type_(other.type_), bool_(other.bool_), num_(other.num_), str_(other.str_) {
    if (other.arr_) arr_ = std::make_unique<Array>(*other.arr_);
    if (other.obj_) obj_ = std::make_unique<Object>(*other.obj_);
}

Value& Value::operator=(const Value& other) {
    if (this != &other) {
        Value copy(other);
        *this = std::move(copy);
    }
    return *this;
}

const Array& Value::AsArray() const { return arr_ ? *arr_ : kEmptyArray; }
const Object& Value::AsObject() const { return obj_ ? *obj_ : kEmptyObject; }

Array& Value::MutableArray() {
    if (type_ != Type::Array) { *this = Value(Array{}); }
    return *arr_;
}

Object& Value::MutableObject() {
    if (type_ != Type::Object) { *this = Value(Object{}); }
    return *obj_;
}

const Value* Value::Find(std::string_view key) const {
    if (type_ != Type::Object || !obj_) return nullptr;
    for (const auto& [k, v] : *obj_) {
        if (k == key) return &v;
    }
    return nullptr;
}

void Value::Set(std::string key, Value v) {
    Object& o = MutableObject();
    for (auto& [k, existing] : o) {
        if (k == key) { existing = std::move(v); return; }
    }
    o.emplace_back(std::move(key), std::move(v));
}

// ---------------------------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------------------------
namespace {

class Parser {
public:
    Parser(std::string_view text, const ParseLimits& limits) : s_(text), limits_(limits) {}

    ParseResult Run() {
        ParseResult r;
        if (s_.size() > limits_.maxInputBytes) {
            r.error = {0, "input exceeds size limit"};
            return r;
        }
        if (!IsValidUtf8(s_)) {
            r.error = {0, "input is not valid UTF-8"};
            return r;
        }
        // Tolerate a UTF-8 BOM (Notepad writes one); it is not part of the document.
        if (s_.substr(0, 3) == "\xEF\xBB\xBF") pos_ = 3;

        Value v;
        if (!ParseValue(v, 0)) { r.error = err_; return r; }
        SkipWs();
        if (pos_ != s_.size()) {
            r.error = {pos_, "unexpected trailing content"};
            return r;
        }
        r.value = std::move(v);
        return r;
    }

private:
    bool Fail(const char* msg) {
        err_ = {pos_, msg};
        return false;
    }

    void SkipWs() {
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
            else break;
        }
    }

    bool Consume(std::string_view lit) {
        if (s_.substr(pos_, lit.size()) == lit) { pos_ += lit.size(); return true; }
        return false;
    }

    bool ParseValue(Value& out, int depth) {
        if (depth > limits_.maxDepth) return Fail("nesting too deep");
        SkipWs();
        if (pos_ >= s_.size()) return Fail("unexpected end of input");
        switch (s_[pos_]) {
            case '{': return ParseObject(out, depth);
            case '[': return ParseArray(out, depth);
            case '"': {
                std::string str;
                if (!ParseString(str)) return false;
                out = Value(std::move(str));
                return true;
            }
            case 't': if (Consume("true")) { out = Value(true); return true; } break;
            case 'f': if (Consume("false")) { out = Value(false); return true; } break;
            case 'n': if (Consume("null")) { out = Value(nullptr); return true; } break;
            default: return ParseNumber(out);
        }
        return Fail("invalid literal");
    }

    bool ParseObject(Value& out, int depth) {
        ++pos_;  // '{'
        Object obj;
        std::unordered_set<std::string> seen;
        SkipWs();
        if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; out = Value(std::move(obj)); return true; }
        for (;;) {
            SkipWs();
            if (pos_ >= s_.size() || s_[pos_] != '"') return Fail("expected object key");
            std::string key;
            if (!ParseString(key)) return false;
            if (!seen.insert(key).second) return Fail("duplicate object key");
            SkipWs();
            if (pos_ >= s_.size() || s_[pos_] != ':') return Fail("expected ':'");
            ++pos_;
            Value v;
            if (!ParseValue(v, depth + 1)) return false;
            obj.emplace_back(std::move(key), std::move(v));
            SkipWs();
            if (pos_ >= s_.size()) return Fail("unterminated object");
            if (s_[pos_] == ',') { ++pos_; continue; }
            if (s_[pos_] == '}') { ++pos_; break; }
            return Fail("expected ',' or '}'");
        }
        out = Value(std::move(obj));
        return true;
    }

    bool ParseArray(Value& out, int depth) {
        ++pos_;  // '['
        Array arr;
        SkipWs();
        if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; out = Value(std::move(arr)); return true; }
        for (;;) {
            Value v;
            if (!ParseValue(v, depth + 1)) return false;
            arr.push_back(std::move(v));
            SkipWs();
            if (pos_ >= s_.size()) return Fail("unterminated array");
            if (s_[pos_] == ',') { ++pos_; continue; }
            if (s_[pos_] == ']') { ++pos_; break; }
            return Fail("expected ',' or ']'");
        }
        out = Value(std::move(arr));
        return true;
    }

    bool ParseHex4(unsigned& out) {
        if (s_.size() - pos_ < 4) return Fail("truncated \\u escape");
        unsigned v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else return Fail("invalid hex digit in \\u escape");
        }
        out = v;
        return true;
    }

    static void AppendUtf8(std::string& out, char32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool ParseString(std::string& out) {
        ++pos_;  // opening quote
        for (;;) {
            if (pos_ >= s_.size()) return Fail("unterminated string");
            const char c = s_[pos_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return Fail("control character in string");
            if (c != '\\') { out.push_back(c); continue; }

            if (pos_ >= s_.size()) return Fail("unterminated escape");
            const char e = s_[pos_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    unsigned hi = 0;
                    if (!ParseHex4(hi)) return false;
                    char32_t cp = hi;
                    if (hi >= 0xD800 && hi <= 0xDBFF) {
                        if (!Consume("\\u")) return Fail("unpaired high surrogate");
                        unsigned lo = 0;
                        if (!ParseHex4(lo)) return false;
                        if (lo < 0xDC00 || lo > 0xDFFF) return Fail("invalid low surrogate");
                        cp = 0x10000 + ((hi - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (hi >= 0xDC00 && hi <= 0xDFFF) {
                        return Fail("unpaired low surrogate");
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default: return Fail("invalid escape");
            }
        }
    }

    bool ParseNumber(Value& out) {
        const size_t start = pos_;
        auto digit = [&](size_t p) { return p < s_.size() && s_[p] >= '0' && s_[p] <= '9'; };

        if (pos_ < s_.size() && s_[pos_] == '-') ++pos_;
        if (!digit(pos_)) return Fail("invalid value");
        if (s_[pos_] == '0') {
            ++pos_;
            if (digit(pos_)) return Fail("leading zero in number");
        } else {
            while (digit(pos_)) ++pos_;
        }
        if (pos_ < s_.size() && s_[pos_] == '.') {
            ++pos_;
            if (!digit(pos_)) return Fail("expected digit after '.'");
            while (digit(pos_)) ++pos_;
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
            if (!digit(pos_)) return Fail("expected digit in exponent");
            while (digit(pos_)) ++pos_;
        }

        double d = 0.0;
        const char* first = s_.data() + start;
        const char* last = s_.data() + pos_;
        const auto [ptr, ec] = std::from_chars(first, last, d);
        if (ec != std::errc() || ptr != last || !std::isfinite(d)) {
            pos_ = start;
            return Fail("number out of range");
        }
        out = Value(d);
        return true;
    }

    std::string_view s_;
    ParseLimits limits_;
    size_t pos_ = 0;
    ParseError err_;
};

// ---------------------------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------------------------
void WriteString(std::string& out, const std::string& s) {
    static constexpr char kHex[] = "0123456789abcdef";
    out.push_back('"');
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out.push_back(kHex[c >> 4]);
                    out.push_back(kHex[c & 0xF]);
                } else {
                    out.push_back(ch);
                }
        }
    }
    out.push_back('"');
}

void WriteNumber(std::string& out, double d) {
    if (!std::isfinite(d)) { out += "null"; return; }
    char buf[32];
    // Whole numbers inside the exactly-representable range print without a fraction.
    if (d == std::floor(d) && std::fabs(d) < 9007199254740992.0) {
        const auto [p, ec] = std::to_chars(buf, buf + sizeof(buf), static_cast<long long>(d));
        if (ec == std::errc()) { out.append(buf, p); return; }
    }
    const auto [p, ec] = std::to_chars(buf, buf + sizeof(buf), d);  // shortest round-trip form
    if (ec == std::errc()) out.append(buf, p);
    else out += "null";
}

void Indent(std::string& out, int level) { out.append(static_cast<size_t>(level) * 2, ' '); }

void WriteValue(std::string& out, const Value& v, int level) {
    switch (v.type()) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += v.AsBool() ? "true" : "false"; break;
        case Type::Number: WriteNumber(out, v.AsNumber()); break;
        case Type::String: WriteString(out, v.AsString()); break;
        case Type::Array: {
            const Array& a = v.AsArray();
            if (a.empty()) { out += "[]"; break; }
            out += "[\n";
            for (size_t i = 0; i < a.size(); ++i) {
                Indent(out, level + 1);
                WriteValue(out, a[i], level + 1);
                out += (i + 1 < a.size()) ? ",\n" : "\n";
            }
            Indent(out, level);
            out.push_back(']');
            break;
        }
        case Type::Object: {
            const Object& o = v.AsObject();
            if (o.empty()) { out += "{}"; break; }
            out += "{\n";
            for (size_t i = 0; i < o.size(); ++i) {
                Indent(out, level + 1);
                WriteString(out, o[i].first);
                out += ": ";
                WriteValue(out, o[i].second, level + 1);
                out += (i + 1 < o.size()) ? ",\n" : "\n";
            }
            Indent(out, level);
            out.push_back('}');
            break;
        }
    }
}

}  // namespace

ParseResult Parse(std::string_view text, const ParseLimits& limits) {
    return Parser(text, limits).Run();
}

std::string Serialize(const Value& v) {
    std::string out;
    WriteValue(out, v, 0);
    out.push_back('\n');
    return out;
}

}  // namespace ixc::json
