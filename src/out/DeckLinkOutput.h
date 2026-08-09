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

#pragma once
#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "decklink/DeckLinkRef.h"
#include "gpu/Compositor.h"
#include "gpu/VkEngine.h"

class IDeckLink;
class IDeckLinkOutput;
class IDeckLinkMutableVideoFrame;
class IDeckLinkVideoFrame;

namespace kloud {

// SDI program output on a DeckLink sub-device. The compositor already packs
// program to UYVY for the network sender, and bmdFormat8BitYUV IS that layout,
// so this output needs no conversion -- it waits on the same readback timeline
// and copies the packed bytes into an SDK frame.
//
// The copy is deliberate. Scheduled playback keeps a frame until the card has
// clocked it out, so wrapping our pack slots (CreateVideoFrameWithBuffer)
// would pin one ring slot per frame of preroll and starve the ring. Copying
// costs one frame memcpy and bounds the pin to microseconds, matching the OMT
// sender's shape.
//
// Clocking: MediaClock stays master (the engine's render tick paces us) and
// the card's scheduler absorbs the drift between it and the card's own clock.
// The buffered-frame depth is steered back toward kTargetPreroll by repeating
// or dropping a frame when it wanders, counted as out.sdi.repeats/.drops --
// this is monitoring-grade, not genlocked. Genlock would mean pacing the
// render loop off GetHardwareReferenceClock instead.
class DeckLinkOutput {
public:
    // ref: a decklink:// ref (see DeckLinkRef.h); an explicit @mode overrides
    // the show format, which otherwise selects the mode.
    DeckLinkOutput(std::string ref, gpu::Compositor& comp,
                   gpu::Timeline& readbackTL, const VideoFormatDesc& show,
                   gpu::Compositor::Feed feed = gpu::Compositor::Feed::Program);
    ~DeckLinkOutput();

    bool ok() const { return ok_.load(std::memory_order_relaxed); }
    int64_t framesSent() const { return sent_.load(std::memory_order_relaxed); }
    const std::string& ref() const { return ref_; }

    // Media-clock origin (CLOCK_MONOTONIC ns) that sendAudio's sample indices
    // count from. Set once before the mixer starts.
    void setClockOrigin(int64_t originNs) { originNs_ = originNs; }

    // Render thread: presentation time of the tick packed into `slot`. Same
    // contract as OmtOutput::stampSlot -- call before stamping the slot.
    void stampSlot(int slot, int64_t ptsNs) {
        slotPts_[size_t(slot)].store(ptsNs, std::memory_order_relaxed);
    }

    // Mixer thread: embed one master-bus chunk (interleaved stereo, 48 kHz).
    void sendAudio(const float* lr, int frames, int64_t firstSample);

private:
    class Callback;
    friend class Callback;

    void run(std::stop_token st);
    bool open();
    void close();
    bool writeFrame(IDeckLinkMutableVideoFrame* frame, const uint8_t* src);
    void schedule(IDeckLinkMutableVideoFrame* frame);
    void onFrameCompleted(IDeckLinkVideoFrame* frame, unsigned result);

    // Frames handed to the card ahead of the first one played out. Three is
    // the SDK's usual floor for a stable start without adding visible delay.
    static constexpr int kTargetPreroll = 3;
    static constexpr int kFrameRing = 6;  // >= preroll + one in hand

    std::string ref_;
    DeckLinkRef parsed_;
    gpu::Compositor& comp_;
    gpu::Timeline& readbackTL_;
    VideoFormatDesc show_;
    gpu::Compositor::Feed feed_;

    IDeckLink* device_ = nullptr;
    IDeckLinkOutput* output_ = nullptr;
    Callback* callback_ = nullptr;
    std::vector<IDeckLinkMutableVideoFrame*> frames_;  // our recycle pool
    std::mutex freeM_;
    std::vector<IDeckLinkMutableVideoFrame*> free_;

    // Mode timing, filled by open(): BMDTimeValue / BMDTimeScale are int64_t,
    // spelled plainly here so the header stays SDK-free.
    int64_t frameDuration_ = 0;
    int64_t frameTimescale_ = 0;

    int64_t originNs_ = 0;
    int64_t scheduledFrames_ = 0;  // send thread only: playback stream time
    int64_t audioScheduled_ = 0;   // mixer thread only: sample frames written
    std::vector<int32_t> audioScratch_;  // mixer thread only (interleave)

    std::atomic<bool> ok_{false};
    std::atomic<bool> playing_{false};
    std::atomic<bool> audioPrerolling_{false};
    std::atomic<int64_t> sent_{0};
    std::array<std::atomic<int64_t>, gpu::Compositor::kPackSlots> slotPts_{};
    std::jthread thread_;
};

}  // namespace kloud
