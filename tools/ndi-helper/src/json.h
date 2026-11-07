// A small, dependency-free JSON value for the helper's line protocol. The
// helper must build on Windows without the app's Boost dependency, so this is a
// deliberately tiny implementation: objects, arrays, strings, numbers, booleans
// and null. Strings handle the standard escapes and \uXXXX (including surrogate
// pairs) and are written as UTF-8. Nesting is bounded so a malformed command
// cannot exhaust the stack. This file is MIT, like the rest of the helper.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace openshow::ndi::json {

struct Value {
    enum class Type { Null, Bool, Number, String, Object, Array };

    Type type = Type::Null;
    bool boolean = false;
    std::string text;  // string contents, or the number literal as written
    std::vector<std::pair<std::string, Value>> object;
    std::vector<Value> array;

    static Value makeNull();
    static Value makeBool(bool value);
    static Value makeString(std::string value);
    static Value makeInteger(long long value);
    static Value makeNumber(double value);
    static Value makeObject();
    static Value makeArray();
};

// Parses one complete JSON value. `error` carries a byte offset on failure.
// Duplicate object keys and nesting deeper than 32 levels are refused.
bool parse(const std::string& text, Value* out, std::string* error);

// Serialises a value with no trailing newline.
std::string write(const Value& value);

// Escapes a string for embedding in JSON text (without the surrounding quotes).
std::string escape(const std::string& text);

// Object helpers. `find` returns nullptr when the value is not an object or the
// key is absent; a later duplicate key wins, but `parse` refuses duplicates.
const Value* find(const Value& object, const std::string& key);
bool has(const Value& object, const std::string& key);
std::string stringOr(const Value& object, const std::string& key,
                     const std::string& fallback = {});
bool numberOr(const Value& object, const std::string& key, double* out);
bool boolOr(const Value& object, const std::string& key, bool fallback);

}  // namespace openshow::ndi::json
