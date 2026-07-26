#include <cstdio>
#include <cstdlib>
#include <cwchar>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) {
        ::fwprintf(stderr, L"usage: mmvp_decode_smoke <video>\n");
        return 2;
    }

    char path[32768]{};
    if (::wcstombs(path, argv[1], sizeof(path) - 1) == static_cast<std::size_t>(-1)) {
        ::fwprintf(stderr, L"could not convert video path\n");
        return 3;
    }

    std::puts("stage=open");
    std::fflush(stdout);
    AVFormatContext* format = nullptr;
    int result = avformat_open_input(&format, path, nullptr, nullptr);
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
    AVCodec* codec = parameters->codec_id == AV_CODEC_ID_AV1 ?
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
    std::printf(
        "ok codec=%s width=%d height=%d pixel_format=%d\n",
        codec->name,
        frame->width,
        frame->height,
        frame->format);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&decoder);
    avformat_close_input(&format);
    return 0;
}
