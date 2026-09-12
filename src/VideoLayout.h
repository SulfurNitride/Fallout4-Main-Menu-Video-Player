#pragma once

#include <cstdint>

namespace VideoLayout
{
    struct OutputRect
    {
        std::uint32_t x {0};
        std::uint32_t y {0};
        std::uint32_t width {0};
        std::uint32_t height {0};
    };

    // Return the largest rectangle inside the carrier that preserves the
    // requested presentation aspect. A missing presentation size returns the
    // full carrier so callers retain the legacy behavior.
    [[nodiscard]] OutputRect
    ComputeAspectFitRect(std::uint32_t carrierWidth,
                         std::uint32_t carrierHeight,
                         std::uint32_t presentationWidth,
                         std::uint32_t presentationHeight) noexcept;

    // A carrier must be close to the actual window aspect: fitting video
    // inside a 16:9 carrier does not change Fallout's 16:9 presentation quad.
    [[nodiscard]] bool IsCarrierAspectMatch(
        std::uint32_t carrierWidth, std::uint32_t carrierHeight,
        std::uint32_t presentationWidth,
        std::uint32_t presentationHeight) noexcept;
} // namespace VideoLayout
