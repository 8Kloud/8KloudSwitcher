/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once
#include <cstdarg>

namespace kloud::media {

// libav logging. FFmpeg's own callback (stderr, repeat suppression) stays the
// sink; the only addition is a per-thread quiet scope, so a caller that knows
// it is about to make libav complain harmlessly can hide that without hiding
// anything another thread reports at the same time.
//
// The case: a live stream is joined wherever the connection landed, which is
// mid-GOP until the next keyframe, and every packet until then is an error
// to a decoder. avformat_find_stream_info() feeds them to FFmpeg's default
// software decoder ("PPS id out of range", "Skipping invalid undecodable
// NALU", for AV1 "No sequence header available"), then the real NVDEC decode
// sees the rest ("Missing Sequence Header", "Error constructing the frame
// RPS") -- a hundred lines per connect that read as a fault and are not one.
// The probe runs its decoders with threads=1 and the decode loop is one
// thread, so a thread-local flag on the SrtInput thread catches all of it.

// Installs the process-wide callback. Idempotent; the first caller wins.
void installAvLog();

// Quiets libav on the calling thread for the scope's lifetime: messages less
// severe than AV_LOG_FATAL are dropped. Nestable. Other threads are unaffected.
class AvLogQuiet {
public:
    AvLogQuiet();
    ~AvLogQuiet();
    AvLogQuiet(const AvLogQuiet&) = delete;
    AvLogQuiet& operator=(const AvLogQuiet&) = delete;
};

// Test seam: where the callback forwards messages it does not drop. nullptr
// restores av_log_default_callback.
using AvLogSink = void (*)(void* avcl, int level, const char* fmt, va_list vl);
void setAvLogSink(AvLogSink sink);

}  // namespace kloud::media
