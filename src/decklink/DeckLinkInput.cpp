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

#include "decklink/DeckLinkInput.h"

#include <cstring>

#include "DeckLinkAPI.h"
#include "audio/AudioEngine.h"
#include "core/Log.h"
#include "core/MediaClock.h"
#include "core/Stats.h"

namespace kloud {
namespace {

// The SDK hands out `const char*` that the caller frees.
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

// Seed mode for format detection: anything valid works, the card corrects us
// on the first VideoInputFormatChanged.
constexpr BMDDisplayMode kSeedMode = bmdModeHD1080p5994;

}  // namespace

// COM shim: the SDK owns a reference to this, not to DeckLinkInput (whose
// lifetime is the engine's unique_ptr). Every callback is forwarded to the
// owner, which outlives the delegate -- close() clears the callback and
// releases the SDK's reference before the owner goes away.
class DeckLinkInput::Delegate : public IDeckLinkInputCallback {
public:
    explicit Delegate(DeckLinkInput* owner) : owner_(owner) {}

    HRESULT QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG AddRef() override {
        return ++refs_;
    }
    ULONG Release() override {
        const ULONG r = --refs_;
        if (r == 0) delete this;
        return r;
    }

    HRESULT VideoInputFormatChanged(BMDVideoInputFormatChangedEvents events,
                                    IDeckLinkDisplayMode* mode,
                                    BMDDetectedVideoInputFormatFlags flags) override {
        owner_->onFormatChanged(unsigned(events), mode, unsigned(flags));
        return S_OK;
    }

    HRESULT VideoInputFrameArrived(IDeckLinkVideoInputFrame* video,
                                   IDeckLinkAudioInputPacket* audio) override {
        owner_->onFrame(video, audio);
        return S_OK;
    }

private:
    DeckLinkInput* owner_;
    std::atomic<ULONG> refs_{1};
};

DeckLinkInput::DeckLinkInput(gpu::VkEngine& eng, gpu::Queue& uploadQueue,
                             std::string ref, int index, int syncFrames)
    : eng_(eng),
      queue_(uploadQueue),
      ref_(std::move(ref)),
      parsed_(parseDeckLinkRef(ref_)),
      index_(index),
      syncFrames_(syncFrames) {
    thread_ = std::jthread([this](std::stop_token st) { run(st); });
}

DeckLinkInput::~DeckLinkInput() {
    thread_ = {};  // stop + join
    close();
}

DeckLinkInput::Status DeckLinkInput::status() const {
    Status s;
    s.connected = connected_.load(std::memory_order_relaxed);
    s.frames = frames_.load(std::memory_order_relaxed);
    s.drops = drops_.load(std::memory_order_relaxed);
    std::lock_guard lk(descM_);
    s.desc = desc_;
    return s;
}

std::vector<std::string> DeckLinkInput::devices() {
    std::vector<std::string> out;
    IDeckLinkIterator* it = CreateDeckLinkIteratorInstance();
    if (!it) return out;  // no driver / no card
    IDeckLink* dl = nullptr;
    while (it->Next(&dl) == S_OK) {
        out.push_back(displayName(dl));
        dl->Release();
    }
    it->Release();
    return out;
}

bool DeckLinkInput::open() {
    if (!parsed_.valid) {
        KLOUD_LOGE("in%d(decklink): bad ref '%s' (expected decklink://INDEX "
                 "or decklink://NAME)",
                 index_, ref_.c_str());
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
            if (!everLoggedOpen_) {
                everLoggedOpen_ = true;
                KLOUD_LOGI("in%d(decklink): opening '%s'", index_, name.c_str());
            }
            break;
        }
        dl->Release();
        ++i;
    }
    it->Release();
    if (!device_) return false;

    if (device_->QueryInterface(IID_IDeckLinkInput, (void**)&input_) != S_OK ||
        !input_) {
        // Sub-devices of a card in a one-sub-device profile have no capture
        // interface at all -- this is the usual cause and worth naming.
        KLOUD_LOGE("in%d(decklink): device has no input interface (wrong "
                 "profile? see docs/decklink.md)", index_);
        close();
        return false;
    }

    delegate_ = new Delegate(this);
    input_->SetCallback(delegate_);

    // Explicit mode: resolve the name against the device's own list.
    IDeckLinkDisplayMode* forced = nullptr;
    if (!parsed_.mode.empty()) {
        IDeckLinkDisplayModeIterator* mi = nullptr;
        if (input_->GetDisplayModeIterator(&mi) == S_OK) {
            IDeckLinkDisplayMode* m = nullptr;
            while (mi->Next(&m) == S_OK) {
                const char* mn = nullptr;
                m->GetName(&mn);
                if (takeString(mn) == parsed_.mode) {
                    forced = m;  // keep
                    break;
                }
                m->Release();
            }
            mi->Release();
        }
        if (!forced)
            KLOUD_LOGW("in%d(decklink): mode '%s' not offered by this device, "
                     "falling back to auto-detect",
                     index_, parsed_.mode.c_str());
    }

    startStreams(forced, forced != nullptr);
    if (forced) forced->Release();
    return streaming_.load(std::memory_order_relaxed);
}

void DeckLinkInput::startStreams(void* displayMode, bool applyMode) {
    auto* mode = static_cast<IDeckLinkDisplayMode*>(displayMode);

    // Format detection is what makes SDI plug-and-play; without it we can only
    // capture the mode we were told to expect.
    bool detect = false;
    IDeckLinkProfileAttributes* at = nullptr;
    if (device_->QueryInterface(IID_IDeckLinkProfileAttributes, (void**)&at) ==
        S_OK) {
        at->GetFlag(BMDDeckLinkSupportsInputFormatDetection, &detect);
        at->Release();
    }

    const BMDDisplayMode dm =
        applyMode && mode ? mode->GetDisplayMode() : kSeedMode;
    const BMDVideoInputFlags flags =
        detect ? bmdVideoInputEnableFormatDetection : bmdVideoInputFlagDefault;

    if (const HRESULT hr = input_->EnableVideoInput(dm, bmdFormat8BitYUV, flags);
        hr != S_OK) {
        // Throttled: the open loop retries every second and this is the
        // failure an operator actually has to act on, so say what to check.
        const int64_t now = MediaClock::nowNs();
        if (now - lastEnableLogNs_ > 10'000'000'000LL) {
            lastEnableLogNs_ = now;
            KLOUD_LOGE("in%d(decklink): EnableVideoInput failed (0x%08x) -- the "
                     "sub-device is most likely in use by another process, or "
                     "is doing playback in a half-duplex profile. See "
                     "docs/decklink.md",
                     index_, unsigned(hr));
        }
        streaming_.store(false, std::memory_order_relaxed);
        return;
    }
    // 2 channels is what the mixer lane takes; the card can do up to 64.
    if (input_->EnableAudioInput(bmdAudioSampleRate48kHz,
                                 bmdAudioSampleType32bitInteger, 2) != S_OK)
        KLOUD_LOGW("in%d(decklink): audio input unavailable, video only",
                 index_);

    if (mode) {
        BMDTimeValue dur = 0;
        BMDTimeScale scale = 0;
        if (mode->GetFrameRate(&dur, &scale) == S_OK && dur > 0) {
            fpsN_ = int64_t(scale);
            fpsD_ = int64_t(dur);
        }
    }

    if (input_->StartStreams() != S_OK) {
        KLOUD_LOGE("in%d(decklink): StartStreams failed", index_);
        streaming_.store(false, std::memory_order_relaxed);
        return;
    }
    streaming_.store(true, std::memory_order_relaxed);
    lastFrameNs_.store(MediaClock::nowNs(), std::memory_order_relaxed);
}

void DeckLinkInput::close() {
    if (input_) {
        input_->StopStreams();
        input_->DisableVideoInput();
        input_->DisableAudioInput();
        input_->SetCallback(nullptr);  // releases the SDK's delegate ref
        input_->Release();
        input_ = nullptr;
    }
    if (delegate_) {
        delegate_->Release();  // ours
        delegate_ = nullptr;
    }
    if (device_) {
        device_->Release();
        device_ = nullptr;
    }
    streaming_.store(false, std::memory_order_relaxed);
    connected_.store(false, std::memory_order_relaxed);
}

// Card told us the incoming signal changed. Restart the streams on the new
// mode; this is also how the very first real format arrives when the seed mode
// was wrong (the common case).
void DeckLinkInput::onFormatChanged(unsigned events, void* newMode,
                                    unsigned flags) {
    (void)flags;
    if (!(events & bmdVideoInputDisplayModeChanged) || !input_) return;
    auto* mode = static_cast<IDeckLinkDisplayMode*>(newMode);

    input_->StopStreams();
    input_->FlushStreams();
    if (mode) {
        const char* mn = nullptr;
        mode->GetName(&mn);
        KLOUD_LOGI("in%d(decklink): input format -> %s", index_,
                 takeString(mn).c_str());
    }
    startStreams(mode, true);
}

void DeckLinkInput::onFrame(IDeckLinkVideoInputFrame* video,
                            IDeckLinkAudioInputPacket* audio) {
    static thread_local bool ctrsReady = false;
    auto& dropCtr = Stats::counter("in" + std::to_string(index_) + ".drops");
    auto& frameCtr = Stats::counter("in" + std::to_string(index_) + ".frames");
    auto& noSigCtr =
        Stats::counter("in" + std::to_string(index_) + ".noSignal");
    auto& feedDropCtr =
        Stats::counter("in" + std::to_string(index_) + ".sync.feedDrops");
    (void)ctrsReady;

    if (audio) {
        if (auto* ch = audioSink_.load(std::memory_order_acquire)) {
            const long n = audio->GetSampleFrameCount();
            void* bytes = nullptr;
            if (n > 0 && audio->GetBytes(&bytes) == S_OK && bytes) {
                const auto* src = static_cast<const int32_t*>(bytes);
                audioL_.resize(size_t(n));
                audioR_.resize(size_t(n));
                constexpr float kScale = 1.0f / 2147483648.0f;
                for (long i = 0; i < n; ++i) {
                    audioL_[size_t(i)] = float(src[i * 2]) * kScale;
                    audioR_[size_t(i)] = float(src[i * 2 + 1]) * kScale;
                }
                BMDTimeValue pt = 0;
                const int64_t pts =
                    audio->GetPacketTime(&pt, 1'000'000'000LL) == S_OK
                        ? int64_t(pt)
                        : audio::InputChannel::kNoPts;
                ch->pushPlanar(audioL_.data(), audioR_.data(), int(n), 48000,
                               pts);
            }
        }
    }

    if (!video) return;
    lastFrameNs_.store(MediaClock::nowNs(), std::memory_order_relaxed);

    // No cable / no lock: frames keep arriving on cadence, flagged. Treat as
    // disconnected so the render loop shows the no-signal placeholder.
    if (video->GetFlags() & bmdFrameHasNoInputSource) {
        if (connected_.exchange(false, std::memory_order_relaxed)) {
            KLOUD_LOGW("in%d(decklink): no input signal", index_);
        }
        noSigCtr.add();
        return;
    }
    if (!connected_.exchange(true, std::memory_order_relaxed))
        KLOUD_LOGI("in%d(decklink): signal locked", index_);

    // SDK 16 moved pixel access behind IDeckLinkVideoBuffer: the buffer must
    // be checked out with StartAccess before GetBytes is valid.
    IDeckLinkVideoBuffer* buf = nullptr;
    if (video->QueryInterface(IID_IDeckLinkVideoBuffer, (void**)&buf) != S_OK ||
        !buf)
        return;
    struct BufGuard {
        IDeckLinkVideoBuffer* b;
        bool started = false;
        ~BufGuard() {
            if (started) b->EndAccess(bmdBufferAccessRead);
            b->Release();
        }
    } guard{buf};
    if (buf->StartAccess(bmdBufferAccessRead) != S_OK) return;
    guard.started = true;

    void* bytes = nullptr;
    if (buf->GetBytes(&bytes) != S_OK || !bytes) return;

    VideoFormatDesc d;
    d.width = int(video->GetWidth());
    d.height = int(video->GetHeight());
    d.fpsN = fpsN_;
    d.fpsD = fpsD_;
    d.pixfmt = PixFmt::UYVY8_422;  // bmdFormat8BitYUV *is* UYVY
    d.colorimetry = VideoFormatDesc::colorimetryForHeight(d.height);
    if (!d.valid()) return;

    if (!ring_ || !(ring_->desc() == d)) {
        KLOUD_LOGI("in%d(decklink): format %dx%d @ %ld/%ld", index_, d.width,
                 d.height, long(d.fpsN), long(d.fpsD));
        const int slots =
            gpu::UploadRing::kSlots + (syncFrames_ >= 0 ? syncFrames_ + 2 : 0);
        ring_ = std::make_shared<gpu::UploadRing>(eng_, d, queue_, slots);
        std::lock_guard lk(descM_);
        desc_ = d;
    }

    const int slot = ring_->acquire();
    if (slot < 0) {
        drops_.fetch_add(1, std::memory_order_relaxed);
        dropCtr.add();
        return;
    }

    const size_t dstStride = d.rowBytes();
    const size_t srcStride = size_t(video->GetRowBytes());
    uint8_t* dst = ring_->stagingPtr(slot);
    const auto* src = static_cast<const uint8_t*>(bytes);
    if (srcStride == dstStride) {
        memcpy(dst, src, dstStride * size_t(d.height));
    } else {
        const size_t n = srcStride < dstStride ? srcStride : dstStride;
        for (int y = 0; y < d.height; ++y)
            memcpy(dst + size_t(y) * dstStride, src + size_t(y) * srcStride, n);
    }

    const uint64_t value = ring_->submit(slot);
    auto frame = std::make_shared<const gpu::GpuFrame>(ring_, slot, value, false);
    mailbox_.publish(frame);

    if (syncFrames_ >= 0) {
        // The card's own reference clock -- a real sender clock, so unlike the
        // network inputs there is no synthesized-pts fallback here.
        BMDTimeValue frameTime = 0, frameDur = 0;
        const bool haveHw = video->GetHardwareReferenceTimestamp(
                                1'000'000'000LL, &frameTime, &frameDur) == S_OK;
        const int64_t arrNs = lastFrameNs_.load(std::memory_order_relaxed);
        const int64_t ptsNs = haveHw ? int64_t(frameTime) : arrNs;
        if (!feed_.push({frame, ++pubSeq_, ptsNs, arrNs, haveHw}))
            feedDropCtr.add();
    }

    frames_.fetch_add(1, std::memory_order_relaxed);
    frameCtr.add();
}

// Open/reopen loop: the card may be missing, held by another process, or in a
// profile without capture. Keep retrying so a later plug-in or profile switch
// is picked up without restarting the switcher.
void DeckLinkInput::run(std::stop_token st) {
    auto& reconnCtr =
        Stats::counter("in" + std::to_string(index_) + ".reconnects");
    bool everOpened = false;

    while (!st.stop_requested()) {
        if (!streaming_.load(std::memory_order_relaxed)) {
            close();
            if (open()) {
                if (everOpened) reconnCtr.add();
                everOpened = true;
            } else {
                for (int i = 0; i < 10 && !st.stop_requested(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
        }
        // Streams can stall without an error (card reset, profile change by
        // another app). Frames stop arriving entirely -- reopen after 3 s.
        const int64_t quiet =
            MediaClock::nowNs() - lastFrameNs_.load(std::memory_order_relaxed);
        if (quiet > 3'000'000'000LL) {
            KLOUD_LOGW("in%d(decklink): no frames for 3 s, reopening", index_);
            streaming_.store(false, std::memory_order_relaxed);
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    close();
}

}  // namespace kloud
