/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once
#include <cstdint>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

namespace kloud::web {

// RGBA -> JPEG through libavcodec's mjpeg encoder, for the browser
// multiview. The wall is downscaled to `dstW` wide (aspect kept) so a tablet
// on Wi-Fi stays fluid; quality is the mjpeg qscale (2 best .. 31 worst).
class MjpegEncoder {
public:
    ~MjpegEncoder() { close(); }

    bool open(int srcW, int srcH, int dstW, int quality);
    void close();
    bool ok() const { return enc_ != nullptr; }
    int width() const { return dstW_; }
    int height() const { return dstH_; }

    // `rgba` is srcW x srcH tightly packed. Appends nothing on failure.
    bool encode(const uint8_t* rgba, std::string& jpeg);

private:
    AVCodecContext* enc_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* pkt_ = nullptr;
    SwsContext* sws_ = nullptr;
    int srcW_ = 0, srcH_ = 0, dstW_ = 0, dstH_ = 0;
    int64_t pts_ = 0;
};

}  // namespace kloud::web
