/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * Additional permission under GNU GPL version 3 section 7: you may link
 * 8Kloud Switcher against the proprietary NDI SDK, the NVIDIA CUDA / Video
 * Codec SDK runtime (CUDA, NVENC, NVDEC), the OMT (libomt / libvmx)
 * runtime, and the Blackmagic DeckLink SDK, and distribute the combined
 * work. See EXCEPTIONS.md for the full exception text. */

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
