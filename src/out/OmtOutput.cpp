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

#include "out/OmtOutput.h"

#include "core/Log.h"

#ifndef KLOUD_HAVE_OMT

// Built without the OMT SDK: the engine still constructs the output (so no
// caller needs an #ifdef), it just never comes up.
namespace kloud {

OmtOutput::OmtOutput(std::string name, gpu::Compositor& comp,
                     gpu::Timeline& readbackTL, gpu::Compositor::Feed feed)
    : name_(std::move(name)), comp_(comp), readbackTL_(readbackTL),
      feed_(feed) {
    KLOUD_LOGE("OMT out: '%s' requested but built without the OMT SDK",
               name_.c_str());
}

OmtOutput::~OmtOutput() = default;
void OmtOutput::sendAudio(const float*, int, int64_t) {}
void OmtOutput::run(std::stop_token) {}

}  // namespace kloud

#else

#include <libomt.h>

#include "audio/MixerCore.h"
#include "core/MediaClock.h"
#include "core/Stats.h"

namespace kloud {

OmtOutput::OmtOutput(std::string name, gpu::Compositor& comp,
                     gpu::Timeline& readbackTL, gpu::Compositor::Feed feed)
    : name_(std::move(name)), comp_(comp), readbackTL_(readbackTL),
      feed_(feed) {
    // Quality_Default defers to whatever the connected receivers ask for.
    sender_ = omt_send_create(name_.c_str(), OMTQuality_Default);
    if (!sender_) {
        KLOUD_LOGE("OMT out: send_create('%s') failed", name_.c_str());
        return;
    }
    char addr[OMT_MAX_STRING_LENGTH] = {};
    omt_send_getaddress(sender_, addr, sizeof addr);
    thread_ = std::jthread([this](std::stop_token st) { run(st); });
    KLOUD_LOGI("OMT out: '%s' created (%s)", name_.c_str(), addr);
}

OmtOutput::~OmtOutput() {
    thread_ = {};  // stop + join before the sender goes away
    if (sender_) omt_send_destroy(sender_);
}

void OmtOutput::sendAudio(const float* lr, int frames, int64_t firstSample) {
    if (!sender_ || frames <= 0) return;
    audioScratch_.resize(size_t(frames) * 2);
    float* l = audioScratch_.data();
    float* r = l + frames;
    for (int f = 0; f < frames; ++f) {
        l[f] = lr[2 * f];
        r[f] = lr[2 * f + 1];
    }
    OMTMediaFrame af{};
    af.Type = OMTFrameType_Audio;
    af.Codec = OMTCodec_FPA1;
    af.SampleRate = audio::kSampleRate;
    af.Channels = 2;
    af.SamplesPerChannel = frames;
    // First-sample time on the media-clock origin, same domain as the video
    // timestamps below (100 ns units).
    af.Timestamp =
        (originNs_ + firstSample * 1'000'000'000LL / audio::kSampleRate) / 100;
    af.Data = audioScratch_.data();
    af.DataLength = frames * 2 * int(sizeof(float));
    omt_send(sender_, &af);
    audioSent_.fetch_add(1, std::memory_order_relaxed);
}

void OmtOutput::run(std::stop_token st) {
    const std::string prefix =
        feed_ == gpu::Compositor::Feed::Clean ? "out.omt.clean" : "out.omt";
    auto& sentCtr = Stats::counter(prefix + ".sent");
    auto& skipCtr = Stats::counter(prefix + ".droppedToLatest");

    const auto& show = comp_.showFormat();
    OMTMediaFrame vf{};
    vf.Type = OMTFrameType_Video;
    vf.Codec = OMTCodec_UYVY;
    vf.Width = show.width;
    vf.Height = show.height;
    vf.Stride = show.width * 2;
    vf.FrameRateN = int(show.fpsN);
    vf.FrameRateD = int(show.fpsD);
    vf.AspectRatio = float(show.width) / float(show.height);
    vf.ColorSpace = show.colorimetry == Colorimetry::BT601
                        ? OMTColorSpace_BT601
                        : OMTColorSpace_BT709;
    vf.DataLength = show.width * 2 * show.height;

    uint64_t lastSent = 0;

    while (!st.stop_requested()) {
        if (!readbackTL_.waitCompleted(lastSent + 1, 100'000'000)) continue;
        const uint64_t newest = readbackTL_.completed();
        if (newest <= lastSent) continue;
        if (newest > lastSent + 1) skipCtr.add(int64_t(newest - lastSent - 1));

        // Find the slot stamped with `newest` (engine stamps before submit).
        int slot = -1;
        for (int s = 0; s < gpu::Compositor::kPackSlots; ++s)
            if (comp_.packStamp(s, feed_).load(std::memory_order_acquire) ==
                newest) {
                slot = s;
                break;
            }
        lastSent = newest;
        if (slot < 0) continue;  // engine skipped packing that tick

        // Pin before reading, then confirm the slot still holds our frame:
        // the render thread may have recycled it while we were looking.
        if (!comp_.packTryPin(slot, feed_)) continue;
        if (comp_.packStamp(slot, feed_).load(std::memory_order_acquire) !=
            newest) {
            comp_.packUnpin(slot, feed_);
            continue;
        }

        // The stamp above is the acquire that publishes this tick's pts.
        const int64_t ptsNs = slotPts_[size_t(slot)].load(
            std::memory_order_relaxed);
        vf.Timestamp = (ptsNs ? ptsNs : MediaClock::nowNs()) / 100;

        vf.Data = const_cast<uint8_t*>(comp_.packPtr(slot, feed_));
        omt_send(sender_, &vf);  // VMX encode happens in this call
        // Synchronous: our buffer is free the moment it returns.
        comp_.packUnpin(slot, feed_);
        sent_.fetch_add(1, std::memory_order_relaxed);
        sentCtr.add();
    }
}

}  // namespace kloud

#endif  // KLOUD_HAVE_OMT
