#include "json.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace openshow::ndi::json {

namespace {

constexpr int kMaxDepth = 32;

bool fail(std::string* error, const std::string& message, std::size_t pos) {
    if (error) {
        *error = message + " at byte " + std::to_string(pos);
    }
    return false;
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

void appendUtf8(std::string& out, unsigned cp) {
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

void skipSpace(const std::string& text, std::size_t& i) {
    while (i < text.size()) {
        const char c = text[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            ++i;
        } else {
            break;
        }
    }
}

bool parseString(const std::string& text, std::size_t& i, std::string* out, std::string* error) {
    if (i >= text.size() || text[i] != '"') {
        return fail(error, "expected a string", i);
    }
    ++i;
    std::string value;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '"') {
            ++i;
            *out = std::move(value);
            return true;
        }
        if (c == '\\') {
            ++i;
            if (i >= text.size()) {
                return fail(error, "unfinished escape", i);
            }
            switch (text[i]) {
                case '"': value.push_back('"'); ++i; break;
                case '\\': value.push_back('\\'); ++i; break;
                case '/': value.push_back('/'); ++i; break;
                case 'b': value.push_back('\b'); ++i; break;
                case 'f': value.push_back('\f'); ++i; break;
                case 'n': value.push_back('\n'); ++i; break;
                case 'r': value.push_back('\r'); ++i; break;
                case 't': value.push_back('\t'); ++i; break;
                case 'u': {
                    ++i;
                    unsigned cp = 0;
                    for (int k = 0; k < 4; ++k) {
                        if (i >= text.size()) {
                            return fail(error, "unfinished \\u escape", i);
                        }
                        const int digit = hexDigit(text[i]);
                        if (digit < 0) {
                            return fail(error, "bad \\u escape", i);
                        }
                        cp = (cp << 4) | static_cast<unsigned>(digit);
                        ++i;
                    }
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (i + 1 >= text.size() || text[i] != '\\' || text[i + 1] != 'u') {
                            return fail(error, "unpaired high surrogate", i);
                        }
                        i += 2;
                        unsigned low = 0;
                        for (int k = 0; k < 4; ++k) {
                            if (i >= text.size()) {
                                return fail(error, "unfinished low surrogate", i);
                            }
                            const int digit = hexDigit(text[i]);
                            if (digit < 0) {
                                return fail(error, "bad low surrogate", i);
                            }
                            low = (low << 4) | static_cast<unsigned>(digit);
                            ++i;
                        }
                        if (low < 0xDC00 || low > 0xDFFF) {
                            return fail(error, "bad low surrogate", i);
                        }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return fail(error, "unpaired low surrogate", i);
                    }
                    appendUtf8(value, cp);
                    break;
                }
                default:
                    return fail(error, "unknown escape", i);
            }
            continue;
        }
        if (c < 0x20) {
            return fail(error, "control character in string", i);
        }
        value.push_back(static_cast<char>(c));
        ++i;
    }
    return fail(error, "unterminated string", i);
}

bool parseNumber(const std::string& text, std::size_t& i, Value* out, std::string* error) {
    const std::size_t start = i;
    if (i < text.size() && text[i] == '-') {
        ++i;
    }
    bool digits = false;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
        ++i;
        digits = true;
    }
    if (!digits) {
        return fail(error, "number has no digits", start);
    }
    if (i < text.size() && text[i] == '.') {
        ++i;
        bool fraction = false;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
            ++i;
            fraction = true;
        }
        if (!fraction) {
            return fail(error, "number has no fraction digits", start);
        }
    }
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
            ++i;
        }
        bool exponent = false;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
            ++i;
            exponent = true;
        }
        if (!exponent) {
            return fail(error, "number has no exponent digits", start);
        }
    }
    const std::string literal = text.substr(start, i - start);
    char* end = nullptr;
    std::strtod(literal.c_str(), &end);
    if (!end || *end != '\0') {
        return fail(error, "invalid number", start);
    }
    out->type = Value::Type::Number;
    out->text = literal;
    return true;
}

bool parseValue(const std::string& text, std::size_t& i, Value* out, std::string* error,
                int depth);

bool parseObject(const std::string& text, std::size_t& i, Value* out, std::string* error,
                 int depth) {
    ++i;  // '{'
    Value result = Value::makeObject();
    skipSpace(text, i);
    if (i < text.size() && text[i] == '}') {
        ++i;
        *out = std::move(result);
        return true;
    }
    while (true) {
        skipSpace(text, i);
        if (i >= text.size() || text[i] != '"') {
            return fail(error, "expected an object key", i);
        }
        std::string key;
        if (!parseString(text, i, &key, error)) {
            return false;
        }
        skipSpace(text, i);
        if (i >= text.size() || text[i] != ':') {
            return fail(error, "expected ':' after a key", i);
        }
        ++i;
        skipSpace(text, i);
        Value member;
        if (!parseValue(text, i, &member, error, depth + 1)) {
            return false;
        }
        for (const auto& existing : result.object) {
            if (existing.first == key) {
                return fail(error, "duplicate object key", i);
            }
        }
        result.object.push_back({std::move(key), std::move(member)});
        skipSpace(text, i);
        if (i >= text.size()) {
            return fail(error, "unterminated object", i);
        }
        if (text[i] == ',') {
            ++i;
            continue;
        }
        if (text[i] == '}') {
            ++i;
            *out = std::move(result);
            return true;
        }
        return fail(error, "expected ',' or '}'", i);
    }
}

bool parseArray(const std::string& text, std::size_t& i, Value* out, std::string* error,
                int depth) {
    ++i;  // '['
    Value result = Value::makeArray();
    skipSpace(text, i);
    if (i < text.size() && text[i] == ']') {
        ++i;
        *out = std::move(result);
        return true;
    }
    while (true) {
        skipSpace(text, i);
        Value item;
        if (!parseValue(text, i, &item, error, depth + 1)) {
            return false;
        }
        result.array.push_back(std::move(item));
        skipSpace(text, i);
        if (i >= text.size()) {
            return fail(error, "unterminated array", i);
        }
        if (text[i] == ',') {
            ++i;
            continue;
        }
        if (text[i] == ']') {
            ++i;
            *out = std::move(result);
            return true;
        }
        return fail(error, "expected ',' or ']'", i);
    }
}

bool parseValue(const std::string& text, std::size_t& i, Value* out, std::string* error,
                int depth) {
    if (depth > kMaxDepth) {
        return fail(error, "nesting is too deep", i);
    }
    if (i >= text.size()) {
        return fail(error, "expected a value", i);
    }
    const char c = text[i];
    if (c == '{') {
        return parseObject(text, i, out, error, depth);
    }
    if (c == '[') {
        return parseArray(text, i, out, error, depth);
    }
    if (c == '"') {
        Value value = Value::makeString({});
        std::string contents;
        if (!parseString(text, i, &contents, error)) {
            return false;
        }
        value.text = std::move(contents);
        *out = std::move(value);
        return true;
    }
    if (c == 't') {
        if (text.compare(i, 4, "true") == 0) {
            i += 4;
            *out = Value::makeBool(true);
            return true;
        }
        return fail(error, "expected 'true'", i);
    }
    if (c == 'f') {
        if (text.compare(i, 5, "false") == 0) {
            i += 5;
            *out = Value::makeBool(false);
            return true;
        }
        return fail(error, "expected 'false'", i);
    }
    if (c == 'n') {
        if (text.compare(i, 4, "null") == 0) {
            i += 4;
            *out = Value::makeNull();
            return true;
        }
        return fail(error, "expected 'null'", i);
    }
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
        return parseNumber(text, i, out, error);
    }
    return fail(error, "unexpected character", i);
}

void writeValue(const Value& value, std::string& out) {
    switch (value.type) {
        case Value::Type::Null:
            out += "null";
            break;
        case Value::Type::Bool:
            out += value.boolean ? "true" : "false";
            break;
        case Value::Type::Number:
            out += value.text.empty() ? "0" : value.text;
            break;
        case Value::Type::String:
            out += '"';
            out += escape(value.text);
            out += '"';
            break;
        case Value::Type::Object: {
            out += '{';
            bool first = true;
            for (const auto& member : value.object) {
                if (!first) {
                    out += ',';
                }
                first = false;
                out += '"';
                out += escape(member.first);
                out += "\":";
                writeValue(member.second, out);
            }
            out += '}';
            break;
        }
        case Value::Type::Array: {
            out += '[';
            bool first = true;
            for (const auto& item : value.array) {
                if (!first) {
                    out += ',';
                }
                first = false;
                writeValue(item, out);
            }
            out += ']';
            break;
        }
    }
}

}  // namespace

Value Value::makeNull() { return Value{}; }

Value Value::makeBool(bool value) {
    Value result;
    result.type = Type::Bool;
    result.boolean = value;
    return result;
}

Value Value::makeString(std::string value) {
    Value result;
    result.type = Type::String;
    result.text = std::move(value);
    return result;
}

Value Value::makeInteger(long long value) {
    Value result;
    result.type = Type::Number;
    result.text = std::to_string(value);
    return result;
}

Value Value::makeNumber(double value) {
    Value result;
    result.type = Type::Number;
    if (!std::isfinite(value)) {
        result.text = "0";
        return result;
    }
    const double rounded = std::round(value);
    if (std::fabs(value - rounded) < 1e-9 && std::fabs(value) < 1e15) {
        result.text = std::to_string(static_cast<long long>(rounded));
    } else {
        char buffer[32];
        std::snprintf(buffer, sizeof buffer, "%.10g", value);
        result.text = buffer;
    }
    return result;
}

Value Value::makeObject() {
    Value result;
    result.type = Type::Object;
    return result;
}

Value Value::makeArray() {
    Value result;
    result.type = Type::Array;
    return result;
}

bool parse(const std::string& text, Value* out, std::string* error) {
    if (!out) {
        if (error) {
            *error = "parse: no output";
        }
        return false;
    }
    std::size_t i = 0;
    skipSpace(text, i);
    Value value;
    if (!parseValue(text, i, &value, error, 0)) {
        return false;
    }
    skipSpace(text, i);
    if (i != text.size()) {
        return fail(error, "trailing characters", i);
    }
    *out = std::move(value);
    return true;
}

std::string write(const Value& value) {
    std::string out;
    writeValue(value, out);
    return out;
}

std::string escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const unsigned char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof buffer, "\\u%04x", c);
                    out += buffer;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    return out;
}

const Value* find(const Value& object, const std::string& key) {
    if (object.type != Value::Type::Object) {
        return nullptr;
    }
    const Value* found = nullptr;
    for (const auto& member : object.object) {
        if (member.first == key) {
            found = &member.second;
        }
    }
    return found;
}

bool has(const Value& object, const std::string& key) { return find(object, key) != nullptr; }

std::string stringOr(const Value& object, const std::string& key, const std::string& fallback) {
    const Value* value = find(object, key);
    if (value && value->type == Value::Type::String) {
        return value->text;
    }
    return fallback;
}

bool numberOr(const Value& object, const std::string& key, double* out) {
    const Value* value = find(object, key);
    if (!value || value->type != Value::Type::Number) {
        return false;
    }
    char* end = nullptr;
    const double parsed = std::strtod(value->text.c_str(), &end);
    if (!end || *end != '\0') {
        return false;
    }
    if (out) {
        *out = parsed;
    }
    return true;
}

bool boolOr(const Value& object, const std::string& key, bool fallback) {
    const Value* value = find(object, key);
    if (value && value->type == Value::Type::Bool) {
        return value->boolean;
    }
    return fallback;
}

}  // namespace openshow::ndi::json
