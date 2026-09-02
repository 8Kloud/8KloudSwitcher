/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "web/MjpegEncoder.h"

#include <algorithm>

#include "core/Log.h"

namespace kloud::web {

bool MjpegEncoder::open(int srcW, int srcH, int dstW, int quality) {
    close();
    if (srcW < 2 || srcH < 2) return false;
    dstW = std::clamp(dstW, 160, srcW) & ~1;
    const int dstH = std::max(2, (dstW * srcH / srcW) & ~1);
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!codec) {
        KLOUD_LOGE("web: this FFmpeg has no mjpeg encoder; no browser multiview");
        return false;
    }
    enc_ = avcodec_alloc_context3(codec);
    if (!enc_) return false;
    enc_->width = dstW;
    enc_->height = dstH;
    // Full-range yuv420p (not the deprecated yuvj420p alias); the encoder
    // accepts it below the "unofficial" compliance bar.
    enc_->pix_fmt = AV_PIX_FMT_YUV420P;
    enc_->color_range = AVCOL_RANGE_JPEG;
    enc_->time_base = {1, 30};
    enc_->flags |= AV_CODEC_FLAG_QSCALE;
    enc_->global_quality = std::clamp(quality, 2, 31) * FF_QP2LAMBDA;
    enc_->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL;
    if (avcodec_open2(enc_, codec, nullptr) < 0) {
        KLOUD_LOGE("web: mjpeg encoder open failed");
        close();
        return false;
    }
    frame_ = av_frame_alloc();
    frame_->format = enc_->pix_fmt;
    frame_->width = dstW;
    frame_->height = dstH;
    frame_->color_range = AVCOL_RANGE_JPEG;
    if (av_frame_get_buffer(frame_, 32) < 0) {
        close();
        return false;
    }
    pkt_ = av_packet_alloc();
    sws_ = sws_getContext(srcW, srcH, AV_PIX_FMT_RGBA, dstW, dstH,
                          AV_PIX_FMT_YUV420P,
                          dstW == srcW ? SWS_POINT : SWS_AREA, nullptr,
                          nullptr, nullptr);
    if (!sws_) {
        close();
        return false;
    }
    // RGB in, full-range JPEG YCbCr (BT.601 matrix, as JPEG expects) out.
    const int* coefficients = sws_getCoefficients(SWS_CS_ITU601);
    sws_setColorspaceDetails(sws_, coefficients, 1, coefficients, 1, 0, 1 << 16,
                             1 << 16);
    srcW_ = srcW;
    srcH_ = srcH;
    dstW_ = dstW;
    dstH_ = dstH;
    return true;
}

void MjpegEncoder::close() {
    if (sws_) sws_freeContext(sws_);
    sws_ = nullptr;
    if (pkt_) av_packet_free(&pkt_);
    if (frame_) av_frame_free(&frame_);
    if (enc_) avcodec_free_context(&enc_);
}

bool MjpegEncoder::encode(const uint8_t* rgba, std::string& jpeg) {
    if (!enc_) return false;
    if (av_frame_make_writable(frame_) < 0) return false;
    const uint8_t* src[1] = {rgba};
    const int srcStride[1] = {srcW_ * 4};
    sws_scale(sws_, src, srcStride, 0, srcH_, frame_->data, frame_->linesize);
    frame_->pts = pts_++;
    if (avcodec_send_frame(enc_, frame_) < 0) return false;
    bool got = false;
    for (;;) {
        const int r = avcodec_receive_packet(enc_, pkt_);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
        if (r < 0) return false;
        jpeg.assign(reinterpret_cast<const char*>(pkt_->data),
                    size_t(pkt_->size));
        got = true;
        av_packet_unref(pkt_);
    }
    return got;
}

}  // namespace kloud::web
