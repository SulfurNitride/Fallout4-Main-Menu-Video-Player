#include "VideoLayout.h"

#include <algorithm>

namespace VideoLayout
{
    bool IsCarrierAspectMatch(const std::uint32_t carrierWidth,
        const std::uint32_t carrierHeight,
        const std::uint32_t presentationWidth,
        const std::uint32_t presentationHeight) noexcept
    {
        if (carrierWidth == 0 || carrierHeight == 0 ||
            presentationWidth == 0 || presentationHeight == 0) {
            return false;
        }

        const auto carrierAspect =
            static_cast<std::uint64_t>(carrierWidth) * presentationHeight;
        const auto presentationAspect =
            static_cast<std::uint64_t>(presentationWidth) * carrierHeight;
        const auto difference = carrierAspect > presentationAspect
                                    ? carrierAspect - presentationAspect
                                    : presentationAspect - carrierAspect;
        // Accept ordinary 21:9 rounding (for example 2560x1080 on a
        // 3440x1440 window), but reject a 16:9 carrier on an ultrawide one.
        return difference <=
               std::max(carrierAspect, presentationAspect) / 100;
    }

    namespace
    {
        std::uint64_t RoundedDivide(
            const std::uint64_t numerator,
            const std::uint32_t denominator) noexcept
        {
            const std::uint64_t quotient = numerator / denominator;
            const std::uint64_t remainder = numerator % denominator;
            return quotient +
                   (remainder >=
                            (static_cast<std::uint64_t>(denominator) + 1) / 2
                        ? 1
                        : 0);
        }
    } // namespace

    OutputRect ComputeAspectFitRect(const std::uint32_t carrierWidth,
        const std::uint32_t carrierHeight,
        const std::uint32_t presentationWidth,
        const std::uint32_t presentationHeight) noexcept
    {
        if (carrierWidth == 0 || carrierHeight == 0) {
            return {};
        }

        OutputRect result{ .width = carrierWidth, .height = carrierHeight };
        if (presentationWidth == 0 || presentationHeight == 0) {
            return result;
        }

        const auto carrierAspect =
            static_cast<std::uint64_t>(carrierWidth) * presentationHeight;
        const auto presentationAspect =
            static_cast<std::uint64_t>(presentationWidth) * carrierHeight;
        if (carrierAspect == presentationAspect) {
            return result;
        }

        if (presentationAspect > carrierAspect) {
            const auto numerator =
                static_cast<std::uint64_t>(carrierWidth) * presentationHeight;
            result.height =
                std::max(1U, static_cast<std::uint32_t>(
                                 RoundedDivide(numerator, presentationWidth)));
            result.height = std::min(result.height, carrierHeight);
            result.y = (carrierHeight - result.height) / 2;
        } else {
            const auto numerator =
                static_cast<std::uint64_t>(carrierHeight) * presentationWidth;
            result.width =
                std::max(1U, static_cast<std::uint32_t>(
                                 RoundedDivide(numerator, presentationHeight)));
            result.width = std::min(result.width, carrierWidth);
            result.x = (carrierWidth - result.width) / 2;
        }
        return result;
    }
} // namespace VideoLayout
