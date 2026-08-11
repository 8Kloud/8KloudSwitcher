/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once
#include <array>
#include <atomic>
#include <vector>

#include "core/Format.h"
#include "engine/SwitcherCore.h"
#include "gpu/UploadRing.h"
#include "gpu/VkEngine.h"

namespace kloud::gpu {

// Compute pipelines + per-frame-in-flight targets:
//   composite.comp:      inputs (UYVY) -> program RGBA16F (mix/wipes/FTB);
//                        dispatched again at proxy res for the look-ahead
//                        preview monitor (preview bus + post-transition DSKs)
//   pack_uyvy.comp:      program -> UYVY words (device buffer, for OMT out)
//   proxy_down.comp:     inputs + program -> <=960x544 RGBA8 proxies
//   multiview_tile.comp: proxies/labels/solid borders -> multiview RGBA8
// plus multiview -> host readback and the pack -> host ring shared by the
// network and SDI senders (see the pack-slot ownership rules below).
class Compositor {
public:
    static constexpr int kFramesInFlight = 2;
    static constexpr int kReadbackSlots = 3;   // multiview (GUI)
    // Program UYVY. Two senders (OMT + SDI) can sit a frame apart and pin
    // different slots, so budget: 2 pinned, one in DMA flight, one writable,
    // plus one of margin.
    static constexpr int kPackSlots = 5;
    static constexpr uint32_t kPackWriter = 1u << 31;  // exclusive writer bit
    static constexpr int kFeedCount = 3;
    // NV12 packing (SRT out, recorders) only ever consumes the two show-format
    // feeds; the multiview feed carries UYVY only.
    static constexpr int kNvFeedCount = 2;
    static constexpr int kProxyW = 960, kProxyH = 544;
    static constexpr int kLabelRowH = 24;

    // Program and Clean are show-format; Multiview is the mvW x mvH monitor
    // wall the GUI shows, packed for an OMT sender. Per-feed geometry lives in
    // feedFormat() -- nothing may assume a feed is show-sized.
    enum class Feed : int { Program = 0, Clean = 1, Multiview = 2 };

    Compositor(VkEngine& eng, const VideoFormatDesc& show, int mvW, int mvH,
               int numInputs);
    ~Compositor();

    struct SourceRef {
        const GpuFrame* frame = nullptr;
    };
    struct TickJob {
        const GpuFrame* a = nullptr;   // program bus source (full res)
        const GpuFrame* b = nullptr;   // preview bus source (full res)
        CompositeJob sw;
        std::vector<SourceRef> mvInputs;  // per input, frame or placeholder
        int tallyPgmA = -1, tallyPgmB = -1, tallyPvw = -1;
        // Keyer fill frames (null = keyer dark; levels/flags ride in sw)
        // and their input indices for the red multiview border (-1 = none).
        const GpuFrame* dsk[kDskCount] = {nullptr, nullptr};
        int tallyDsk[kDskCount] = {-1, -1};
        // Look-ahead preview monitor: keyer levels for the proxy-res
        // preview composite (post-next-transition state; 0 = keyer absent).
        float pvwDskLevel[kDskCount] = {0.f, 0.f};
        bool packProgram = false;         // record UYVY pack (OMT out enabled)
        bool packClean = false;           // UYVY clean-feed OMT output
        bool packMultiview = false;       // UYVY multiview OMT output
        bool packNv12 = false;            // record NV12 pack (SRT out enabled)
        bool packCleanNv12 = false;       // clean-feed recorder
    };

    void record(VkCommandBuffer cmd, const TickJob& job, int fif, int rbSlot);
    // Copy pack device buffer (fif) into host pack slot; runs on xferDown.
    void recordDownCopy(VkCommandBuffer cmd, int fif, int packSlot,
                        Feed feed = Feed::Program);

    // Multiview readback (GUI).
    const uint8_t* readbackPtr(int rbSlot) const {
        return static_cast<const uint8_t*>(readback_[rbSlot].mapped);
    }
    size_t readbackBytes() const { return size_t(mvW_) * mvH_ * 4; }
    int mvWidth() const { return mvW_; }
    int mvHeight() const { return mvH_; }

    // Pack host ring (OMT sender).
    const uint8_t* packPtr(int slot, Feed feed = Feed::Program) const {
        return static_cast<const uint8_t*>(
            packHost_[int(feed)][slot].mapped);
    }
    size_t packBytes(Feed feed = Feed::Program) const {
        return feedFormat(feed).frameBytes();
    }
    std::atomic<uint64_t>& packStamp(int slot, Feed feed = Feed::Program) {
        return packStamp_[int(feed)][slot];
    }

    // Pack-slot ownership. A slot can be held by the render thread (writer,
    // exclusive) or by any number of senders (readers), never both -- the
    // OMT and SDI outputs both consume a feed, and each grabs the newest slot
    // for only as long as it takes to hand the bytes on.
    //
    // Render thread: take the slot BEFORE stamping it, so a sender cannot pin
    // it during the window between selection and the stamp store and then
    // read a buffer that is about to be DMA'd into.
    bool packTryAcquireWrite(int slot, Feed feed) {
        uint32_t expected = 0;
        return packPins_[int(feed)][slot].compare_exchange_strong(
            expected, kPackWriter, std::memory_order_acquire,
            std::memory_order_relaxed);
    }
    void packReleaseWrite(int slot, Feed feed) {
        packPins_[int(feed)][slot].fetch_and(~kPackWriter,
                                             std::memory_order_release);
    }
    // Sender: pin for reading unless the writer owns it. Re-check packStamp
    // after this succeeds -- the slot may have been recycled first.
    bool packTryPin(int slot, Feed feed) {
        auto& pins = packPins_[int(feed)][slot];
        uint32_t p = pins.load(std::memory_order_relaxed);
        do {
            if (p & kPackWriter) return false;
        } while (!pins.compare_exchange_weak(p, p + 1,
                                             std::memory_order_acquire,
                                             std::memory_order_relaxed));
        return true;
    }
    void packUnpin(int slot, Feed feed) {
        packPins_[int(feed)][slot].fetch_sub(1, std::memory_order_release);
    }

    // NV12 pack buffers for SRT/recorders (exportable; importer owns fds).
    int nvPackExportFd(int fif, Feed feed = Feed::Program) {
        // Only the show-format feeds have NV12 buffers (kNvFeedCount); asking
        // for the multiview's would read off the end of the array.
        if (int(feed) >= kNvFeedCount) return -1;
        return eng_.exportMemoryFd(packNvDev_[int(feed)][fif]);
    }
    size_t nvPackBytes() const { return size_t(show_.width) * show_.height * 3 / 2; }

    // Label atlas: rows 0=PROGRAM, 1=PREVIEW, 2+i=input i; usedWidths in pixels.
    void setLabelAtlas(Image atlas, std::vector<int> usedWidths);

    const VideoFormatDesc& showFormat() const { return show_; }
    // Geometry of a pack feed: show format for Program/Clean, the multiview
    // wall for Multiview. Senders take width/height/stride from here rather
    // than from showFormat().
    const VideoFormatDesc& feedFormat(Feed feed) const {
        return feed == Feed::Multiview ? mvFormat_ : show_;
    }

private:
    struct TilePC;
    struct CompositePC;
    struct PackPC;
    struct ProxyPC;

    struct Pipe {
        VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipe = VK_NULL_HANDLE;
    };
    Pipe makePipe(const uint8_t* spv, size_t size,
                  std::initializer_list<VkDescriptorType> bindings, uint32_t pcSize);
    void destroyPipe(Pipe& p);

    void barrier(VkCommandBuffer cmd, VkImage img, VkPipelineStageFlags2 srcStage,
                 VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                 VkAccessFlags2 dstAccess, VkImageLayout oldLayout,
                 VkImageLayout newLayout);
    void memBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage,
                    VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                    VkAccessFlags2 dstAccess);
    void initImageOnce(VkCommandBuffer cmd, Image& img, uint8_t& flag);

    void dispatchProxy(VkCommandBuffer cmd, VkImageView src, VkImageView srcUv,
                       int mode, const VideoFormatDesc* srcDesc, Image& dst,
                       int usedW, int usedH);
    void dispatchTile(VkCommandBuffer cmd, VkImageView src, VkImageView srcUv,
                      int mode, const float srcMap[4],
                      const VideoFormatDesc* srcDesc, int dstX, int dstY,
                      int dstW, int dstH, int fif);
    void tileFromProxy(VkCommandBuffer cmd, const Image& proxy, int usedW,
                       int usedH, int dstX, int dstY, int dstW, int dstH, int fif);
    void labelTile(VkCommandBuffer cmd, int row, int dstX, int dstY, int dstW,
                   int fif);
    void borderTiles(VkCommandBuffer cmd, int x, int y, int w, int h,
                     const float rgb[3], int fif);

    // Per-input proxy used extent for a source format.
    static void proxyUsed(const VideoFormatDesc& d, int& w, int& h);

    VkEngine& eng_;
    VideoFormatDesc show_;
    int mvW_, mvH_, numInputs_;
    VideoFormatDesc mvFormat_;  // mvW_ x mvH_ UYVY, show cadence

    Image program_[kFramesInFlight];
    Image clean_[kFramesInFlight];
    Image multiview_[kFramesInFlight];
    Image programProxy_[kFramesInFlight];
    // Look-ahead preview monitor, composited directly at proxy resolution
    // (a full-res preview pass would double composite bandwidth -- ~32 GB/s
    // at 8K; the multiview tile is the only consumer).
    Image previewMon_[kFramesInFlight];
    std::vector<Image> inputProxy_[kFramesInFlight];  // [fif][input]
    std::vector<uint8_t> proxyInit_[kFramesInFlight];
    bool targetsInit_[kFramesInFlight] = {};

    Buffer readback_[kReadbackSlots];
    Buffer packDev_[kFeedCount][kFramesInFlight];
    Buffer packNvDev_[kNvFeedCount][kFramesInFlight];
    Buffer packHost_[kFeedCount][kPackSlots];
    std::atomic<uint64_t> packStamp_[kFeedCount][kPackSlots]{};
    std::atomic<uint32_t> packPins_[kFeedCount][kPackSlots]{};

    Image labelAtlas_{};
    std::vector<int> labelUsedW_;

    Pipe composite_, tile_, pack_, packNv_, proxy_;
};

}  // namespace kloud::gpu
