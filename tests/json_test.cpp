/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <catch2/catch_test_macros.hpp>

#include "core/Json.h"

using kloud::json::Value;

namespace {
Value must(const char* text) {
    Value v;
    std::string err;
    INFO(text << " -> " << err);
    REQUIRE(kloud::json::parse(text, v, err));
    return v;
}
bool fails(const char* text) {
    Value v;
    std::string err;
    return !kloud::json::parse(text, v, err) && !err.empty();
}
}  // namespace

TEST_CASE("json: parses documents") {
    const Value v = must(
        R"json({"cmd":"replaceInput","input":3,"ref":"HOST (Cam A)","sync":-1,)json"
        R"json("items":[{"path":"/a b.mkv","in":500,"out":0,"speed":1.5}],)json"
        R"json("on":true,"off":false,"nil":null,"esc":"q\"\\\/\n\u00e9\ud83d\udc04"})json");
    CHECK(v["cmd"].asString() == "replaceInput");
    CHECK(v["input"].asInt() == 3);
    CHECK(v["ref"].asString() == "HOST (Cam A)");
    CHECK(v["sync"].asInt() == -1);
    CHECK(v["items"].size() == 1);
    CHECK(v["items"][0]["path"].asString() == "/a b.mkv");
    CHECK(v["items"][0]["in"].asInt64() == 500);
    CHECK(v["items"][0]["speed"].asNumber() == 1.5);
    CHECK(v["on"].asBool());
    CHECK_FALSE(v["off"].asBool(true));
    CHECK(v["nil"].isNull());
    CHECK(v["esc"].asString() == "q\"\\/\n\xc3\xa9\xf0\x9f\x90\x84");
    CHECK(v["missing"].isNull());
    CHECK(v["missing"]["deeper"].isNull());
    CHECK(v["items"][5].isNull());
    CHECK(v["cmd"].asInt(9) == 9);
}

TEST_CASE("json: whitespace, numbers, nesting") {
    const Value v = must(" [ 1 , -2.5e3 , 0 , [ [ ] ] , { } ] ");
    CHECK(v.size() == 5);
    CHECK(v[0].asInt() == 1);
    CHECK(v[1].asNumber() == -2500.0);
    CHECK(v[3][0].isArray());
    CHECK(v[4].isObject());
}

TEST_CASE("json: rejects malformed input") {
    CHECK(fails(""));
    CHECK(fails("{"));
    CHECK(fails("[1,]"));
    CHECK(fails("{\"a\" 1}"));
    CHECK(fails("nul"));
    CHECK(fails("01"));
    CHECK(fails("1."));
    CHECK(fails("\"unterminated"));
    CHECK(fails("\"tab\tinside\""));
    CHECK(fails("{} x"));
    CHECK(fails("\"\\u12\""));
    std::string deep(100, '[');
    CHECK(fails(deep.c_str()));
}

TEST_CASE("json: dumps deterministically and escapes") {
    Value v = Value::object();
    v.set("b", 2).set("a", "x\"y\n\x01").set("n", nullptr).set("t", true);
    v.set("list", Value::array().push(1.5).push(Value{{"k", "v"}}));
    v.set("big", int64_t(1) << 40);
    CHECK(v.dump() ==
          R"json({"a":"x\"y\n\u0001","b":2,"big":1099511627776,"list":[1.5,{"k":"v"}],"n":null,"t":true})json");
    Value back = must(v.dump().c_str());
    CHECK(back.dump() == v.dump());
    CHECK(kloud::json::escape("a\\b") == "\"a\\\\b\"");
}
