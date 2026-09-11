#include <cstdio>
#include <cstdint>
#include <cwchar>
#include <limits>
#include <vector>

#include "SmokePath.h"
#include "VideoScaling.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
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
    const AVCodec* codec = parameters->codec_id == AV_CODEC_ID_AV1 ?
        avcodec_find_decoder_by_name("libaom-av1") :
        avcodec_find_decoder(parameters->codec_id);
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

    SwsContext* scaler = sws_getContext(frame->width,
        frame->height,
        static_cast<AVPixelFormat>(frame->format),
        static_cast<int>(outputWidth),
        static_cast<int>(outputHeight),
        AV_PIX_FMT_BGRA,
        SWS_BILINEAR,
        nullptr,
        nullptr,
        nullptr);
    const int bufferSize = av_image_get_buffer_size(AV_PIX_FMT_BGRA,
        static_cast<int>(outputWidth),
        static_cast<int>(outputHeight),
        1);
    if (!scaler || bufferSize <= 0) {
        std::puts("scaler setup failed");
        sws_freeContext(scaler);
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&decoder);
        avformat_close_input(&format);
        return 10;
    }
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(bufferSize));
    std::uint8_t* outputPlanes[4]{};
    int outputStrides[4]{};
    if (av_image_fill_arrays(outputPlanes,
            outputStrides,
            pixels.data(),
            AV_PIX_FMT_BGRA,
            static_cast<int>(outputWidth),
            static_cast<int>(outputHeight),
            1) < 0 ||
        sws_scale(scaler,
            frame->data,
            frame->linesize,
            0,
            frame->height,
            outputPlanes,
            outputStrides) != static_cast<int>(outputHeight)) {
        std::puts("scale failed");
        sws_freeContext(scaler);
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&decoder);
        avformat_close_input(&format);
        return 11;
    }
    sws_freeContext(scaler);
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
