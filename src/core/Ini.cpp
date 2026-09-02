/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "core/Ini.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace kloud {

namespace {

constexpr char kListSep = '\x1f';

std::string trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r'))
        --e;
    return std::string(s.substr(b, e - b));
}

// "section/a/b" -> section "section", ini key "a\b". No slash -> General.
void splitKey(std::string_view full, std::string& section, std::string& key) {
    const size_t slash = full.find('/');
    if (slash == std::string_view::npos) {
        section = "General";
        key = std::string(full);
    } else {
        section = std::string(full.substr(0, slash));
        key = std::string(full.substr(slash + 1));
        std::replace(key.begin(), key.end(), '/', '\\');
    }
}

std::string joinKey(std::string_view section, std::string_view iniKey) {
    std::string key(iniKey);
    std::replace(key.begin(), key.end(), '\\', '/');
    if (section == "General") return key;
    return std::string(section) + "/" + key;
}

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
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

}  // namespace

std::vector<std::string> IniFile::unescapeValue(std::string_view raw,
                                                bool& isList) {
    isList = false;
    std::vector<std::string> items;
    std::string cur;
    bool inQuote = false;
    bool sawUnquotedComma = false;
    size_t i = 0;
    const std::string_view s = raw;
    if (s == "@Invalid()") {
        isList = true;
        return items;
    }
    // "@@x" is a literal "@x"; other @-prefixed forms are QVariant encodings
    // this dialect never writes for a show file -- keep them verbatim.
    if (s.size() >= 2 && s[0] == '@' && s[1] == '@') i = 1;
    while (i < s.size()) {
        const char c = s[i];
        if (c == '"') {
            inQuote = !inQuote;
            ++i;
            continue;
        }
        if (c == '\\' && i + 1 < s.size()) {
            const char n = s[i + 1];
            i += 2;
            switch (n) {
                case 'n': cur += '\n'; break;
                case 't': cur += '\t'; break;
                case 'r': cur += '\r'; break;
                case '0': cur += '\0'; break;
                case 'a': cur += '\a'; break;
                case 'b': cur += '\b'; break;
                case 'f': cur += '\f'; break;
                case 'v': cur += '\v'; break;
                case 'x': {
                    uint32_t cp = 0;
                    int digits = 0;
                    while (i < s.size() && hexVal(s[i]) >= 0) {
                        cp = cp * 16 + uint32_t(hexVal(s[i]));
                        ++i;
                        ++digits;
                    }
                    if (digits) appendUtf8(cur, cp);
                    break;
                }
                default: cur += n; break;
            }
            continue;
        }
        if (c == ',' && !inQuote) {
            sawUnquotedComma = true;
            items.push_back(cur);
            cur.clear();
            ++i;
            while (i < s.size() && s[i] == ' ') ++i;
            continue;
        }
        cur += c;
        ++i;
    }
    // Whitespace before an unquoted comma is part of the join, not the item.
    if (sawUnquotedComma) {
        items.push_back(cur);
        for (auto& it : items) {
            while (!it.empty() && it.back() == ' ') it.pop_back();
        }
        isList = true;
        return items;
    }
    items.push_back(cur);
    return items;
}

std::string IniFile::escapeValue(std::string_view s, bool inList) {
    std::string body;
    bool needQuotes = false;
    if (s.empty()) return {};
    if (s.front() == ' ' || s.back() == ' ') needQuotes = true;
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '\\': body += "\\\\"; break;
            case '"': body += "\\\""; break;
            case '\n': body += "\\n"; break;
            case '\t': body += "\\t"; break;
            case '\r': body += "\\r"; break;
            case '\0': body += "\\0"; break;
            case '\a': body += "\\a"; break;
            case '\b': body += "\\b"; break;
            case '\f': body += "\\f"; break;
            case '\v': body += "\\v"; break;
            case ',':
            case ';':
            case '=':
                needQuotes = true;
                body += char(c);
                break;
            default:
                if (c < 0x20 || c == 0x7f) {
                    char buf[8];
                    snprintf(buf, sizeof buf, "\\x%x", c);
                    body += buf;
                    // A following hex digit would be swallowed by the greedy
                    // read; a quote boundary keeps it separate without
                    // changing the decoded value.
                    if (i + 1 < s.size() && hexVal(s[i + 1]) >= 0) {
                        body += "\"\"";
                        needQuotes = true;
                    }
                } else {
                    body += char(c);
                }
        }
    }
    if (!inList && s[0] == '@') body.insert(body.begin(), '@');
    if (needQuotes) return "\"" + body + "\"";
    return body;
}

std::string IniFile::escapeList(const std::vector<std::string>& items) {
    if (items.empty()) return "@Invalid()";
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) out += ", ";
        out += escapeValue(items[i], true);
    }
    // A one-item list that itself contains a comma is still written quoted
    // above; a one-item list reads back as a scalar, which getList wraps.
    return out;
}

bool IniFile::load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    values_.clear();
    lists_.clear();
    std::string section = "General";
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string t = trim(line);
        if (t.empty() || t[0] == ';' || t[0] == '#') continue;
        if (t.front() == '[') {
            const size_t close = t.find(']');
            section = trim(t.substr(1, close == std::string::npos
                                           ? std::string::npos
                                           : close - 1));
            if (section.empty()) section = "General";
            continue;
        }
        const size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(t.substr(0, eq));
        const std::string rawValue = trim(t.substr(eq + 1));
        bool isList = false;
        const auto items = unescapeValue(rawValue, isList);
        const std::string full = joinKey(section, key);
        if (isList) {
            std::string joined;
            for (size_t i = 0; i < items.size(); ++i) {
                if (i) joined += kListSep;
                joined += items[i];
            }
            values_[full] = joined;
            lists_[full] = true;
        } else {
            values_[full] = items.empty() ? std::string() : items[0];
            lists_.erase(full);
        }
    }
    return true;
}

bool IniFile::save(const std::string& path) const {
    // Group by section, sections and keys sorted, General first: the order
    // QSettings produces, so a Qt-written file and ours diff cleanly.
    std::map<std::string, std::map<std::string, std::string>> sections;
    for (const auto& [full, value] : values_) {
        std::string section, key;
        splitKey(full, section, key);
        std::string encoded;
        if (const auto it = lists_.find(full); it != lists_.end() && it->second) {
            std::vector<std::string> items;
            if (!value.empty()) {
                std::string cur;
                for (const char c : value) {
                    if (c == kListSep) {
                        items.push_back(cur);
                        cur.clear();
                    } else {
                        cur += c;
                    }
                }
                items.push_back(cur);
            }
            encoded = escapeList(items);
        } else {
            encoded = escapeValue(value);
        }
        sections[section][key] = std::move(encoded);
    }
    std::ostringstream out;
    bool first = true;
    auto writeSection = [&](const std::string& name,
                            const std::map<std::string, std::string>& kv) {
        if (!first) out << '\n';
        first = false;
        out << '[' << name << "]\n";
        for (const auto& [k, v] : kv) out << k << '=' << v << '\n';
    };
    if (const auto g = sections.find("General"); g != sections.end())
        writeSection("General", g->second);
    for (const auto& [name, kv] : sections)
        if (name != "General") writeSection(name, kv);

    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << out.str();
        if (!f) return false;
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

bool IniFile::has(std::string_view key) const {
    return values_.find(std::string(key)) != values_.end();
}

std::string IniFile::getString(std::string_view key,
                               std::string_view def) const {
    const auto it = values_.find(std::string(key));
    if (it == values_.end()) return std::string(def);
    if (const auto l = lists_.find(it->first); l != lists_.end() && l->second) {
        // QVariant(QStringList).toString() is empty; a show file never
        // stores a list where a string is expected, so join for readability.
        std::string joined = it->second;
        std::replace(joined.begin(), joined.end(), kListSep, ',');
        return joined;
    }
    return it->second;
}

int IniFile::getInt(std::string_view key, int def) const {
    const int64_t v = getInt64(key, def);
    if (v < INT32_MIN || v > INT32_MAX) return def;
    return int(v);
}

int64_t IniFile::getInt64(std::string_view key, int64_t def) const {
    const auto it = values_.find(std::string(key));
    if (it == values_.end()) return def;
    const std::string& s = it->second;
    int64_t v = 0;
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc() || p != s.data() + s.size()) {
        // QVariant::toInt also accepts "1.0"-style doubles.
        const double d = getDouble(key, NAN);
        if (std::isfinite(d)) return int64_t(d);
        return def;
    }
    return v;
}

double IniFile::getDouble(std::string_view key, double def) const {
    const auto it = values_.find(std::string(key));
    if (it == values_.end()) return def;
    const std::string& s = it->second;
    if (s.empty()) return def;
    char* end = nullptr;
    errno = 0;
    const double d = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0' || errno == ERANGE) return def;
    return d;
}

bool IniFile::getBool(std::string_view key, bool def) const {
    const auto it = values_.find(std::string(key));
    if (it == values_.end()) return def;
    std::string s = it->second;
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    // QVariant::toBool: "0", "false" and empty are false, anything else true.
    return !(s.empty() || s == "0" || s == "false");
}

std::vector<std::string> IniFile::getList(std::string_view key) const {
    const auto it = values_.find(std::string(key));
    if (it == values_.end()) return {};
    if (const auto l = lists_.find(it->first); l != lists_.end() && l->second) {
        std::vector<std::string> items;
        if (it->second.empty()) return items;  // @Invalid() -> empty list
        std::string cur;
        for (const char c : it->second) {
            if (c == kListSep) {
                items.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
        items.push_back(cur);
        return items;
    }
    // A scalar reads as a one-element list (QVariant::toStringList); an
    // empty scalar is an empty list rather than [""], which is how a Qt
    // reader treats an absent-or-empty playlist too.
    if (it->second.empty()) return {};
    return {it->second};
}

void IniFile::set(std::string_view key, std::string_view value) {
    values_[std::string(key)] = std::string(value);
    lists_.erase(std::string(key));
}

void IniFile::set(std::string_view key, int64_t value) {
    set(key, std::string_view(std::to_string(value)));
}

void IniFile::set(std::string_view key, double value) {
    // Shortest round-trip form, like QVariant's double-to-string.
    char buf[64];
    if (value == std::floor(value) && std::fabs(value) < 1e15) {
        snprintf(buf, sizeof buf, "%.0f", value);
    } else {
        for (int prec = 1; prec <= 17; ++prec) {
            snprintf(buf, sizeof buf, "%.*g", prec, value);
            if (std::strtod(buf, nullptr) == value) break;
        }
    }
    set(key, std::string_view(buf));
}

void IniFile::set(std::string_view key, bool value) {
    set(key, std::string_view(value ? "true" : "false"));
}

void IniFile::setList(std::string_view key,
                      const std::vector<std::string>& items) {
    std::string joined;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) joined += kListSep;
        joined += items[i];
    }
    values_[std::string(key)] = joined;
    lists_[std::string(key)] = true;
    // An empty list stores as empty text; the list flag makes save() emit
    // @Invalid() and getList() return nothing.
}

void IniFile::remove(std::string_view key) {
    values_.erase(std::string(key));
    lists_.erase(std::string(key));
}

int IniFile::arraySize(std::string_view prefix) const {
    return std::max(0, getInt(std::string(prefix) + "/size", 0));
}

void IniFile::setArraySize(std::string_view prefix, int n) {
    set(std::string(prefix) + "/size", n);
}

std::string IniFile::arrayKey(std::string_view prefix, int index,
                              std::string_view key) {
    return std::string(prefix) + "/" + std::to_string(index + 1) + "/" +
           std::string(key);
}

}  // namespace kloud
