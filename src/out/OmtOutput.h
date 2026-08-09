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
#include <string>
#include <thread>
#include <vector>

#include "gpu/Compositor.h"
#include "gpu/VkEngine.h"

typedef long long omt_send_t;  // matches libomt.h; keep it out of headers

namespace kloud {

// OMT (Open Media Transport) program output: waits for completed pack
// readbacks and hands the host UYVY buffer straight to omt_send, which VMX-
// encodes from our memory inside the call, so a slot is only pinned for the
// duration of that one call. Drop-to-latest when the encoder falls behind.
// Audio is embedded on the same sender via
// sendAudio(); libomt's send API is thread-safe, so the mixer thread calls it
// directly while this thread sends video.
//
// Timestamps are CLOCK_MONOTONIC ns in OMT's 100 ns units. Video carries the
// composite tick's presentation time (stamped by the render thread, not the
// send time, so the readback lag does not skew A/V) and audio carries the
// mixer's sample position on the same origin.
class OmtOutput {
public:
    OmtOutput(std::string name, gpu::Compositor& comp,
              gpu::Timeline& readbackTL,
              gpu::Compositor::Feed feed = gpu::Compositor::Feed::Program);
    ~OmtOutput();

    bool ok() const { return sender_ != nullptr; }
    int64_t framesSent() const { return sent_.load(std::memory_order_relaxed); }
    int64_t audioChunksSent() const {
        return audioSent_.load(std::memory_order_relaxed);
    }

    // Media-clock origin (CLOCK_MONOTONIC ns) that sendAudio's sample indices
    // count from. Set once before the mixer starts.
    void setClockOrigin(int64_t originNs) { originNs_ = originNs; }

    // Render thread: presentation time of the tick being packed into `slot`.
    // Call before stamping the compositor's pack slot -- that release store is
    // what publishes this value to the send thread.
    void stampSlot(int slot, int64_t ptsNs) {
        slotPts_[size_t(slot)].store(ptsNs, std::memory_order_relaxed);
    }

    // Mixer thread: embed one master-bus chunk (interleaved stereo, 48 kHz).
    void sendAudio(const float* lr, int frames, int64_t firstSample);

private:
    void run(std::stop_token st);

    std::string name_;
    gpu::Compositor& comp_;
    gpu::Timeline& readbackTL_;
    gpu::Compositor::Feed feed_;
    omt_send_t* sender_ = nullptr;
    int64_t originNs_ = 0;
    std::atomic<int64_t> sent_{0};
    std::atomic<int64_t> audioSent_{0};
    std::array<std::atomic<int64_t>, gpu::Compositor::kPackSlots> slotPts_{};
    std::vector<float> audioScratch_;  // mixer-thread only (deinterleave)
    std::jthread thread_;
};

}  // namespace kloud
