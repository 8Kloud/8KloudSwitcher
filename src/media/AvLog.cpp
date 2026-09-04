/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "media/AvLog.h"

#include <atomic>
#include <mutex>

extern "C" {
#include <libavutil/log.h>
}

namespace kloud::media {
namespace {

thread_local int tlQuietDepth = 0;
std::atomic<AvLogSink> gSink{nullptr};

void callback(void* avcl, int level, const char* fmt, va_list vl) {
    // libav levels grow less severe upward: PANIC 0, FATAL 8, ERROR 16 ...
    if (tlQuietDepth > 0 && level > AV_LOG_FATAL) return;
    AvLogSink sink = gSink.load(std::memory_order_acquire);
    (sink ? sink : &av_log_default_callback)(avcl, level, fmt, vl);
}

}  // namespace

void installAvLog() {
    static std::once_flag once;
    std::call_once(once, [] { av_log_set_callback(&callback); });
}

AvLogQuiet::AvLogQuiet() { ++tlQuietDepth; }
AvLogQuiet::~AvLogQuiet() { --tlQuietDepth; }

void setAvLogSink(AvLogSink sink) {
    gSink.store(sink, std::memory_order_release);
}

}  // namespace kloud::media
