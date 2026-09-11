#pragma once

struct VideoFrame
{
    std::vector<std::uint8_t> pixels;
    std::uint32_t width{ 0 };
    std::uint32_t height{ 0 };
    std::uint32_t rowPitch{ 0 };
    std::uint64_t serial{ 0 };
};
