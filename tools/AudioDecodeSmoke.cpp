#include <cstdio>

#include "SmokePath.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) {
        ::fwprintf(stderr, L"usage: mmvp_audio_smoke <audio>\n");
        return 2;
    }

    const std::string path = WidePathToUtf8(argv[1]);
    if (path.empty()) {
        ::fwprintf(stderr, L"could not convert audio path\n");
        return 3;
    }

    AVFormatContext* format = nullptr;
    if (avformat_open_input(&format, path.c_str(), nullptr, nullptr) < 0) {
        std::puts("audio open failed");
        return 4;
    }

    int stream = -1;
    for (unsigned int index = 0; index < format->nb_streams; ++index) {
        if (format->streams[index]->codecpar->codec_type ==
            AVMEDIA_TYPE_AUDIO) {
            stream = static_cast<int>(index);
            break;
        }
    }
    if (stream < 0) {
        std::puts("no audio stream");
        avformat_close_input(&format);
        return 5;
    }

    const AVCodecParameters* parameters = format->streams[stream]->codecpar;
    AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
    AVCodecContext* decoder = codec ?
        avcodec_alloc_context3(codec) :
        nullptr;
    if (!codec ||
        !decoder ||
        avcodec_parameters_to_context(decoder, parameters) < 0 ||
        avcodec_open2(decoder, codec, nullptr) < 0) {
        std::puts("audio decoder open failed");
        avcodec_free_context(&decoder);
        avformat_close_input(&format);
        return 6;
    }

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool decoded = false;
    while (!decoded && av_read_frame(format, packet) >= 0) {
        if (packet->stream_index == stream &&
            avcodec_send_packet(decoder, packet) >= 0 &&
            avcodec_receive_frame(decoder, frame) == 0) {
            decoded = true;
        }
        av_packet_unref(packet);
    }

    if (!decoded) {
        std::puts("no audio frame decoded");
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&decoder);
        avformat_close_input(&format);
        return 7;
    }

    std::printf(
        "ok codec=%s sample_rate=%d channels=%d samples=%d\n",
        codec->name,
        decoder->sample_rate,
        decoder->channels,
        frame->nb_samples);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&decoder);
    avformat_close_input(&format);
    return 0;
}
