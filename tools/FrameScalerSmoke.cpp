#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "FrameScaler.h"
#include "VideoScaling.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
}

namespace
{
    constexpr std::uint8_t kUnwritten{ 0xA5 };

    std::uint64_t Hash(const std::vector<std::uint8_t>& bytes)
    {
        std::uint64_t value = 1469598103934665603ULL;
        for (const std::uint8_t byte : bytes) {
            value ^= byte;
            value *= 1099511628211ULL;
        }
        return value;
    }

    void FillYuv420(AVFrame& frame)
    {
        for (int y = 0; y < frame.height; ++y) {
            for (int x = 0; x < frame.width; ++x) {
                frame.data[0][y * frame.linesize[0] + x] =
                    static_cast<std::uint8_t>(16 + (x * 219 / frame.width));
            }
        }
        for (int y = 0; y < frame.height / 2; ++y) {
            for (int x = 0; x < frame.width / 2; ++x) {
                frame.data[1][y * frame.linesize[1] + x] =
                    static_cast<std::uint8_t>(96 + (y % 64));
                frame.data[2][y * frame.linesize[2] + x] =
                    static_cast<std::uint8_t>(160 - (x % 64));
            }
        }
    }

    bool RunScale(const ScalingAlgorithm algorithm, std::uint64_t& hash)
    {
        constexpr int inputWidth = 320;
        constexpr int inputHeight = 180;
        constexpr int outputWidth = 640;
        constexpr int outputHeight = 360;

        const int inputBytes = av_image_get_buffer_size(
            AV_PIX_FMT_YUV420P, inputWidth, inputHeight, 1);
        const int outputBytes = av_image_get_buffer_size(
            AV_PIX_FMT_BGRA, outputWidth, outputHeight, 1);
        if (inputBytes <= 0 || outputBytes <= 0) {
            return false;
        }

        std::vector<std::uint8_t> input(
            static_cast<std::size_t>(inputBytes));
        std::vector<std::uint8_t> output(
            static_cast<std::size_t>(outputBytes), kUnwritten);
        AVFrame source{};
        source.format = AV_PIX_FMT_YUV420P;
        source.width = inputWidth;
        source.height = inputHeight;
        AVFrame destination{};
        destination.format = AV_PIX_FMT_BGRA;
        destination.width = outputWidth;
        destination.height = outputHeight;
        if (av_image_fill_arrays(source.data, source.linesize, input.data(),
                                 AV_PIX_FMT_YUV420P, inputWidth, inputHeight,
                                 1) < 0 ||
            av_image_fill_arrays(destination.data, destination.linesize,
                                 output.data(), AV_PIX_FMT_BGRA, outputWidth,
                                 outputHeight, 1) < 0) {
            return false;
        }
        FillYuv420(source);
        VideoScaling::EnsureColorMetadata(&source);
        VideoScaling::EnsureColorMetadata(&destination);

        VideoScaling::FrameScaler scaler;
        if (!scaler.Scale(&source, &destination, algorithm)) {
            std::printf("%s failed: %s\n", ScalingAlgorithmName(algorithm).data(),
                        scaler.LastError().c_str());
            return false;
        }
        if (scaler.ActiveAlgorithm() != algorithm ||
            scaler.FellBackToBicubic() || scaler.UsedDirectCopy() ||
            std::ranges::all_of(output, [](const std::uint8_t value) {
                return value == kUnwritten;
            })) {
            std::printf("%s did not execute its requested backend\n",
                        ScalingAlgorithmName(algorithm).data());
            return false;
        }
        hash = Hash(output);
        scaler.Reset();
        return true;
    }

    bool RunDirectCopy()
    {
        constexpr int width = 64;
        constexpr int height = 32;
        std::vector<std::uint8_t> input(
            static_cast<std::size_t>(width) * height * 4);
        for (std::size_t index = 0; index < input.size(); ++index) {
            input[index] = static_cast<std::uint8_t>(index * 37U);
        }
        std::vector<std::uint8_t> output(input.size(), kUnwritten);
        AVFrame source{};
        source.format = AV_PIX_FMT_BGRA;
        source.width = width;
        source.height = height;
        source.data[0] = input.data();
        source.linesize[0] = width * 4;
        AVFrame destination{};
        destination.format = AV_PIX_FMT_BGRA;
        destination.width = width;
        destination.height = height;
        destination.data[0] = output.data();
        destination.linesize[0] = width * 4;

        VideoScaling::FrameScaler scaler;
        return scaler.Scale(&source, &destination,
                            ScalingAlgorithm::Spline36) &&
               scaler.UsedDirectCopy() && output == input;
    }

    bool RunZeroAlphaBgraScale()
    {
        constexpr int inputWidth = 64;
        constexpr int inputHeight = 32;
        constexpr int outputWidth = 128;
        constexpr int outputHeight = 64;
        std::vector<std::uint8_t> input(
            static_cast<std::size_t>(inputWidth) * inputHeight * 4);
        for (std::size_t offset = 0; offset < input.size(); offset += 4) {
            input[offset] = 0;
            input[offset + 1] = 0;
            input[offset + 2] = 255;
            input[offset + 3] = 0;
        }
        std::vector<std::uint8_t> output(
            static_cast<std::size_t>(outputWidth) * outputHeight * 4,
            kUnwritten);
        AVFrame source{};
        source.format = AV_PIX_FMT_BGRA;
        source.width = inputWidth;
        source.height = inputHeight;
        source.data[0] = input.data();
        source.linesize[0] = inputWidth * 4;
        AVFrame destination{};
        destination.format = AV_PIX_FMT_BGRA;
        destination.width = outputWidth;
        destination.height = outputHeight;
        destination.data[0] = output.data();
        destination.linesize[0] = outputWidth * 4;

        VideoScaling::FrameScaler scaler;
        if (!scaler.Scale(&source, &destination,
                          ScalingAlgorithm::Spline36) ||
            scaler.ActiveAlgorithm() != ScalingAlgorithm::Spline36 ||
            scaler.FellBackToBicubic()) {
            std::printf("Zero-alpha BGRA Spline36 failed: %s\n",
                        scaler.LastError().c_str());
            return false;
        }
        for (std::size_t offset = 0; offset < output.size(); offset += 4) {
            if (output[offset] > 30 || output[offset + 1] > 30 ||
                output[offset + 2] < 180 || output[offset + 3] != 255) {
                std::puts("Zero-alpha BGRA became dark or transparent");
                return false;
            }
        }
        return true;
    }
}

int main()
{
    std::uint64_t bicubicHash = 0;
    std::uint64_t splineHash = 0;
    if (!RunScale(ScalingAlgorithm::Bicubic, bicubicHash) ||
        !RunScale(ScalingAlgorithm::Spline36, splineHash) ||
        !RunDirectCopy() || !RunZeroAlphaBgraScale() ||
        bicubicHash == splineHash) {
        std::puts("frame scaler smoke test failed");
        return 1;
    }
    std::printf("ok Bicubic=%016llx Spline36=%016llx "
                "direct-copy=yes zero-alpha-BGRA=yes\n",
                static_cast<unsigned long long>(bicubicHash),
                static_cast<unsigned long long>(splineHash));
    return 0;
}
