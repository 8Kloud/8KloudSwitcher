/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace kloud {

// INI settings store in the exact dialect QSettings::IniFormat writes, so a
// show.ini saved by the retired Qt GUI loads unchanged and files written here
// would load back into Qt: sorted sections and keys, `[General]` for
// top-level keys, arrays as `N\key` entries plus `size=`, values quoted only
// when they carry `, ; =` or edge spaces, `\" \\ \n \t \xH..` escapes,
// comma-joined string lists, `@Invalid()` for an empty list, raw UTF-8.
class IniFile {
public:
    // Keys are "section/key" (or "section/sub/key" -> `[section] sub\key`).
    // A key without a section lives in [General].
    bool load(const std::string& path);
    bool save(const std::string& path) const;

    bool has(std::string_view key) const;
    std::string getString(std::string_view key, std::string_view def = {}) const;
    int getInt(std::string_view key, int def) const;
    int64_t getInt64(std::string_view key, int64_t def) const;
    double getDouble(std::string_view key, double def) const;
    bool getBool(std::string_view key, bool def) const;
    std::vector<std::string> getList(std::string_view key) const;

    void set(std::string_view key, std::string_view value);
    void set(std::string_view key, const char* value) { set(key, std::string_view(value)); }
    void set(std::string_view key, int value) { set(key, int64_t(value)); }
    void set(std::string_view key, int64_t value);
    void set(std::string_view key, double value);
    void set(std::string_view key, bool value);
    void setList(std::string_view key, const std::vector<std::string>& items);
    void remove(std::string_view key);

    // Arrays in QSettings layout: "prefix/size" plus "prefix/N/key", N 1-based.
    int arraySize(std::string_view prefix) const;
    void setArraySize(std::string_view prefix, int n);
    static std::string arrayKey(std::string_view prefix, int index,
                                std::string_view key);

    // Raw view (tests, diagnostics): full key -> stored value.
    const std::map<std::string, std::string>& entries() const { return values_; }

    // Exposed for tests: the value-level escaping QSettings applies.
    static std::string escapeValue(std::string_view s, bool inList = false);
    static std::string escapeList(const std::vector<std::string>& items);
    // Decodes one raw value; a value containing unquoted commas yields a
    // list (QSettings does the same on read).
    static std::vector<std::string> unescapeValue(std::string_view raw,
                                                  bool& isList);

private:
    std::map<std::string, std::string> values_;  // decoded scalar text
    // Values known to be lists keep their items joined with '\x1f' in
    // values_ and their key here, so a one-item or empty list survives.
    std::map<std::string, bool> lists_;
};

}  // namespace kloud
