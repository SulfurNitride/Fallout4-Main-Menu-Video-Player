#pragma once

#include "ScalingAlgorithm.h"

#include <string>

struct AVFilterContext;
struct AVFilterGraph;
struct AVFrame;
struct SwsContext;

namespace VideoScaling
{
    class FrameScaler
    {
      public:
        FrameScaler() = default;
        ~FrameScaler();
        FrameScaler(const FrameScaler&) = delete;
        FrameScaler& operator=(const FrameScaler&) = delete;

        void Reset() noexcept;

        [[nodiscard]] bool Scale(AVFrame* source,
                                 AVFrame* destination,
                                 ScalingAlgorithm requested);
        [[nodiscard]] ScalingAlgorithm ActiveAlgorithm() const noexcept;
        [[nodiscard]] bool UsedDirectCopy() const noexcept;
        [[nodiscard]] bool FellBackToBicubic() const noexcept;
        [[nodiscard]] const std::string& LastError() const noexcept;

      private:
        struct Signature
        {
            int inputWidth{ 0 };
            int inputHeight{ 0 };
            int inputFormat{ -1 };
            int inputPrimaries{ 0 };
            int inputTransfer{ 0 };
            int inputColorSpace{ 0 };
            int inputColorRange{ 0 };
            int outputWidth{ 0 };
            int outputHeight{ 0 };
            int outputFormat{ -1 };
            ScalingAlgorithm requested{ ScalingAlgorithm::Bicubic };

            [[nodiscard]] bool operator==(const Signature&) const = default;
        };

        [[nodiscard]] bool Configure(const Signature& signature);
        [[nodiscard]] bool ConfigureBicubic(const Signature& signature);
        [[nodiscard]] bool ConfigureSpline36(const Signature& signature);
        [[nodiscard]] bool ScaleBicubic(AVFrame* source,
                                        AVFrame* destination);
        [[nodiscard]] bool ScaleSpline36(AVFrame* source,
                                         AVFrame* destination);
        void ResetBackend() noexcept;
        void SetError(std::string_view operation, int error);

        Signature signature_{};
        bool configured_{ false };
        bool directCopy_{ false };
        bool fellBack_{ false };
        ScalingAlgorithm active_{ ScalingAlgorithm::Bicubic };
        ::SwsContext* sws_{ nullptr };
        ::AVFilterGraph* graph_{ nullptr };
        ::AVFilterContext* source_{ nullptr };
        ::AVFilterContext* sink_{ nullptr };
        ::AVFrame* filtered_{ nullptr };
        std::string lastError_;
    };
} // namespace VideoScaling
