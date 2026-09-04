/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// SRT/media ingest: the MPEG-TS the SRT output sends (HEVC, and AV1 in the
// draft carriage) opens through SrtInput and decodes on NVDEC. Guards the
// decoder choice: FFmpeg's default AV1 decoder in the LGPL build is libdav1d,
// which is software-only, ignores the CUDA device, and cannot parse the draft
// AV1-in-TS at all -- the program went black while audio kept flowing. The
// decode tests skip without a GPU/CUDA/NVENC.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "gpu/VkEngine.h"
#include "in/SrtInput.h"
#include "media/CudaCtx.h"
#include "media/NvencDirect.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}

using namespace kloud;

namespace {

constexpr int kW = 640, kH = 360, kFrames = 30;
const VideoFormatDesc kShow{kW, kH, 60000, 1001, PixFmt::NV12};

bool takesCudaDevice(const AVCodec* c) {
    for (int i = 0;; ++i) {
        const AVCodecHWConfig* hw = avcodec_get_hw_config(c, i);
        if (!hw) return false;
        if (hw->device_type == AV_HWDEVICE_TYPE_CUDA &&
            (hw->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
            return true;
    }
}

std::vector<uint8_t> nv12Frame(int n) {
    std::vector<uint8_t> f(size_t(kW) * kH * 3 / 2, 128);
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x)
            f[size_t(y) * kW + x] = uint8_t(16 + ((x + y) >> 2) % 200);
    const int bx = (n * 17) % (kW - 64), by = (n * 11) % (kH - 64);
    for (int y = by; y < by + 64; ++y)
        memset(&f[size_t(y) * kW + bx], 235, 64);
    return f;
}

struct CudaFixture {
    gpu::VkEngine eng;
    media::CudaCtx cuda;
    CUdeviceptr buf = 0;
    ~CudaFixture() {
        if (buf) cuMemFree(buf);
    }
    void upload(int n) {
        const std::vector<uint8_t> host = nv12Frame(n);
        REQUIRE(cuMemcpyHtoD(buf, host.data(), host.size()) == CUDA_SUCCESS);
    }
};

std::unique_ptr<CudaFixture> makeFixture() {
    auto fx = std::make_unique<CudaFixture>();
    if (!fx->eng.init(false)) return nullptr;
    if (!fx->cuda.init(fx->eng.deviceUuid())) return nullptr;
    fx->cuda.makeCurrent();
    if (cuMemAlloc(&fx->buf, size_t(kW) * kH * 3 / 2) != CUDA_SUCCESS)
        return nullptr;
    return fx;
}

// Muxes the packets the way SrtOutput::openMux does: mpegts, a 90 kHz
// stream with the encoder's extradata, the draft AV1 carriage for AV1.
// Returns false when this FFmpeg lacks the draft muxer option.
bool writeTs(const std::string& path, media::VideoCodec codec,
             const media::NvencDirect& enc,
             const std::vector<AVPacket*>& packets) {
    AVFormatContext* oc = nullptr;
    if (avformat_alloc_output_context2(&oc, nullptr, "mpegts", path.c_str()) < 0)
        return false;
    bool ok = false;
    AVStream* st = avformat_new_stream(oc, nullptr);
    if (st && enc.fillCodecpar(st->codecpar)) {
        st->time_base = {1, 90000};
        ok = codec != media::VideoCodec::Av1 ||
             av_opt_set_int(oc->priv_data, "av1_mpegts_draft", 1, 0) >= 0;
    }
    ok = ok && avio_open(&oc->pb, path.c_str(), AVIO_FLAG_WRITE) >= 0 &&
         avformat_write_header(oc, nullptr) >= 0;
    if (ok) {
        const AVRational encTb = enc.timeBase();
        for (const AVPacket* src : packets) {
            AVPacket* pkt = av_packet_clone(src);
            av_packet_rescale_ts(pkt, encTb, st->time_base);
            pkt->stream_index = st->index;
            ok = ok && av_write_frame(oc, pkt) >= 0;
            av_packet_free(&pkt);
        }
        ok = ok && av_write_trailer(oc) >= 0;
    }
    if (oc->pb) avio_closep(&oc->pb);
    avformat_free_context(oc);
    return ok;
}

void freeAll(std::vector<AVPacket*>& packets) {
    for (AVPacket* pkt : packets) av_packet_free(&pkt);
    packets.clear();
}

// Encodes kFrames with NVENC, muxes them to a TS, and plays that file back
// through SrtInput in media mode, checking every frame decodes on NVDEC.
void ingest(CudaFixture& fx, media::VideoCodec codec, const char* label) {
    media::NvencDirect enc;
    const media::EncoderConfig cfg{media::EncoderBackend::Direct,
                                   media::EncoderPreset::P4, 4000, true, codec};
    if (!enc.open(fx.cuda, kShow, cfg))
        SKIP("no NVENC session for " << label);

    std::vector<AVPacket*> packets;
    for (int n = 0; n < kFrames; ++n) {
        fx.upload(n);
        REQUIRE(enc.encode(fx.buf, n, packets));
    }
    REQUIRE(enc.drain(packets));
    REQUIRE(int(packets.size()) == kFrames);

    const std::string path =
        (std::filesystem::temp_directory_path() /
         ("kloud_srt_ingest_" + std::string(label) + ".ts")).string();
    const bool wrote = writeTs(path, codec, enc, packets);
    freeAll(packets);
    enc.close();
    if (!wrote && codec == media::VideoCodec::Av1)
        SKIP("FFmpeg lacks the draft AV1 MPEG-TS muxer option");
    REQUIRE(wrote);

    {
        // Media mode paces the file in real time: 30 frames at 59.94 is half
        // a second. Latest-frame mailbox, no loop, so the clip ends.
        SrtInput in(fx.eng, fx.eng.xferUp(), fx.cuda, path, 0, -1, true, true,
                    false);
        uint64_t lastSeq = 0;
        int published = 0;
        VideoFormatDesc seen{};
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            if (auto item = in.newer(lastSeq)) {
                lastSeq = item->seq;
                ++published;
                seen = item->value->desc;
            }
            if (in.status().frames >= kFrames) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        const auto st = in.status();
        INFO(label << ": decoded " << st.frames << " frames, published "
                   << published);
        CHECK(st.frames == kFrames);
        CHECK(st.desc.width == kW);
        CHECK(st.desc.height == kH);
        CHECK(published > 0);
        CHECK(seen.width == kW);
        CHECK(seen.height == kH);
    }
    std::remove(path.c_str());
}

}  // namespace

TEST_CASE("SrtInput picks a decoder that takes the CUDA device") {
    // HEVC: FFmpeg's default is already the native hwaccel front end.
    const AVCodec* hevc = SrtInput::pickVideoDecoder(AV_CODEC_ID_HEVC);
    REQUIRE(hevc);
    CHECK(takesCudaDevice(hevc));
    CHECK(std::string(hevc->name) == "hevc");

    // AV1: the default may be libdav1d (software-only). When a hwaccel
    // decoder exists it must win; only without one does the default stand.
    const AVCodec* av1 = SrtInput::pickVideoDecoder(AV_CODEC_ID_AV1);
    REQUIRE(av1);
    bool anyCuda = false;
    void* it = nullptr;
    while (const AVCodec* c = av_codec_iterate(&it))
        if (c->id == AV_CODEC_ID_AV1 && av_codec_is_decoder(c) &&
            takesCudaDevice(c))
            anyCuda = true;
    if (anyCuda) {
        CHECK(takesCudaDevice(av1));
        CHECK(std::string(av1->name) != "libdav1d");
    } else {
        CHECK(av1 == avcodec_find_decoder(AV_CODEC_ID_AV1));
    }

    // Anything without a CUDA path falls through to FFmpeg's default.
    CHECK(SrtInput::pickVideoDecoder(AV_CODEC_ID_AAC) ==
          avcodec_find_decoder(AV_CODEC_ID_AAC));
}

TEST_CASE("SRT ingest decodes NVENC HEVC and AV1 transport streams on NVDEC") {
    auto fx = makeFixture();
    if (!fx) SKIP("no Vulkan/CUDA device");

    SECTION("hevc") { ingest(*fx, media::VideoCodec::Hevc, "hevc"); }
    SECTION("av1") { ingest(*fx, media::VideoCodec::Av1, "av1"); }
}
