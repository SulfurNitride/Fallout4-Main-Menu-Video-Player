#pragma once

#include "VideoFrame.h"

namespace BinkFrameCompositor
{
    enum class CopyResult
    {
        kCopied,
        kIgnored,
        kUnsupportedSurface
    };

    [[nodiscard]] CopyResult CopyCoverFrame(const VideoFrame& frame,
        void* destination,
        std::int32_t destinationPitch,
        std::uint32_t destinationHeight,
        std::uint32_t destinationX,
        std::uint32_t destinationY,
        std::uint32_t sourceX,
        std::uint32_t sourceY,
        std::uint32_t sourceWidth,
        std::uint32_t sourceHeight,
        std::uint32_t outputWidth,
        std::uint32_t outputHeight,
        std::uint32_t flags);

    void BlendOverlay(const VideoFrame& overlay,
        std::uint32_t left,
        std::uint32_t top,
        void* destination,
        std::int32_t destinationPitch,
        std::uint32_t destinationHeight,
        std::uint32_t destinationX,
        std::uint32_t destinationY,
        std::uint32_t sourceX,
        std::uint32_t sourceY,
        std::uint32_t sourceWidth,
        std::uint32_t sourceHeight,
        std::uint32_t flags);
} // namespace BinkFrameCompositor
