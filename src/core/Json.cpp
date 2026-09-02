/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "core/Json.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace kloud::json {

namespace {

const Value& nullValue() {
    static const Value v;
    return v;
}

const Value::Array& emptyArray() {
    static const Value::Array a;
    return a;
}

const Value::Object& emptyObject() {
    static const Value::Object o;
    return o;
}

const std::string& emptyString() {
    static const std::string s;
    return s;
}

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += char(cp);
    } else if (cp < 0x800) {
        out += char(0xC0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += char(0xE0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    } else {
        out += char(0xF0 | (cp >> 18));
        out += char(0x80 | ((cp >> 12) & 0x3F));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    }
}

struct Parser {
    std::string_view s;
    size_t i = 0;
    std::string err;
    static constexpr int kMaxDepth = 64;

    bool fail(const char* what) {
        if (err.empty())
            err = std::string(what) + " at offset " + std::to_string(i);
        return false;
    }
    void ws() {
        while (i < s.size() &&
               (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t'))
            ++i;
    }
    bool hex4(uint32_t& cp) {
        if (i + 4 > s.size()) return false;
        cp = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s[i++];
            int v;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            else return false;
            cp = cp * 16 + uint32_t(v);
        }
        return true;
    }
    bool string(std::string& out) {
        if (i >= s.size() || s[i] != '"') return fail("expected string");
        ++i;
        while (i < s.size()) {
            const char c = s[i++];
            if (c == '"') return true;
            if ((unsigned char)c < 0x20) return fail("control character in string");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= s.size()) return fail("bad escape");
            const char e = s[i++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp;
                    if (!hex4(cp)) return fail("bad \\u escape");
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        uint32_t lo = 0;
                        if (i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                            i += 2;
                            if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF)
                                return fail("bad surrogate pair");
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            cp = 0xFFFD;
                        }
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }
    bool number(Value& out) {
        const size_t start = i;
        if (i < s.size() && s[i] == '-') ++i;
        if (i >= s.size()) return fail("bad number");
        if (s[i] == '0') {
            ++i;
        } else if (s[i] >= '1' && s[i] <= '9') {
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
        } else {
            return fail("bad number");
        }
        if (i < s.size() && s[i] == '.') {
            ++i;
            if (i >= s.size() || s[i] < '0' || s[i] > '9') return fail("bad number");
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
        }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            ++i;
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
            if (i >= s.size() || s[i] < '0' || s[i] > '9') return fail("bad number");
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
        }
        const std::string text(s.substr(start, i - start));
        const double d = std::strtod(text.c_str(), nullptr);
        if (!std::isfinite(d)) return fail("number out of range");
        out = Value(d);
        return true;
    }
    bool literal(const char* word, Value v, Value& out) {
        const size_t n = strlen(word);
        if (s.substr(i, n) != word) return fail("bad literal");
        i += n;
        out = std::move(v);
        return true;
    }
    bool value(Value& out, int depth) {
        if (depth > kMaxDepth) return fail("nesting too deep");
        ws();
        if (i >= s.size()) return fail("unexpected end");
        const char c = s[i];
        if (c == '{') {
            ++i;
            Value::Object obj;
            ws();
            if (i < s.size() && s[i] == '}') {
                ++i;
                out = Value(std::move(obj));
                return true;
            }
            for (;;) {
                ws();
                std::string key;
                if (!string(key)) return false;
                ws();
                if (i >= s.size() || s[i] != ':') return fail("expected ':'");
                ++i;
                Value v;
                if (!value(v, depth + 1)) return false;
                obj[std::move(key)] = std::move(v);
                ws();
                if (i < s.size() && s[i] == ',') {
                    ++i;
                    continue;
                }
                if (i < s.size() && s[i] == '}') {
                    ++i;
                    out = Value(std::move(obj));
                    return true;
                }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            ++i;
            Value::Array arr;
            ws();
            if (i < s.size() && s[i] == ']') {
                ++i;
                out = Value(std::move(arr));
                return true;
            }
            for (;;) {
                Value v;
                if (!value(v, depth + 1)) return false;
                arr.push_back(std::move(v));
                ws();
                if (i < s.size() && s[i] == ',') {
                    ++i;
                    continue;
                }
                if (i < s.size() && s[i] == ']') {
                    ++i;
                    out = Value(std::move(arr));
                    return true;
                }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            std::string str;
            if (!string(str)) return false;
            out = Value(std::move(str));
            return true;
        }
        if (c == 't') return literal("true", Value(true), out);
        if (c == 'f') return literal("false", Value(false), out);
        if (c == 'n') return literal("null", Value(), out);
        if (c == '-' || (c >= '0' && c <= '9')) return number(out);
        return fail("unexpected character");
    }
};

}  // namespace

int Value::asInt(int def) const {
    if (!isNumber() || !std::isfinite(num_)) return def;
    if (num_ < -2147483648.0 || num_ > 2147483647.0) return def;
    return int(num_);
}

int64_t Value::asInt64(int64_t def) const {
    if (!isNumber() || !std::isfinite(num_)) return def;
    if (num_ < -9.2e18 || num_ > 9.2e18) return def;
    return int64_t(num_);
}

const std::string& Value::asString() const {
    return isString() ? str_ : emptyString();
}

const Value::Array& Value::asArray() const {
    return isArray() ? arr_ : emptyArray();
}

const Value::Object& Value::asObject() const {
    return isObject() ? obj_ : emptyObject();
}

const Value& Value::operator[](std::string_view key) const {
    if (!isObject()) return nullValue();
    const auto it = obj_.find(std::string(key));
    return it == obj_.end() ? nullValue() : it->second;
}

const Value& Value::operator[](size_t i) const {
    if (!isArray() || i >= arr_.size()) return nullValue();
    return arr_[i];
}

bool Value::contains(std::string_view key) const {
    return isObject() && obj_.find(std::string(key)) != obj_.end();
}

size_t Value::size() const {
    if (isArray()) return arr_.size();
    if (isObject()) return obj_.size();
    return 0;
}

Value& Value::set(std::string key, Value v) {
    if (!isObject()) {
        type_ = Type::Object;
        obj_.clear();
    }
    obj_[std::move(key)] = std::move(v);
    return *this;
}

Value& Value::push(Value v) {
    if (!isArray()) {
        type_ = Type::Array;
        arr_.clear();
    }
    arr_.push_back(std::move(v));
    return *this;
}

void escapeTo(std::string& out, std::string_view s) {
    out += '"';
    for (const unsigned char c : s) {
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
                    char buf[8];
                    snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += char(c);
                }
        }
    }
    out += '"';
}

std::string escape(std::string_view s) {
    std::string out;
    escapeTo(out, s);
    return out;
}

void Value::dumpTo(std::string& out) const {
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Number: {
            char buf[32];
            if (num_ == std::floor(num_) && std::fabs(num_) < 1e15) {
                snprintf(buf, sizeof buf, "%.0f", num_);
            } else {
                snprintf(buf, sizeof buf, "%.10g", num_);
            }
            out += buf;
            break;
        }
        case Type::String: escapeTo(out, str_); break;
        case Type::Array: {
            out += '[';
            bool first = true;
            for (const auto& v : arr_) {
                if (!first) out += ',';
                first = false;
                v.dumpTo(out);
            }
            out += ']';
            break;
        }
        case Type::Object: {
            out += '{';
            bool first = true;
            for (const auto& [k, v] : obj_) {
                if (!first) out += ',';
                first = false;
                escapeTo(out, k);
                out += ':';
                v.dumpTo(out);
            }
            out += '}';
            break;
        }
    }
}

std::string Value::dump() const {
    std::string out;
    dumpTo(out);
    return out;
}

bool parse(std::string_view text, Value& out, std::string& err) {
    Parser p{text};
    err.clear();
    if (!p.value(out, 0)) {
        err = p.err;
        return false;
    }
    p.ws();
    if (p.i != text.size()) {
        err = "trailing characters at offset " + std::to_string(p.i);
        return false;
    }
    return true;
}

}  // namespace kloud::json
