#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cwchar>
#include <limits>
#include <vector>

#include "SmokePath.h"
#include "FrameScaler.h"
#include "VideoScaling.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
}

namespace
{
    bool ParseDimension(const wchar_t* text, std::uint32_t& value)
    {
        wchar_t* end = nullptr;
        const unsigned long parsed = std::wcstoul(text, &end, 10);
        if (!text[0] || !end || *end || parsed == 0 ||
            parsed > (std::numeric_limits<std::uint32_t>::max)()) {
            return false;
        }
        value = static_cast<std::uint32_t>(parsed);
        return true;
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2 && argc != 4) {
        ::fwprintf(stderr,
            L"usage: mmvp_decode_smoke <video> [output-width output-height]\n");
        return 2;
    }

    std::uint32_t outputWidth = 3840;
    std::uint32_t outputHeight = 2160;
    if (argc == 4 &&
        (!ParseDimension(argv[2], outputWidth) ||
            !ParseDimension(argv[3], outputHeight))) {
        ::fwprintf(stderr, L"invalid output dimensions\n");
        return 2;
    }

    const std::string path = WidePathToUtf8(argv[1]);
    if (path.empty()) {
        ::fwprintf(stderr, L"could not convert video path\n");
        return 3;
    }

    std::puts("stage=open");
    std::fflush(stdout);
    AVFormatContext* format = nullptr;
    int result = avformat_open_input(&format, path.c_str(), nullptr, nullptr);
    if (result < 0) {
        std::printf("open failed: %d\n", result);
        return 4;
    }

    int stream = -1;
    for (unsigned int index = 0; index < format->nb_streams; ++index) {
        if (format->streams[index]->codecpar->codec_type ==
            AVMEDIA_TYPE_VIDEO) {
            stream = static_cast<int>(index);
            break;
        }
    }
    if (stream < 0) {
        std::puts("no video stream");
        avformat_close_input(&format);
        return 5;
    }

    const AVCodecParameters* parameters = format->streams[stream]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
    if (!codec) {
        std::puts("no decoder");
        avformat_close_input(&format);
        return 6;
    }

    std::printf("stage=open-decoder codec=%s\n", codec->name);
    std::fflush(stdout);
    AVCodecContext* decoder = avcodec_alloc_context3(codec);
    if (decoder) {
        decoder->thread_count = 4;
    }
    if (!decoder ||
        avcodec_parameters_to_context(decoder, parameters) < 0 ||
        avcodec_open2(decoder, codec, nullptr) < 0) {
        std::puts("decoder open failed");
        avcodec_free_context(&decoder);
        avformat_close_input(&format);
        return 7;
    }

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool decoded = false;
    std::puts("stage=decode-first-frame");
    std::fflush(stdout);
    while (!decoded && av_read_frame(format, packet) >= 0) {
        if (packet->stream_index == stream &&
            avcodec_send_packet(decoder, packet) >= 0 &&
            avcodec_receive_frame(decoder, frame) == 0) {
            decoded = true;
        }
        av_packet_unref(packet);
    }

    if (!decoded) {
        std::puts("no frame decoded");
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&decoder);
        avformat_close_input(&format);
        return 8;
    }

    const int decodedWidth = frame->width;
    const int decodedHeight = frame->height;
    if (!VideoScaling::ApplyCenterCrop(frame, outputWidth, outputHeight)) {
        std::puts("center crop failed");
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&decoder);
        avformat_close_input(&format);
        return 9;
    }
    VideoScaling::EnsureColorMetadata(frame);

    const int bufferSize = av_image_get_buffer_size(AV_PIX_FMT_BGRA,
        static_cast<int>(outputWidth),
        static_cast<int>(outputHeight),
        1);
    if (bufferSize <= 0) {
        std::puts("scaler setup failed");
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&decoder);
        avformat_close_input(&format);
        return 10;
    }
    constexpr std::uint8_t unwrittenPixel = 0xA5;
    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(bufferSize), unwrittenPixel);
    AVFrame outputFrame{};
    outputFrame.format = AV_PIX_FMT_BGRA;
    outputFrame.width = static_cast<int>(outputWidth);
    outputFrame.height = static_cast<int>(outputHeight);
    outputFrame.color_primaries = frame->color_primaries;
    outputFrame.color_trc = frame->color_trc;
    outputFrame.colorspace = AVCOL_SPC_RGB;
    outputFrame.color_range = AVCOL_RANGE_JPEG;
    VideoScaling::EnsureColorMetadata(&outputFrame);
    VideoScaling::FrameScaler scaler;
    if (av_image_fill_arrays(outputFrame.data,
            outputFrame.linesize,
            pixels.data(),
            AV_PIX_FMT_BGRA,
            static_cast<int>(outputWidth),
            static_cast<int>(outputHeight),
            1) < 0 ||
        !scaler.Scale(frame, &outputFrame, ScalingAlgorithm::Spline36) ||
        scaler.ActiveAlgorithm() != ScalingAlgorithm::Spline36) {
        std::printf("Spline36 scale failed: %s\n",
                    scaler.LastError().c_str());
        scaler.Reset();
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&decoder);
        avformat_close_input(&format);
        return 11;
    }
    if (std::ranges::all_of(pixels,
            [](std::uint8_t value) { return value == unwrittenPixel; })) {
        std::puts("Spline36 left the destination buffer untouched");
        scaler.Reset();
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&decoder);
        avformat_close_input(&format);
        return 12;
    }

    std::ranges::fill(pixels, unwrittenPixel);
    scaler.Reset();
    if (!scaler.Scale(frame, &outputFrame, ScalingAlgorithm::Bicubic) ||
        scaler.ActiveAlgorithm() != ScalingAlgorithm::Bicubic) {
        std::printf("Bicubic scale failed: %s\n", scaler.LastError().c_str());
        scaler.Reset();
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&decoder);
        avformat_close_input(&format);
        return 13;
    }
    if (std::ranges::all_of(pixels,
            [](std::uint8_t value) { return value == unwrittenPixel; })) {
        std::puts("Bicubic left the destination buffer untouched");
        scaler.Reset();
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&decoder);
        avformat_close_input(&format);
        return 14;
    }
    scaler.Reset();
    std::printf(
        "ok codec=%s decoded=%dx%d target=%ux%u cropped=%dx%d "
        "pixel_format=%d\n",
        codec->name,
        decodedWidth,
        decodedHeight,
        outputWidth,
        outputHeight,
        frame->width,
        frame->height,
        frame->format);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&decoder);
    avformat_close_input(&format);
    return 0;
}
