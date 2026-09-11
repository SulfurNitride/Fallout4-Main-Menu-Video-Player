#pragma once

struct AVCodec;
struct AVCodecParameters;
struct AVFormatContext;

namespace FfmpegSupport
{
    [[nodiscard]] std::string Utf8Path(const std::filesystem::path& path);
    [[nodiscard]] std::string AvError(int code);
    [[nodiscard]] int FindVideoStream(const AVFormatContext* format);
    [[nodiscard]] const AVCodec* FindDecoder(
        const AVCodecParameters* parameters);
} // namespace FfmpegSupport
