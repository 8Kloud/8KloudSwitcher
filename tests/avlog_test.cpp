/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The libav log quiet scope: per thread, nestable, keeps FATAL, and leaves
// every other thread's output alone.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>

#include "media/AvLog.h"

extern "C" {
#include <libavutil/log.h>
}

using namespace kloud::media;

namespace {

std::atomic<int> gForwarded{0};

void countingSink(void*, int, const char*, va_list) { ++gForwarded; }

struct SinkGuard {
    SinkGuard() {
        gForwarded = 0;
        setAvLogSink(&countingSink);
        installAvLog();
    }
    ~SinkGuard() { setAvLogSink(nullptr); }
};

}  // namespace

TEST_CASE("AvLogQuiet drops libav messages on the calling thread only") {
    SinkGuard guard;

    av_log(nullptr, AV_LOG_ERROR, "loud\n");
    CHECK(gForwarded == 1);

    {
        AvLogQuiet quiet;
        av_log(nullptr, AV_LOG_ERROR, "probe noise\n");
        av_log(nullptr, AV_LOG_WARNING, "probe noise\n");
        av_log(nullptr, AV_LOG_INFO, "probe noise\n");
        CHECK(gForwarded == 1);

        // FATAL and PANIC still get through: those are not probe noise.
        av_log(nullptr, AV_LOG_FATAL, "fatal\n");
        CHECK(gForwarded == 2);

        // Nesting: the outer scope keeps quiet after the inner one ends.
        {
            AvLogQuiet inner;
            av_log(nullptr, AV_LOG_ERROR, "still quiet\n");
        }
        av_log(nullptr, AV_LOG_ERROR, "still quiet\n");
        CHECK(gForwarded == 2);

        // Another thread is unaffected by this thread's scope.
        std::thread([] { av_log(nullptr, AV_LOG_ERROR, "other thread\n"); })
            .join();
        CHECK(gForwarded == 3);
    }

    av_log(nullptr, AV_LOG_ERROR, "loud again\n");
    CHECK(gForwarded == 4);
}
