#pragma once

#include <string_view>

enum class ScalingAlgorithm
{
    Bicubic,
    Spline36,
};

[[nodiscard]] constexpr std::string_view ScalingAlgorithmName(
    const ScalingAlgorithm algorithm) noexcept
{
    switch (algorithm) {
    case ScalingAlgorithm::Spline36:
        return "Spline36";
    case ScalingAlgorithm::Bicubic:
    default:
        return "Bicubic";
    }
}
