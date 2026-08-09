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
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/Spsc.h"
#include "decklink/DeckLinkRef.h"
#include "engine/IInputSource.h"
#include "gpu/UploadRing.h"

class IDeckLink;
class IDeckLinkInput;
class IDeckLinkVideoInputFrame;
class IDeckLinkAudioInputPacket;

namespace kloud {

// One DeckLink SDI input, capturing 8-bit YUV (bmdFormat8BitYUV) -- which is
// UYVY 4:2:2, byte-identical to our native PixFmt::UYVY8_422, so frames go
// straight into the upload ring with no conversion.
//
// Unlike the network inputs there is no capture thread of our own: the SDK
// delivers frames on its own callback thread and we do the staging copy +
// upload submit there, exactly the work OmtInput does on its own. An
// open thread runs only to (re)open the device when it is absent or busy.
//
// Frame sync gets a real hardware clock here: GetHardwareReferenceTimestamp is
// the card's own reference, so pts is never synthesized (senderClock = true).
class DeckLinkInput : public IInputSource {
public:
    // ref: see DeckLinkRef.h. syncFrames >= 0 enables the frame-sync feed.
    DeckLinkInput(gpu::VkEngine& eng, gpu::Queue& uploadQueue, std::string ref,
                  int index, int syncFrames = -1);
    ~DeckLinkInput() override;

    std::optional<Mailbox::Item> newer(uint64_t lastSeq) const override {
        return mailbox_.takeNewer(lastSeq);
    }
    int newerCandidates(uint64_t lastSeq, Mailbox::Item* out) const override {
        return mailbox_.takeNewerCandidates(lastSeq, out);
    }

    Status status() const override;
    const std::string& ref() const { return ref_; }

    void attachAudioSink(audio::InputChannel* ch) override {
        audioSink_.store(ch, std::memory_order_release);
    }

    SyncFeed* syncFeed() override { return syncFrames_ >= 0 ? &feed_ : nullptr; }

    // Device display names, in SDK enumeration order (index = position). Empty
    // when no driver/card is present. Safe to call without an instance.
    static std::vector<std::string> devices();

    // Called by the SDK callback shim; public only for that.
    void onFrame(IDeckLinkVideoInputFrame* video, IDeckLinkAudioInputPacket* audio);
    void onFormatChanged(unsigned events, void* newMode, unsigned flags);

private:
    void run(std::stop_token st);
    bool open();          // open thread only
    void close();         // open thread / dtor only
    void startStreams(void* displayMode, bool applyMode);

    gpu::VkEngine& eng_;
    gpu::Queue& queue_;
    std::string ref_;
    const DeckLinkRef parsed_;
    int index_;

    IDeckLink* device_ = nullptr;
    IDeckLinkInput* input_ = nullptr;
    class Delegate;
    Delegate* delegate_ = nullptr;

    // Open-thread owned; keeps the retry loop from spamming the log.
    int64_t lastEnableLogNs_ = 0;
    bool everLoggedOpen_ = false;

    // Callback-thread owned.
    std::shared_ptr<gpu::UploadRing> ring_;
    int64_t fpsN_ = 60000, fpsD_ = 1001;
    uint64_t pubSeq_ = 0;

    Mailbox mailbox_;
    const int syncFrames_;
    SyncFeed feed_{16};
    std::atomic<audio::InputChannel*> audioSink_{nullptr};
    std::vector<float> audioL_, audioR_;  // callback-thread scratch

    std::atomic<bool> connected_{false};
    std::atomic<bool> streaming_{false};
    std::atomic<int64_t> frames_{0};
    std::atomic<int64_t> drops_{0};
    std::atomic<int64_t> lastFrameNs_{0};
    mutable std::mutex descM_;
    VideoFormatDesc desc_{};

    std::jthread thread_;
};

}  // namespace kloud
