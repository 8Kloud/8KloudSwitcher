/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
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
// upload submit there, exactly the work OmtInput does on its own. A worker
// thread (re)opens the device when it is absent or busy, and is the ONLY
// thread that ever touches device_/input_/delegate_ -- see onFormatChanged.
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
    // What EnableVideoInput needs, as plain values. The SDK's
    // IDeckLinkDisplayMode* is only valid inside the callback that delivered
    // it, and the restart it triggers now runs on another thread entirely.
    // displayMode is a BMDDisplayMode (a 32-bit FourCC), kept untyped so this
    // header does not have to pull in the SDK.
    struct ModeInfo {
        uint32_t displayMode = 0;
        int64_t fpsN = 0, fpsD = 0;  // 0 = leave the current rate alone
        bool valid = false;          // false = seed mode, i.e. auto-detect
    };
    static ModeInfo modeInfoFrom(void* displayMode);

    void run(std::stop_token st);
    bool open();                              // worker thread only
    void close();                             // worker thread / dtor only
    void startStreams(const ModeInfo& mode);  // worker thread only
    void applyPendingFormat();                // worker thread only

    gpu::VkEngine& eng_;
    gpu::Queue& queue_;
    std::string ref_;
    const DeckLinkRef parsed_;
    int index_;

    // Worker-thread owned, every one of them. Nothing on the SDK callback
    // thread may read or write these: close() frees them, and a callback
    // holding a stale pointer across that is a use-after-free.
    IDeckLink* device_ = nullptr;
    IDeckLinkInput* input_ = nullptr;
    class Delegate;
    Delegate* delegate_ = nullptr;

    // Format-change handoff. onFormatChanged runs on the SDK callback thread,
    // so it records what the card reported and wakes the worker instead of
    // restarting the streams itself. No lock is ever held across an SDK call:
    // StopStreams can block on an in-flight callback, and a callback blocked
    // on formatM_ would deadlock against it.
    std::mutex formatM_;
    std::condition_variable_any formatCv_;
    ModeInfo pendingFormat_;
    bool formatPending_ = false;

    // Worker-thread owned; keeps the retry loop from spamming the log.
    int64_t lastEnableLogNs_ = 0;
    bool everLoggedOpen_ = false;

    // Callback-thread owned.
    std::shared_ptr<gpu::UploadRing> ring_;
    uint64_t pubSeq_ = 0;
    // Written by startStreams, which runs on the open thread via open() AND on
    // the SDK callback thread via onFormatChanged; read by onFrame. Not
    // "callback-thread owned" despite sitting next to things that are.
    std::atomic<int64_t> fpsN_{60000}, fpsD_{1001};

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
