/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once
#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kloud::json {

// Small JSON value: enough for the web GUI's command messages and the
// richer state documents the web server publishes. Numbers are doubles
// (integers survive exactly up to 2^53, well past any counter here).
class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value>;

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : type_(Type::Bool), bool_(b) {}
    Value(int v) : type_(Type::Number), num_(v) {}
    Value(int64_t v) : type_(Type::Number), num_(double(v)) {}
    Value(uint64_t v) : type_(Type::Number), num_(double(v)) {}
    Value(double v) : type_(Type::Number), num_(v) {}
    Value(const char* s) : type_(Type::String), str_(s) {}
    Value(std::string s) : type_(Type::String), str_(std::move(s)) {}
    Value(std::string_view s) : type_(Type::String), str_(s) {}
    Value(Array a) : type_(Type::Array), arr_(std::move(a)) {}
    Value(Object o) : type_(Type::Object), obj_(std::move(o)) {}
    Value(std::initializer_list<std::pair<const std::string, Value>> o)
        : type_(Type::Object), obj_(o) {}

    static Value array() { return Value(Array{}); }
    static Value object() { return Value(Object{}); }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool asBool(bool def = false) const { return isBool() ? bool_ : def; }
    double asNumber(double def = 0) const { return isNumber() ? num_ : def; }
    int asInt(int def = 0) const;
    int64_t asInt64(int64_t def = 0) const;
    const std::string& asString() const;
    std::string asString(std::string_view def) const {
        return isString() ? str_ : std::string(def);
    }
    const Array& asArray() const;
    const Object& asObject() const;

    // Object access; a missing key or a non-object yields a shared null.
    const Value& operator[](std::string_view key) const;
    const Value& operator[](size_t i) const;
    bool contains(std::string_view key) const;
    size_t size() const;

    Value& set(std::string key, Value v);  // object insert/replace
    Value& push(Value v);                  // array append

    std::string dump() const;
    void dumpTo(std::string& out) const;

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0;
    std::string str_;
    Array arr_;
    Object obj_;
};

// Parses a complete document; returns false with `err` set on malformed
// input (position included). Depth is capped so hostile input cannot
// recurse the stack away.
bool parse(std::string_view text, Value& out, std::string& err);

void escapeTo(std::string& out, std::string_view s);
std::string escape(std::string_view s);

}  // namespace kloud::json
