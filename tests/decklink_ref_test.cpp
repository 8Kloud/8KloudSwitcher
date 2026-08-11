/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <catch2/catch_test_macros.hpp>

#include "decklink/DeckLinkRef.h"

using kloud::isDeckLinkRef;
using kloud::parseDeckLinkRef;

TEST_CASE("decklink ref: device index") {
    const auto r = parseDeckLinkRef("decklink://0");
    REQUIRE(r.valid);
    REQUIRE(r.byIndex());
    REQUIRE(r.index == 0);
    REQUIRE(r.name.empty());
    REQUIRE(r.mode.empty());  // auto-detect

    const auto r2 = parseDeckLinkRef("decklink://3");
    REQUIRE(r2.valid);
    REQUIRE(r2.index == 3);
}

TEST_CASE("decklink ref: device name") {
    const auto r = parseDeckLinkRef("decklink://DeckLink 8K Pro (1)");
    REQUIRE(r.valid);
    REQUIRE_FALSE(r.byIndex());
    REQUIRE(r.name == "DeckLink 8K Pro (1)");
    REQUIRE(r.mode.empty());
}

TEST_CASE("decklink ref: forced mode") {
    const auto r = parseDeckLinkRef("decklink://0@4320p59.94");
    REQUIRE(r.valid);
    REQUIRE(r.byIndex());
    REQUIRE(r.index == 0);
    REQUIRE(r.mode == "4320p59.94");

    const auto n = parseDeckLinkRef("decklink://DeckLink 8K Pro (1)@2160p60");
    REQUIRE(n.valid);
    REQUIRE(n.name == "DeckLink 8K Pro (1)");
    REQUIRE(n.mode == "2160p60");
}

TEST_CASE("decklink ref: rejects non-decklink refs") {
    for (const char* s : {"omt://host:6400", "srt://1.2.3.4:9000", "CAM1",
                          "", "decklink:/0", "decklinkx://0"}) {
        REQUIRE_FALSE(isDeckLinkRef(s));
        REQUIRE_FALSE(parseDeckLinkRef(s).valid);
    }
}

TEST_CASE("decklink ref: scheme with no device is invalid") {
    REQUIRE(isDeckLinkRef("decklink://"));  // sniffs as ours...
    REQUIRE_FALSE(parseDeckLinkRef("decklink://").valid);  // ...but names nothing
    REQUIRE_FALSE(parseDeckLinkRef("decklink://@2160p60").valid);
}

TEST_CASE("decklink ref: name containing '@' splits on the last one") {
    const auto r = parseDeckLinkRef("decklink://odd@name@1080p50");
    REQUIRE(r.valid);
    REQUIRE(r.name == "odd@name");
    REQUIRE(r.mode == "1080p50");
}
