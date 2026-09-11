#pragma once

#include <cstdint>

struct AVFrame;

namespace VideoScaling
{
    [[nodiscard]] bool ApplyCenterCrop(AVFrame* frame,
        std::uint32_t outputWidth,
        std::uint32_t outputHeight);
}
