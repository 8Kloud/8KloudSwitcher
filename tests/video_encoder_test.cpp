/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <catch2/catch_test_macros.hpp>

#include "media/IVideoEncoder.h"

using namespace kloud;

TEST_CASE("video codec names round-trip") {
    media::VideoCodec codec = media::VideoCodec::Hevc;

    CHECK(media::parseVideoCodec("av1", codec));
    CHECK(codec == media::VideoCodec::Av1);
    CHECK(std::string_view(media::videoCodecName(codec)) == "av1");

    CHECK(media::parseVideoCodec("hevc", codec));
    CHECK(codec == media::VideoCodec::Hevc);
    CHECK(std::string_view(media::videoCodecName(codec)) == "hevc");

    CHECK_FALSE(media::parseVideoCodec("h264", codec));
    CHECK(codec == media::VideoCodec::Hevc);
}
