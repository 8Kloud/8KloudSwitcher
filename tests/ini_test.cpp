/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "core/Ini.h"

using kloud::IniFile;

namespace {
std::string tmpPath(const char* name) {
    const char* dir = getenv("TMPDIR");
    return std::string(dir ? dir : "/tmp") + "/kloud-ini-" + name + "-" +
           std::to_string(getpid()) + ".ini";
}
std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
}  // namespace

// The fixture was written by Qt 6 QSettings::IniFormat (see the generator
// notes in tests/fixtures/); every quirk below is what a real show.ini
// from the Qt GUI can contain.
TEST_CASE("ini: reads a QSettings-written file") {
    IniFile ini;
    REQUIRE(ini.load(std::string(KLOUD_TEST_FIXTURES) + "/qt-show-fixture.ini"));

    CHECK(ini.getInt("topLevel", 0) == 5);       // [General]
    CHECK(ini.getInt("show/width", 0) == 1920);
    CHECK(ini.getInt64("show/fpsN", 0) == 60000);
    CHECK(ini.getBool("show/omtOut", false));
    CHECK_FALSE(ini.getBool("show/cleanOmtOut", true));
    CHECK(ini.getDouble("show/gainD", 0) == 0.5);
    CHECK(ini.getDouble("show/gainOne", 0) == 1.0);
    CHECK(ini.getDouble("show/gainThird", 0) == 0.3333333333333333);
    CHECK(ini.getString("show/omtOutName") == "8Kloud Switcher PGM");
    CHECK(ini.getString("show/empty", "x").empty());
    CHECK(ini.has("show/empty"));
    CHECK(ini.getString("show/paren") == "HOST (CamA)");
    CHECK(ini.getString("show/comma") == "a,b");
    CHECK(ini.getString("show/semi") == "a;b");
    CHECK(ini.getString("show/eq") == "a=b");
    CHECK(ini.getString("show/quote") == "say \"hi\"");
    CHECK(ini.getString("show/backslash") == "C:\\x\\y");
    CHECK(ini.getString("show/lead") == " lead");
    CHECK(ini.getString("show/trail") == "trail ");
    CHECK(ini.getString("show/unicode") ==
          "SRT \xc2\xb7 caf\xc3\xa9 \xe2\x80\x94 \xf0\x9f\x90\x84");
    CHECK(ini.getString("show/newline") == "a\nb\tc");
    CHECK(ini.getString("show/ctrl") == std::string("x\x01y"));
    CHECK(ini.getString("show/at") == "@Invalid()");
    CHECK(ini.getString("show/hash") == "#notcomment");
    CHECK(ini.getString("show/url") ==
          "srt://host:9710?mode=caller&latency=120000");

    CHECK(ini.getList("show/listEmpty").empty());
    CHECK(ini.getList("show/listOne") ==
          std::vector<std::string>{"/a b/c.mkv"});
    CHECK(ini.getList("show/listTwo") ==
          std::vector<std::string>{"/a,b/c.mkv", "/d/e f.mkv"});
    CHECK(ini.getList("show/listNums") ==
          std::vector<std::string>{"500", "1250", "0"});
    CHECK(ini.getList("show/listWithEmpty") ==
          std::vector<std::string>{"", "x"});

    CHECK(ini.arraySize("inputs") == 2);
    CHECK(ini.getString(IniFile::arrayKey("inputs", 0, "type")) == "omt");
    CHECK(ini.getString(IniFile::arrayKey("inputs", 0, "ref")) == "HOST (CamA)");
    CHECK(ini.getString(IniFile::arrayKey("inputs", 1, "ref")) ==
          "/shows/roll in.mkv");
    CHECK(ini.getList(IniFile::arrayKey("inputs", 1, "mediaPlaylist")) ==
          std::vector<std::string>{"/shows/roll in.mkv", "/shows/b,c.mkv"});
    CHECK(ini.getString("nested/deep/k") == "v");
}

TEST_CASE("ini: hand-edited variants read like QSettings") {
    const std::string path = tmpPath("hand");
    {
        std::ofstream f(path);
        f << "[g]\n"
             "plain=hello world\n"
             "quoted=\"quoted; value\"\n"
             "hex=a\\x2cb\\x3b\n"
             "list=a, b, c\n"
             "listQ=\"x, y\", z\n"
             "bt=true\nb1=1\nbf=false\n"
             "spaced =  padded  \n"
             "utf8=SRT \xc2\xb7 caf\xc3\xa9\n"
             "emptyq=\"\"\n"
             "; comment\n"
             "[arr]\nsize=3\n1\\k=1\n";
    }
    IniFile ini;
    REQUIRE(ini.load(path));
    CHECK(ini.getString("g/plain") == "hello world");
    CHECK(ini.getString("g/quoted") == "quoted; value");
    // Qt's \x escape is greedy: \x2cb is U+02CB, not ',' + 'b'.
    CHECK(ini.getString("g/hex") == "a\xcb\x8b;");
    CHECK(ini.getList("g/list") == std::vector<std::string>{"a", "b", "c"});
    CHECK(ini.getList("g/listQ") == std::vector<std::string>{"x, y", "z"});
    CHECK(ini.getList("g/plain") == std::vector<std::string>{"hello world"});
    CHECK(ini.getBool("g/bt", false));
    CHECK(ini.getBool("g/b1", false));
    CHECK_FALSE(ini.getBool("g/bf", true));
    CHECK(ini.getString("g/spaced") == "padded");
    CHECK(ini.getString("g/utf8") == "SRT \xc2\xb7 caf\xc3\xa9");
    CHECK(ini.getString("g/emptyq", "x").empty());
    CHECK(ini.arraySize("arr") == 3);
    CHECK(ini.getInt("g/missing", 7) == 7);
    CHECK(ini.getInt("g/plain", 7) == 7);
    std::remove(path.c_str());
}

TEST_CASE("ini: writes the QSettings dialect and round-trips") {
    IniFile ini;
    ini.set("topLevel", 5);
    ini.set("show/width", 1920);
    ini.set("show/omtOut", true);
    ini.set("show/cleanOmtOut", false);
    ini.set("show/omtOutName", "8Kloud Switcher PGM");
    ini.set("show/empty", "");
    ini.set("show/fpsN", int64_t(60000));
    ini.set("show/gainD", 0.5);
    ini.set("show/gainOne", 1.0);
    ini.set("show/gainThird", 1.0 / 3.0);
    ini.set("show/paren", "HOST (CamA)");
    ini.set("show/comma", "a,b");
    ini.set("show/semi", "a;b");
    ini.set("show/eq", "a=b");
    ini.set("show/quote", "say \"hi\"");
    ini.set("show/backslash", "C:\\x\\y");
    ini.set("show/lead", " lead");
    ini.set("show/trail", "trail ");
    ini.set("show/unicode",
            "SRT \xc2\xb7 caf\xc3\xa9 \xe2\x80\x94 \xf0\x9f\x90\x84");
    ini.set("show/newline", "a\nb\tc");
    ini.set("show/ctrl", std::string("x\x01y"));
    ini.set("show/at", "@Invalid()");
    ini.set("show/hash", "#notcomment");
    ini.set("show/url", "srt://host:9710?mode=caller&latency=120000");
    ini.setList("show/listEmpty", {});
    ini.setList("show/listOne", {"/a b/c.mkv"});
    ini.setList("show/listTwo", {"/a,b/c.mkv", "/d/e f.mkv"});
    ini.setList("show/listNums", {"500", "1250", "0"});
    ini.setList("show/listWithEmpty", {"", "x"});
    ini.setArraySize("inputs", 2);
    ini.set(IniFile::arrayKey("inputs", 0, "type"), "omt");
    ini.set(IniFile::arrayKey("inputs", 0, "ref"), "HOST (CamA)");
    ini.set(IniFile::arrayKey("inputs", 1, "type"), "media");
    ini.set(IniFile::arrayKey("inputs", 1, "ref"), "/shows/roll in.mkv");
    ini.setList(IniFile::arrayKey("inputs", 1, "mediaPlaylist"),
                {"/shows/roll in.mkv", "/shows/b,c.mkv"});
    ini.set("nested/deep/k", "v");

    const std::string path = tmpPath("rt");
    REQUIRE(ini.save(path));
    const std::string written = slurp(path);
    // Byte-identical to what Qt wrote for the same values.
    CHECK(written ==
          slurp(std::string(KLOUD_TEST_FIXTURES) + "/qt-show-fixture.ini"));

    IniFile back;
    REQUIRE(back.load(path));
    CHECK(back.entries() == ini.entries());
    CHECK(back.getList("show/listEmpty").empty());
    CHECK(back.getList("show/listWithEmpty") ==
          std::vector<std::string>{"", "x"});
    std::remove(path.c_str());
}

TEST_CASE("ini: a control character before a hex digit survives") {
    IniFile ini;
    ini.set("g/v", std::string("x\x01" "2"));
    const std::string path = tmpPath("hex");
    REQUIRE(ini.save(path));
    IniFile back;
    REQUIRE(back.load(path));
    CHECK(back.getString("g/v") == std::string("x\x01" "2"));
    std::remove(path.c_str());
}

TEST_CASE("ini: remove and missing file") {
    IniFile ini;
    CHECK_FALSE(ini.load("/nonexistent/dir/show.ini"));
    ini.set("a/b", 1);
    ini.remove("a/b");
    CHECK_FALSE(ini.has("a/b"));
}
