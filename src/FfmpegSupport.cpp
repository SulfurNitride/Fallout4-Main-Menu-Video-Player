#include "PCH.h"

#include "FfmpegSupport.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace FfmpegSupport
{
    std::string Utf8Path(const std::filesystem::path& path)
    {
        const auto utf8 = path.u8string();
        return { reinterpret_cast<const char*>(utf8.data()), utf8.size() };
    }

    std::string AvError(const int code)
    {
        std::array<char, AV_ERROR_MAX_STRING_SIZE> message{};
        av_strerror(code, message.data(), message.size());
        return message.data();
    }

    int FindVideoStream(const AVFormatContext* format)
    {
        for (unsigned int index = 0; index < format->nb_streams; ++index) {
            const AVStream* stream = format->streams[index];
            if (stream && stream->codecpar &&
                stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                return static_cast<int>(index);
            }
        }
        return -1;
    }

    const AVCodec* FindDecoder(const AVCodecParameters* parameters)
    {
        // FFmpeg 4.4's native AV1 decoder fails on some 10-bit streams under
        // Wine. Use the external libaom decoder for AV1 instead.
        if (parameters->codec_id == AV_CODEC_ID_AV1) {
            if (const AVCodec* aom =
                    avcodec_find_decoder_by_name("libaom-av1")) {
                return aom;
            }
        }
        return avcodec_find_decoder(parameters->codec_id);
    }
} // namespace FfmpegSupport
