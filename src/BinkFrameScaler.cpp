#include "PCH.h"

#include "BinkFrameScaler.h"
#include "Config.h"
#include "VideoScaling.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace BinkFrameScaler
{
    CoverCrop ComputeCoverCrop(const std::uint32_t inputWidth,
                               const std::uint32_t inputHeight,
                               const std::uint32_t outputWidth,
                               const std::uint32_t outputHeight) noexcept
    {
        CoverCrop crop{ .width = inputWidth, .height = inputHeight };
        if (inputWidth == 0 || inputHeight == 0 || outputWidth == 0 ||
            outputHeight == 0) {
            return {};
        }

        if (static_cast<std::uint64_t>(inputWidth) * outputHeight >
            static_cast<std::uint64_t>(outputWidth) * inputHeight) {
            crop.width =
                std::max(1U, static_cast<std::uint32_t>(
                                 static_cast<std::uint64_t>(inputHeight) *
                                 outputWidth / outputHeight));
            crop.width = std::min(crop.width, inputWidth);
            crop.x = (inputWidth - crop.width) / 2;
        } else {
            crop.height =
                std::max(1U, static_cast<std::uint32_t>(
                                 static_cast<std::uint64_t>(inputWidth) *
                                 outputHeight / outputWidth));
            crop.height = std::min(crop.height, inputHeight);
            crop.y = (inputHeight - crop.height) / 2;
        }
        return crop;
    }

    Scaler::~Scaler() { Reset(); }

    void Scaler::Reset() noexcept
    {
        scaler_.Reset();
        std::vector<std::uint8_t>().swap(source_);
        sourceWidth_ = 0;
        sourceHeight_ = 0;
    }

    bool Scaler::PrepareSource(const std::uint32_t width,
                               const std::uint32_t height)
    {
        if (width == 0 || height == 0 ||
            static_cast<std::uint64_t>(width) * height >
                std::numeric_limits<std::size_t>::max() / 4) {
            return false;
        }
        const std::size_t byteCount =
            static_cast<std::size_t>(width) * height * 4;
        try {
            source_.resize(byteCount);
        } catch (const std::bad_alloc&) {
            return false;
        }
        sourceWidth_ = width;
        sourceHeight_ = height;
        return true;
    }

    std::uint8_t* Scaler::SourceData() noexcept { return source_.data(); }

    bool Scaler::Scale(const CoverCrop& crop,
                       const VideoLayout::OutputRect& destination,
                       VideoFrame& output)
    {
        const std::uint32_t outputWidth = destination.width;
        const std::uint32_t outputHeight = destination.height;
        if (sourceWidth_ == 0 || sourceHeight_ == 0 || crop.width == 0 ||
            crop.height == 0 || outputWidth == 0 || outputHeight == 0 ||
            output.width == 0 || output.height == 0 ||
            static_cast<std::uint64_t>(output.rowPitch) <
                static_cast<std::uint64_t>(output.width) * 4 ||
            output.pixels.size() <
                static_cast<std::uint64_t>(output.rowPitch) * output.height ||
            source_.size() <
                static_cast<std::uint64_t>(sourceWidth_) * sourceHeight_ * 4 ||
            static_cast<std::uint64_t>(crop.x) + crop.width > sourceWidth_ ||
            static_cast<std::uint64_t>(crop.y) + crop.height > sourceHeight_ ||
            static_cast<std::uint64_t>(sourceWidth_) * 4 >
                std::numeric_limits<int>::max() ||
            static_cast<std::uint64_t>(outputWidth) >
                std::numeric_limits<int>::max() ||
            static_cast<std::uint64_t>(outputHeight) >
                std::numeric_limits<int>::max() ||
            static_cast<std::uint64_t>(crop.width) >
                std::numeric_limits<int>::max() ||
            static_cast<std::uint64_t>(crop.height) >
                std::numeric_limits<int>::max() ||
            static_cast<std::uint64_t>(destination.x) + outputWidth >
                output.width ||
            static_cast<std::uint64_t>(destination.y) + outputHeight >
                output.height ||
            static_cast<std::uint64_t>(output.rowPitch) >
                std::numeric_limits<int>::max()) {
            return false;
        }

        AVFrame sourceFrame{};
        sourceFrame.format = AV_PIX_FMT_BGRA;
        sourceFrame.width = static_cast<int>(crop.width);
        sourceFrame.height = static_cast<int>(crop.height);
        sourceFrame.data[0] =
            source_.data() +
            static_cast<std::size_t>(crop.y) * sourceWidth_ * 4 +
            static_cast<std::size_t>(crop.x) * 4;
        sourceFrame.linesize[0] = static_cast<int>(sourceWidth_ * 4);
        VideoScaling::EnsureColorMetadata(&sourceFrame);

        AVFrame outputFrame{};
        outputFrame.format = AV_PIX_FMT_BGRA;
        outputFrame.width = static_cast<int>(outputWidth);
        outputFrame.height = static_cast<int>(outputHeight);
        outputFrame.data[0] =
            output.pixels.data() +
            static_cast<std::size_t>(destination.y) * output.rowPitch +
            static_cast<std::size_t>(destination.x) * 4;
        outputFrame.linesize[0] = static_cast<int>(output.rowPitch);
        VideoScaling::EnsureColorMetadata(&outputFrame);

        return scaler_.Scale(&sourceFrame, &outputFrame,
                             Config::ScalerAlgorithm());
    }

    ScalingAlgorithm Scaler::ActiveAlgorithm() const noexcept
    {
        return scaler_.ActiveAlgorithm();
    }

    bool Scaler::FellBackToBicubic() const noexcept
    {
        return scaler_.FellBackToBicubic();
    }

    const std::string& Scaler::LastError() const noexcept
    {
        return scaler_.LastError();
    }
} // namespace BinkFrameScaler
