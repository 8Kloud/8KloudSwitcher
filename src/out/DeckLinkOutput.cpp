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

#include "out/DeckLinkOutput.h"

#include "core/Log.h"

#ifndef KLOUD_HAVE_DECKLINK

// Built without the DeckLink SDK: the engine still constructs the output (so
// no caller needs an #ifdef), it just never comes up.
namespace kloud {

DeckLinkOutput::DeckLinkOutput(std::string ref, gpu::Compositor& comp,
                               gpu::Timeline& readbackTL,
                               const VideoFormatDesc& show,
                               gpu::Compositor::Feed feed)
    : ref_(std::move(ref)), comp_(comp), readbackTL_(readbackTL), show_(show),
      feed_(feed) {
    KLOUD_LOGE("sdi out: '%s' requested but built without the DeckLink SDK",
               ref_.c_str());
}

DeckLinkOutput::~DeckLinkOutput() = default;
void DeckLinkOutput::sendAudio(const float*, int, int64_t) {}
void DeckLinkOutput::run(std::stop_token) {}
bool DeckLinkOutput::open() { return false; }
void DeckLinkOutput::close() {}
bool DeckLinkOutput::writeFrame(IDeckLinkMutableVideoFrame*, const uint8_t*) {
    return false;
}
void DeckLinkOutput::schedule(IDeckLinkMutableVideoFrame*) {}
void DeckLinkOutput::onFrameCompleted(IDeckLinkVideoFrame*, unsigned) {}

}  // namespace kloud

#else

#include <cstring>

#include "DeckLinkAPI.h"
#include "core/Stats.h"

namespace kloud {
namespace {

// Same two shims as DeckLinkInput.cpp. They stay local rather than moving to
// DeckLinkRef.h, which is included by GUI code that must not pull in the SDK.
std::string takeString(const char* s) {
    std::string r = s ? s : "";
    if (s) free(const_cast<char*>(s));
    return r;
}

std::string displayName(IDeckLink* dl) {
    const char* n = nullptr;
    if (dl->GetDisplayName(&n) != S_OK) return {};
    return takeString(n);
}

}  // namespace

// COM shim, same ownership rule as the input's: the SDK holds a reference to
// this, never to DeckLinkOutput. close() clears the callback and drops that
// reference before the owner dies.
class DeckLinkOutput::Callback : public IDeckLinkVideoOutputCallback {
public:
    explicit Callback(DeckLinkOutput* owner) : owner_(owner) {}

    HRESULT ScheduledFrameCompleted(IDeckLinkVideoFrame* frame,
                                    BMDOutputFrameCompletionResult result) override {
        owner_->onFrameCompleted(frame, unsigned(result));
        return S_OK;
    }
    HRESULT ScheduledPlaybackHasStopped() override { return S_OK; }

    HRESULT QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG AddRef() override {
        return ULONG(refs_.fetch_add(1, std::memory_order_relaxed) + 1);
    }
    ULONG Release() override {
        const int n = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (n == 0) delete this;
        return ULONG(n);
    }

private:
    DeckLinkOutput* owner_;
    std::atomic<int> refs_{1};
};

DeckLinkOutput::DeckLinkOutput(std::string ref, gpu::Compositor& comp,
                               gpu::Timeline& readbackTL,
                               const VideoFormatDesc& show,
                               gpu::Compositor::Feed feed)
    : ref_(std::move(ref)),
      parsed_(parseDeckLinkRef(ref_)),
      comp_(comp),
      readbackTL_(readbackTL),
      show_(show),
      feed_(feed) {
    thread_ = std::jthread([this](std::stop_token st) { run(st); });
}

DeckLinkOutput::~DeckLinkOutput() {
    thread_ = {};  // stop + join before the SDK objects go away
    close();
}

bool DeckLinkOutput::open() {
    if (!parsed_.valid) {
        KLOUD_LOGE("sdi out: bad ref '%s' (expected decklink://INDEX or "
                 "decklink://NAME)", ref_.c_str());
        return false;
    }
    IDeckLinkIterator* it = CreateDeckLinkIteratorInstance();
    if (!it) return false;  // driver absent; the open loop retries

    IDeckLink* dl = nullptr;
    int i = 0;
    while (it->Next(&dl) == S_OK) {
        const std::string name = displayName(dl);
        const bool hit = parsed_.byIndex() ? (i == parsed_.index)
                                           : (name == parsed_.name);
        if (hit) {
            device_ = dl;  // keep the reference
            KLOUD_LOGI("sdi out: opening '%s'", name.c_str());
            break;
        }
        dl->Release();
        ++i;
    }
    it->Release();
    if (!device_) return false;

    if (device_->QueryInterface(IID_IDeckLinkOutput, (void**)&output_) != S_OK ||
        !output_) {
        KLOUD_LOGE("sdi out: device has no output interface (wrong profile? "
                 "see docs/decklink.md)");
        close();
        return false;
    }

    // Pick the display mode. An explicit @mode wins; otherwise match the show
    // format exactly -- the card cannot rescale, so a mismatch is an error
    // rather than something to paper over.
    IDeckLinkDisplayMode* chosen = nullptr;
    IDeckLinkDisplayModeIterator* mi = nullptr;
    if (output_->GetDisplayModeIterator(&mi) == S_OK) {
        IDeckLinkDisplayMode* m = nullptr;
        while (mi->Next(&m) == S_OK) {
            bool hit = false;
            if (!parsed_.mode.empty()) {
                const char* mn = nullptr;
                m->GetName(&mn);
                hit = takeString(mn) == parsed_.mode;
            } else {
                BMDTimeValue dur = 0;
                BMDTimeScale scale = 0;
                m->GetFrameRate(&dur, &scale);
                hit = m->GetWidth() == show_.width &&
                      m->GetHeight() == show_.height &&
                      int64_t(dur) == show_.fpsD &&
                      int64_t(scale) == show_.fpsN &&
                      m->GetFieldDominance() == bmdProgressiveFrame;
            }
            if (hit) {
                chosen = m;  // keep
                break;
            }
            m->Release();
        }
        mi->Release();
    }
    if (!chosen) {
        KLOUD_LOGE("sdi out: no mode matching %dx%d @ %lld/%lld%s%s",
                 show_.width, show_.height, (long long)show_.fpsN,
                 (long long)show_.fpsD, parsed_.mode.empty() ? "" : " / ",
                 parsed_.mode.c_str());
        close();
        return false;
    }
    const BMDDisplayMode mode = chosen->GetDisplayMode();
    chosen->GetFrameRate(&frameDuration_, &frameTimescale_);
    const long modeW = chosen->GetWidth(), modeH = chosen->GetHeight();
    chosen->Release();

    if (modeW != show_.width || modeH != show_.height) {
        KLOUD_LOGE("sdi out: mode '%s' is %ldx%ld but the show is %dx%d",
                 parsed_.mode.c_str(), modeW, modeH, show_.width, show_.height);
        close();
        return false;
    }

    if (const HRESULT hr =
            output_->EnableVideoOutput(mode, bmdVideoOutputFlagDefault);
        hr != S_OK) {
        // Half duplex: a sub-device already capturing will refuse to play out.
        KLOUD_LOGE("sdi out: EnableVideoOutput failed (0x%08x) -- is this "
                 "sub-device already capturing? see docs/decklink.md",
                 unsigned(hr));
        close();
        return false;
    }
    output_->EnableAudioOutput(bmdAudioSampleRate48kHz,
                               bmdAudioSampleType32bitInteger, 2,
                               bmdAudioOutputStreamContinuous);
    // The mixer starts feeding us before the video preroll completes. Without
    // preroll mode the card's audio queue accepts only a fraction of that and
    // silently truncates the rest (measured: 1680 sample frames = 35 ms), which
    // would leave a permanent A/V offset. Preroll mode holds the samples until
    // playback starts, so audio and video both begin at tick 0.
    output_->BeginAudioPreroll();
    audioPrerolling_ = true;

    // Frame pool: the card holds each frame until it has been clocked out, so
    // we recycle a fixed set rather than allocating per tick.
    const int32_t rowBytes = int32_t(show_.width) * 2;
    for (int f = 0; f < kFrameRing; ++f) {
        IDeckLinkMutableVideoFrame* frame = nullptr;
        if (output_->CreateVideoFrame(show_.width, show_.height, rowBytes,
                                      bmdFormat8BitYUV, bmdFrameFlagDefault,
                                      &frame) != S_OK ||
            !frame) {
            KLOUD_LOGE("sdi out: CreateVideoFrame failed");
            close();
            return false;
        }
        frames_.push_back(frame);
        free_.push_back(frame);
    }

    callback_ = new Callback(this);
    output_->SetScheduledFrameCompletionCallback(callback_);
    scheduledFrames_ = 0;
    audioScheduled_ = 0;
    ok_.store(true, std::memory_order_relaxed);
    KLOUD_LOGI("sdi out: %dx%d @ %lld/%lld ready on '%s'", show_.width,
             show_.height, (long long)show_.fpsN, (long long)show_.fpsD,
             ref_.c_str());
    return true;
}

void DeckLinkOutput::close() {
    ok_.store(false, std::memory_order_relaxed);
    if (output_) {
        if (playing_.exchange(false)) {
            BMDTimeValue stopped = 0;
            output_->StopScheduledPlayback(0, &stopped, frameTimescale_);
        }
        output_->SetScheduledFrameCompletionCallback(nullptr);
        output_->DisableAudioOutput();
        output_->DisableVideoOutput();
    }
    {
        std::lock_guard lk(freeM_);
        free_.clear();
    }
    for (auto* f : frames_)
        if (f) f->Release();
    frames_.clear();
    if (callback_) {
        callback_->Release();
        callback_ = nullptr;
    }
    if (output_) {
        output_->Release();
        output_ = nullptr;
    }
    if (device_) {
        device_->Release();
        device_ = nullptr;
    }
}

void DeckLinkOutput::onFrameCompleted(IDeckLinkVideoFrame* frame,
                                      unsigned result) {
    if (result == bmdOutputFrameDisplayedLate)
        Stats::counter("out.sdi.late").add();
    else if (result == bmdOutputFrameDropped)
        Stats::counter("out.sdi.dropped").add();
    else if (result == bmdOutputFrameFlushed)
        Stats::counter("out.sdi.flushed").add();

    std::lock_guard lk(freeM_);
    free_.push_back(static_cast<IDeckLinkMutableVideoFrame*>(frame));
}

void DeckLinkOutput::sendAudio(const float* lr, int frames, int64_t firstSample) {
    if (!ok_.load(std::memory_order_relaxed) || !output_ || frames <= 0) return;
    // The card takes 32-bit integer PCM; the mixer already limits to -1 dBFS,
    // so a straight scale with a clamp is all that is needed.
    audioScratch_.resize(size_t(frames) * 2);
    for (int f = 0; f < frames * 2; ++f) {
        const float v = lr[f] < -1.f ? -1.f : (lr[f] > 1.f ? 1.f : lr[f]);
        audioScratch_[size_t(f)] = int32_t(v * 2147483647.0f);
    }
    uint32_t written = 0;
    output_->ScheduleAudioSamples(audioScratch_.data(), uint32_t(frames), 0, 0,
                                  &written);
    audioScheduled_ += written;
    if (written < uint32_t(frames))
        Stats::counter("out.sdi.audioShort").add(int64_t(frames - written));
    (void)firstSample;  // continuous stream: the card keeps its own position
}

// SDK 16 moved pixel access behind IDeckLinkVideoBuffer on the output side
// too: IDeckLinkMutableVideoFrame has no GetBytes of its own, and the buffer
// must be checked out for writing before its pointer is valid.
bool DeckLinkOutput::writeFrame(IDeckLinkMutableVideoFrame* frame,
                                const uint8_t* src) {
    IDeckLinkVideoBuffer* buf = nullptr;
    if (frame->QueryInterface(IID_IDeckLinkVideoBuffer, (void**)&buf) != S_OK ||
        !buf)
        return false;
    struct BufGuard {
        IDeckLinkVideoBuffer* b;
        bool started = false;
        ~BufGuard() {
            if (started) b->EndAccess(bmdBufferAccessWrite);
            b->Release();
        }
    } guard{buf};
    if (buf->StartAccess(bmdBufferAccessWrite) != S_OK) return false;
    guard.started = true;

    void* dst = nullptr;
    if (buf->GetBytes(&dst) != S_OK || !dst) return false;
    memcpy(dst, src, comp_.packBytes(feed_));
    return true;
}

void DeckLinkOutput::schedule(IDeckLinkMutableVideoFrame* frame) {
    if (output_->ScheduleVideoFrame(frame, scheduledFrames_ * frameDuration_,
                                    frameDuration_, frameTimescale_) != S_OK) {
        Stats::counter("out.sdi.scheduleFailed").add();
        std::lock_guard lk(freeM_);
        free_.push_back(frame);
        return;
    }
    ++scheduledFrames_;
    sent_.fetch_add(1, std::memory_order_relaxed);
    Stats::counter(feed_ == gpu::Compositor::Feed::Clean ? "out.sdi.clean.sent"
                                                         : "out.sdi.sent")
        .add();
}

void DeckLinkOutput::run(std::stop_token st) {
    auto& skipCtr = Stats::counter("out.sdi.droppedToLatest");
    auto& starveCtr = Stats::counter("out.sdi.poolStarved");
    auto& deepCtr = Stats::counter("out.sdi.bufferDeep");

    while (!st.stop_requested() && !open()) {
        close();
        for (int i = 0; i < 5 && !st.stop_requested(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (st.stop_requested()) return;

    uint64_t lastSent = 0;

    while (!st.stop_requested()) {
        if (!readbackTL_.waitCompleted(lastSent + 1, 100'000'000)) continue;
        const uint64_t newest = readbackTL_.completed();
        if (newest <= lastSent) continue;
        if (newest > lastSent + 1) skipCtr.add(int64_t(newest - lastSent - 1));

        int slot = -1;
        for (int s = 0; s < gpu::Compositor::kPackSlots; ++s)
            if (comp_.packStamp(s, feed_).load(std::memory_order_acquire) ==
                newest) {
                slot = s;
                break;
            }
        lastSent = newest;
        if (slot < 0) continue;  // engine skipped packing that tick

        // Let the card's queue drain rather than run away: MediaClock and the
        // card's clock differ by a few ppm, and without this the buffer walks
        // one way over a long show.
        uint32_t buffered = 0;
        output_->GetBufferedVideoFrameCount(&buffered);
        if (playing_.load(std::memory_order_relaxed) &&
            buffered >= uint32_t(kTargetPreroll + 3)) {
            deepCtr.add();
            continue;
        }

        IDeckLinkMutableVideoFrame* frame = nullptr;
        {
            std::lock_guard lk(freeM_);
            if (!free_.empty()) {
                frame = free_.back();
                free_.pop_back();
            }
        }
        if (!frame) {  // every frame still in the card's hands
            starveCtr.add();
            continue;
        }

        // Pin before reading, then confirm the slot still holds our frame --
        // the render thread may have recycled it while we were looking.
        bool pinned = comp_.packTryPin(slot, feed_);
        if (pinned &&
            comp_.packStamp(slot, feed_).load(std::memory_order_acquire) !=
                newest) {
            comp_.packUnpin(slot, feed_);
            pinned = false;
        }
        if (!pinned) {
            std::lock_guard lk(freeM_);
            free_.push_back(frame);
            continue;
        }

        const bool copied = writeFrame(frame, comp_.packPtr(slot, feed_));
        comp_.packUnpin(slot, feed_);
        if (!copied) {
            Stats::counter("out.sdi.bufferAccessFailed").add();
            std::lock_guard lk(freeM_);
            free_.push_back(frame);
            continue;
        }

        schedule(frame);

        // Preroll before letting the card run, so playback starts with a full
        // pipeline instead of underflowing into black.
        if (!playing_.load(std::memory_order_relaxed) &&
            scheduledFrames_ >= kTargetPreroll) {
            if (audioPrerolling_.exchange(false)) output_->EndAudioPreroll();
            if (output_->StartScheduledPlayback(0, frameTimescale_, 1.0) ==
                S_OK) {
                playing_.store(true, std::memory_order_relaxed);
                KLOUD_LOGI("sdi out: playback started (preroll %lld frames)",
                         (long long)scheduledFrames_);
            }
        }
    }
}

}  // namespace kloud

#endif  // KLOUD_HAVE_DECKLINK
