#pragma once

#include <cstdint>

extern "C" {
#include <libavutil/pixfmt.h>
}

struct AVFrame;
struct SwsContext;

namespace VideoScaling
{
    void EnsureColorMetadata(AVFrame* frame) noexcept;

    [[nodiscard]] SwsContext* CreateAutoThreadedBicubicScaler(
        int inputWidth, int inputHeight, AVPixelFormat inputFormat,
        int outputWidth, int outputHeight, AVPixelFormat outputFormat);

    [[nodiscard]] bool ApplyCenterCrop(AVFrame* frame,
                                       std::uint32_t outputWidth,
                                       std::uint32_t outputHeight);
} // namespace VideoScaling
