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
 * 8Kloud Switcher against the NVIDIA CUDA / Video Codec SDK runtime (CUDA,
 * NVENC, NVDEC), the OMT (libomt / libvmx) runtime, and the Blackmagic
 * DeckLink SDK, and distribute the combined work. See EXCEPTIONS.md for
 * the full exception text. */

// kloud-latmeter: OMT receiver that decodes kloud-testgen's strips and reports
// end-to-end latency, frame continuity, effective fps, and A/V sync offset.
//
// Latency = CLOCK_REALTIME(now) - wallclock strip (valid same-host, or across
// PTP-synced hosts). A/V offset = tone-burst onset timestamp - flash frame
// timestamp (sender-stamped, so it measures the OMT chain, not our receive).

#include <algorithm>
#include <cctype>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include "common/pattern.h"
#include "core/Log.h"
#include "core/MediaClock.h"

#include <libomt.h>

namespace pat = kloud::pattern;

namespace {

volatile std::sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }

struct Options {
    std::string source = "KloudTestgen";
    double findTimeout = 10.0;
    double duration = 0;  // 0 = until signal
    std::string csvPath;
    bool quiet = false;
};

bool parseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "--source") {
            if (const char* v = next()) o.source = v; else return false;
        } else if (a == "--find-timeout") {
            const char* v = next();
            if (!v) return false;
            o.findTimeout = atof(v);
        } else if (a == "--duration") {
            const char* v = next();
            if (!v) return false;
            o.duration = atof(v);
        } else if (a == "--csv") {
            if (const char* v = next()) o.csvPath = v; else return false;
        } else if (a == "--quiet") {
            o.quiet = true;
        } else {
            fprintf(stderr,
                    "usage: kloud-latmeter [--source SUBSTR] [--find-timeout S]\n"
                    "                    [--duration S] [--csv PATH] [--quiet]\n");
            return false;
        }
    }
    return true;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

int64_t realtimeNs() {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return int64_t(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, opt)) return 2;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    // -- discover the source. An explicit omt:// URL skips discovery. --
    std::string address;
    if (opt.source.rfind("omt://", 0) == 0) {
        address = opt.source;
    } else {
        const std::string want = lower(opt.source);
        const int64_t findDeadline =
            kloud::MediaClock::nowNs() + int64_t(opt.findTimeout * 1e9);
        while (!g_stop && address.empty() &&
               kloud::MediaClock::nowNs() < findDeadline) {
            int count = 0;
            char** list = omt_discovery_getaddresses(&count);
            for (int i = 0; list && i < count; ++i)
                if (list[i] && lower(list[i]).find(want) != std::string::npos) {
                    address = list[i];
                    break;
                }
            if (address.empty())
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (address.empty()) {
            KLOUD_LOGE("no OMT source matching '%s' within %.1fs",
                     opt.source.c_str(), opt.findTimeout);
            return 2;
        }
    }
    KLOUD_LOGI("connecting to '%s'", address.c_str());

    omt_receive_t* recv = omt_receive_create(
        address.c_str(), OMTFrameType(OMTFrameType_Video | OMTFrameType_Audio),
        OMTPreferredVideoFormat_UYVY, OMTReceiveFlags_None);
    if (!recv) {
        KLOUD_LOGE("omt_receive_create('%s') failed", address.c_str());
        return 1;
    }

    FILE* csv = nullptr;
    if (!opt.csvPath.empty()) {
        csv = fopen(opt.csvPath.c_str(), "w");
        if (!csv) {
            KLOUD_LOGE("cannot open csv '%s'", opt.csvPath.c_str());
            return 2;
        }
        fprintf(csv,
                "time_s,frames,fps,gaps,bad_parity,lat_avg_ms,lat_min_ms,"
                "lat_max_ms,av_offset_ms\n");
    }

    // -- receive loop --
    const int64_t startNs = kloud::MediaClock::nowNs();
    const int64_t endNs =
        opt.duration > 0 ? startNs + int64_t(opt.duration * 1e9) : 0;

    int64_t totalFrames = 0, totalGaps = 0, totalBad = 0;
    int64_t lastCounter = -1;

    int64_t winStart = startNs, winFrames = 0, winGaps = 0, winBad = 0;
    double winLatSum = 0, winLatMin = 1e18, winLatMax = -1e18;
    int64_t winLatCount = 0;

    // A/V pairing state (sender timecodes, 100ns units).
    int64_t lastFlashT = INT64_MIN, lastOnsetT = INT64_MIN;
    double avOffsetMs = NAN;
    int quietRun = 1 << 30;  // start "quiet" so the first burst counts

    double latAvgAll = 0;
    int64_t latCountAll = 0;

    while (!g_stop && (endNs == 0 || kloud::MediaClock::nowNs() < endNs)) {
        // libomt owns the returned frame; there is nothing to free. Video
        // pointers survive a following audio receive, but not vice versa.
        OMTMediaFrame* fr = omt_receive(
            recv, OMTFrameType(OMTFrameType_Video | OMTFrameType_Audio), 500);

        if (fr && fr->Type == OMTFrameType_Video && fr->Data) {
            ++totalFrames;
            ++winFrames;
            const uint8_t* data = static_cast<const uint8_t*>(fr->Data);
            const int stride = fr->Stride;

            uint64_t counter = 0, sendNs = 0;
            const bool okC = pat::readStrip(data, stride, pat::kCounterRow, counter);
            const bool okT = pat::readStrip(data, stride, pat::kTimeRow, sendNs);
            if (!okC || !okT) {
                ++totalBad;
                ++winBad;
            } else {
                if (lastCounter >= 0 && int64_t(counter) > lastCounter + 1) {
                    const int64_t g = int64_t(counter) - lastCounter - 1;
                    totalGaps += g;
                    winGaps += g;
                }
                lastCounter = int64_t(counter);

                const double latMs = double(realtimeNs() - int64_t(sendNs)) / 1e6;
                if (latMs < -1000.0 || latMs > 10'000.0) {
                    // Blended strips can pass 8-bit parity by luck (~1/256)
                    // during transitions; discard absurd timestamps.
                    ++totalBad;
                    ++winBad;
                    continue;
                }
                winLatSum += latMs;
                winLatMin = std::min(winLatMin, latMs);
                winLatMax = std::max(winLatMax, latMs);
                ++winLatCount;
                latAvgAll += latMs;
                ++latCountAll;

                if (pat::readFlash(data, stride)) {
                    lastFlashT = fr->Timestamp;
                    if (std::abs(lastFlashT - lastOnsetT) < 5'000'000)  // 0.5 s
                        avOffsetMs = double(lastOnsetT - lastFlashT) / 1e4;
                }
            }
        } else if (fr && fr->Type == OMTFrameType_Audio) {
            if (fr->Codec == OMTCodec_FPA1 && fr->Channels > 0 && fr->Data) {
                // Onset detect with a hold zone: the burst is a sine ramping
                // from zero, so early samples sit between the quiet (0.05)
                // and trigger (0.15) thresholds and must not disarm us.
                const float* ch0 = static_cast<const float*>(fr->Data);
                for (int i = 0; i < fr->SamplesPerChannel; ++i) {
                    const float x = std::fabs(ch0[i]);
                    if (x > 0.15f) {
                        if (quietRun > pat::kSampleRate / 20) {  // >=50ms quiet
                            lastOnsetT =
                                fr->Timestamp +
                                int64_t(i) * 10'000'000 / pat::kSampleRate;
                            if (std::abs(lastFlashT - lastOnsetT) < 5'000'000)
                                avOffsetMs = double(lastOnsetT - lastFlashT) / 1e4;
                        }
                        quietRun = 0;
                    } else if (x < 0.05f) {
                        ++quietRun;
                    }  // else: ramp zone, hold state
                }
            }
        }

        const int64_t nowNs = kloud::MediaClock::nowNs();
        if (nowNs - winStart >= 1'000'000'000) {
            const double dt = double(nowNs - winStart) / 1e9;
            const double fps = winFrames / dt;
            const double lavg = winLatCount ? winLatSum / winLatCount : NAN;
            if (!opt.quiet)
                KLOUD_LOGI(
                    "fps=%6.2f frames=%lld gaps=%lld bad=%lld lat(ms) "
                    "avg=%6.2f min=%6.2f max=%6.2f av=%+.2fms",
                    fps, (long long)totalFrames, (long long)totalGaps,
                    (long long)totalBad, lavg,
                    winLatCount ? winLatMin : NAN, winLatCount ? winLatMax : NAN,
                    avOffsetMs);
            if (csv) {
                fprintf(csv, "%.3f,%lld,%.3f,%lld,%lld,%.3f,%.3f,%.3f,%.3f\n",
                        double(nowNs - startNs) / 1e9, (long long)totalFrames,
                        fps, (long long)winGaps, (long long)winBad, lavg,
                        winLatCount ? winLatMin : NAN,
                        winLatCount ? winLatMax : NAN, avOffsetMs);
                fflush(csv);
            }
            winStart = nowNs;
            winFrames = winGaps = winBad = 0;
            winLatSum = 0;
            winLatMin = 1e18;
            winLatMax = -1e18;
            winLatCount = 0;
        }
    }

    omt_receive_destroy(recv);

    KLOUD_LOGI("summary: frames=%lld gaps=%lld bad=%lld lat_avg=%.2fms av=%+.2fms",
             (long long)totalFrames, (long long)totalGaps, (long long)totalBad,
             latCountAll ? latAvgAll / latCountAll : NAN, avOffsetMs);
    if (csv) fclose(csv);
    return totalFrames > 0 ? 0 : 1;
}
