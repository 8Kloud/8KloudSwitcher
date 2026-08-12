/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once
#include <cstdint>
#include <vector>

#include "core/Format.h"
#include "media/CudaCtx.h"
#include "media/IVideoEncoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}

namespace kloud::media {

// hevc_nvenc or av1_nvenc via CUDA hwframes built on OUR primary context. Low-latency
// tuned: EncoderConfig::preset + tune ull, CBR with single-frame VBV, no
// B-frames and in-band codec headers. AV1 also exports sequence-header
// extradata for its MPEG-TS descriptor; file recording requests GLOBAL_HEADER
// for Matroska codec configuration. PTS is the media tick index
// in show timebase (fpsD/fpsN).
class FfmpegNvenc final : public IVideoEncoder {
public:
    ~FfmpegNvenc() override { close(); }

    bool open(CudaCtx& cuda, const VideoFormatDesc& show,
              const EncoderConfig& cfg) override;
    void close() override;
    bool ok() const override { return enc_ != nullptr; }

    AVRational timeBase() const override {
        return enc_ ? enc_->time_base : AVRational{1, 1};
    }
    bool fillCodecpar(AVCodecParameters* par) const override;

    // Copies tight-pitch NV12 planes from `src` (device ptr) into a pool
    // frame (synchronized before return -> src is reusable) and encodes.
    // Emitted packets are appended to `out` (caller frees).
    bool encode(CUdeviceptr src, int64_t pts,
                std::vector<AVPacket*>& out) override;
    bool drain(std::vector<AVPacket*>& out) override;

private:
    bool receiveAll(std::vector<AVPacket*>& out);

    CudaCtx* cuda_ = nullptr;
    AVBufferRef* hwDev_ = nullptr;
    AVBufferRef* hwFrames_ = nullptr;
    AVCodecContext* enc_ = nullptr;
    AVFrame* frame_ = nullptr;
    int w_ = 0, h_ = 0;
};

}  // namespace kloud::media
