#pragma once

#include "FrameScaler.h"
#include "VideoFrame.h"
#include "VideoLayout.h"

#include <cstdint>
#include <vector>

namespace BinkFrameScaler
{
    struct CoverCrop
    {
        std::uint32_t x{ 0 };
        std::uint32_t y{ 0 };
        std::uint32_t width{ 0 };
        std::uint32_t height{ 0 };
    };

    [[nodiscard]] CoverCrop ComputeCoverCrop(std::uint32_t inputWidth,
        std::uint32_t inputHeight,
        std::uint32_t outputWidth,
        std::uint32_t outputHeight) noexcept;

    class Scaler
    {
      public:
        Scaler() = default;
        ~Scaler();
        Scaler(const Scaler&) = delete;
        Scaler& operator=(const Scaler&) = delete;

        void Reset() noexcept;

        [[nodiscard]] bool PrepareSource(std::uint32_t width,
            std::uint32_t height);
        [[nodiscard]] std::uint8_t* SourceData() noexcept;

        [[nodiscard]] bool Scale(const CoverCrop& crop,
            const VideoLayout::OutputRect& destination,
            VideoFrame& output);
        [[nodiscard]] ScalingAlgorithm ActiveAlgorithm() const noexcept;
        [[nodiscard]] bool FellBackToBicubic() const noexcept;
        [[nodiscard]] const std::string& LastError() const noexcept;

      private:
        VideoScaling::FrameScaler scaler_;
        std::vector<std::uint8_t> source_;
        std::uint32_t sourceWidth_{ 0 };
        std::uint32_t sourceHeight_{ 0 };
    };
} // namespace BinkFrameScaler
