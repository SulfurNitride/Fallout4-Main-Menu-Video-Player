#include "VideoLayout.h"

#include <cstdio>

namespace
{
    bool Matches(const VideoLayout::OutputRect& actual,
        const std::uint32_t x,
        const std::uint32_t y,
        const std::uint32_t width,
        const std::uint32_t height)
    {
        return actual.x == x && actual.y == y && actual.width == width &&
               actual.height == height;
    }
} // namespace

int main()
{
    if (!VideoLayout::IsCarrierAspectMatch(3440, 1440, 3440, 1440) ||
        !VideoLayout::IsCarrierAspectMatch(2560, 1080, 3440, 1440) ||
        VideoLayout::IsCarrierAspectMatch(3840, 2160, 3440, 1440) ||
        VideoLayout::IsCarrierAspectMatch(0, 1440, 3440, 1440)) {
        return 1;
    }
    if (!Matches(VideoLayout::ComputeAspectFitRect(3840, 2160, 1920, 1080), 0,
                 0, 3840, 2160) ||
        !Matches(VideoLayout::ComputeAspectFitRect(3840, 2160, 3440, 1440), 0,
                 276, 3840, 1607) ||
        !Matches(VideoLayout::ComputeAspectFitRect(3840, 2160, 5120, 1440), 0,
                 540, 3840, 1080) ||
        !Matches(VideoLayout::ComputeAspectFitRect(3840, 2160, 1920, 1200), 192,
                 0, 3456, 2160) ||
        !Matches(VideoLayout::ComputeAspectFitRect(3840, 2160, 0, 0), 0, 0,
                 3840, 2160)) {
        std::puts("aspect-layout smoke test failed");
        return 1;
    }

    std::puts("ok aspect layouts: 16:9, 21:9, 32:9, 16:10, fallback");
    return 0;
}
