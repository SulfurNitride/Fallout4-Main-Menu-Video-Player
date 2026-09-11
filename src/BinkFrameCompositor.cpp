#include "PCH.h"

#include "BinkFrameCompositor.h"

namespace BinkFrameCompositor
{
    namespace
    {
        constexpr std::uint32_t kSurfaceMask{ 15 };
        constexpr std::uint32_t kSurface24{ 1 };
        constexpr std::uint32_t kSurface24Reversed{ 2 };
        constexpr std::uint32_t kSurface32{ 3 };
        constexpr std::uint32_t kSurface32Reversed{ 4 };
        constexpr std::uint32_t kSurface32Alpha{ 5 };
        constexpr std::uint32_t kSurface32ReversedAlpha{ 6 };

        struct SurfaceLayout
        {
            std::uint32_t bytesPerPixel{ 0 };
            bool reversed{ false };
        };

        [[nodiscard]] SurfaceLayout DescribeSurface(
            const std::uint32_t flags) noexcept
        {
            const std::uint32_t surface = flags & kSurfaceMask;
            const bool reversed = surface == kSurface24Reversed ||
                                  surface == kSurface32Reversed ||
                                  surface == kSurface32ReversedAlpha;
            const std::uint32_t bytesPerPixel =
                surface == kSurface24 || surface == kSurface24Reversed ? 3
                : surface == kSurface32 || surface == kSurface32Reversed ||
                        surface == kSurface32Alpha ||
                        surface == kSurface32ReversedAlpha
                    ? 4
                    : 0;
            return { bytesPerPixel, reversed };
        }

        struct ScaleMap
        {
            void Update(const VideoFrame& frame,
                const std::uint32_t outputWidth,
                const std::uint32_t outputHeight)
            {
                if (inputWidth == frame.width && inputHeight == frame.height &&
                    targetWidth == outputWidth &&
                    targetHeight == outputHeight) {
                    return;
                }

                inputWidth = frame.width;
                inputHeight = frame.height;
                targetWidth = outputWidth;
                targetHeight = outputHeight;

                std::uint32_t cropX = 0;
                std::uint32_t cropY = 0;
                std::uint32_t cropWidth = frame.width;
                std::uint32_t cropHeight = frame.height;
                if (static_cast<std::uint64_t>(frame.width) * outputHeight >
                    static_cast<std::uint64_t>(outputWidth) * frame.height) {
                    cropWidth = static_cast<std::uint32_t>(
                        static_cast<std::uint64_t>(frame.height) * outputWidth /
                        outputHeight);
                    cropX = (frame.width - cropWidth) / 2;
                } else {
                    cropHeight = static_cast<std::uint32_t>(
                        static_cast<std::uint64_t>(frame.width) * outputHeight /
                        outputWidth);
                    cropY = (frame.height - cropHeight) / 2;
                }

                x.resize(outputWidth);
                y.resize(outputHeight);
                for (std::uint32_t column = 0; column < outputWidth; ++column) {
                    x[column] = std::min(
                        cropX + static_cast<std::uint32_t>(
                                    static_cast<std::uint64_t>(column) *
                                    cropWidth / outputWidth),
                        frame.width - 1);
                }
                for (std::uint32_t row = 0; row < outputHeight; ++row) {
                    y[row] =
                        std::min(cropY + static_cast<std::uint32_t>(
                                             static_cast<std::uint64_t>(row) *
                                             cropHeight / outputHeight),
                            frame.height - 1);
                }
            }

            [[nodiscard]] bool IsIdentity() const noexcept
            {
                return inputWidth == targetWidth && inputHeight == targetHeight;
            }

            std::uint32_t inputWidth{ 0 };
            std::uint32_t inputHeight{ 0 };
            std::uint32_t targetWidth{ 0 };
            std::uint32_t targetHeight{ 0 };
            std::vector<std::uint32_t> x;
            std::vector<std::uint32_t> y;
        };

        thread_local ScaleMap scaleMap;
    } // namespace

    CopyResult CopyCoverFrame(const VideoFrame& frame,
        void* destination,
        const std::int32_t destinationPitch,
        const std::uint32_t destinationHeight,
        const std::uint32_t destinationX,
        const std::uint32_t destinationY,
        const std::uint32_t sourceX,
        const std::uint32_t sourceY,
        const std::uint32_t sourceWidth,
        const std::uint32_t sourceHeight,
        const std::uint32_t outputWidth,
        const std::uint32_t outputHeight,
        const std::uint32_t flags)
    {
        if (!destination || destinationPitch == 0 ||
            destinationPitch == std::numeric_limits<std::int32_t>::min() ||
            destinationHeight == 0 || frame.width == 0 || frame.height == 0 ||
            static_cast<std::uint64_t>(frame.rowPitch) <
                static_cast<std::uint64_t>(frame.width) * 4 ||
            frame.pixels.size() <
                static_cast<std::uint64_t>(frame.rowPitch) * frame.height ||
            outputWidth == 0 || outputHeight == 0 || sourceX >= outputWidth ||
            sourceY >= outputHeight) {
            return CopyResult::kIgnored;
        }

        const SurfaceLayout layout = DescribeSurface(flags);
        if (layout.bytesPerPixel == 0) {
            return CopyResult::kUnsupportedSurface;
        }

        const std::uint64_t absolutePitch =
            destinationPitch < 0 ? static_cast<std::uint64_t>(-destinationPitch)
                                 : static_cast<std::uint64_t>(destinationPitch);
        const std::uint32_t rowCapacity =
            static_cast<std::uint32_t>(absolutePitch / layout.bytesPerPixel);
        if (destinationX >= rowCapacity || destinationY >= destinationHeight) {
            return CopyResult::kIgnored;
        }

        const std::uint32_t copyWidth = std::min(
            { sourceWidth, rowCapacity - destinationX, outputWidth - sourceX });
        const std::uint32_t copyHeight = std::min({ sourceHeight,
            destinationHeight - destinationY,
            outputHeight - sourceY });
        if (copyWidth == 0 || copyHeight == 0) {
            return CopyResult::kIgnored;
        }

        scaleMap.Update(frame, outputWidth, outputHeight);
        const bool directCopy = layout.bytesPerPixel == 4 && !layout.reversed &&
                                scaleMap.IsIdentity();

        auto* destinationBytes = static_cast<std::uint8_t*>(destination);
        for (std::uint32_t row = 0; row < copyHeight; ++row) {
            const std::uint32_t logicalY = sourceY + row;
            const std::uint32_t frameY = scaleMap.y[logicalY];
            const auto* sourceRow =
                frame.pixels.data() +
                static_cast<std::size_t>(frameY) * frame.rowPitch;
            auto* destinationRow =
                destinationBytes +
                static_cast<std::ptrdiff_t>(destinationY + row) *
                    destinationPitch +
                static_cast<std::size_t>(destinationX) * layout.bytesPerPixel;

            if (directCopy) {
                std::memcpy(destinationRow,
                    sourceRow + static_cast<std::size_t>(sourceX) * 4,
                    static_cast<std::size_t>(copyWidth) * 4);
                continue;
            }

            for (std::uint32_t column = 0; column < copyWidth; ++column) {
                const std::uint32_t logicalX = sourceX + column;
                const std::uint32_t frameX = scaleMap.x[logicalX];
                const auto* pixel =
                    sourceRow + static_cast<std::size_t>(frameX) * 4;
                auto* output =
                    destinationRow +
                    static_cast<std::size_t>(column) * layout.bytesPerPixel;
                if (layout.reversed) {
                    output[0] = pixel[2];
                    output[1] = pixel[1];
                    output[2] = pixel[0];
                } else {
                    output[0] = pixel[0];
                    output[1] = pixel[1];
                    output[2] = pixel[2];
                }
                if (layout.bytesPerPixel == 4) {
                    output[3] = 255;
                }
            }
        }
        return CopyResult::kCopied;
    }

    void BlendOverlay(const VideoFrame& overlay,
        const std::uint32_t left,
        const std::uint32_t top,
        void* destination,
        const std::int32_t destinationPitch,
        const std::uint32_t destinationHeight,
        const std::uint32_t destinationX,
        const std::uint32_t destinationY,
        const std::uint32_t sourceX,
        const std::uint32_t sourceY,
        const std::uint32_t sourceWidth,
        const std::uint32_t sourceHeight,
        const std::uint32_t flags)
    {
        if (!destination || destinationPitch == 0 ||
            destinationPitch == std::numeric_limits<std::int32_t>::min() ||
            destinationHeight == 0 || overlay.width == 0 ||
            overlay.height == 0 ||
            static_cast<std::uint64_t>(overlay.rowPitch) <
                static_cast<std::uint64_t>(overlay.width) * 4 ||
            overlay.pixels.size() <
                static_cast<std::uint64_t>(overlay.rowPitch) * overlay.height) {
            return;
        }

        const SurfaceLayout layout = DescribeSurface(flags);
        if (layout.bytesPerPixel == 0) {
            return;
        }
        const std::uint64_t absolutePitch =
            destinationPitch < 0 ? static_cast<std::uint64_t>(-destinationPitch)
                                 : static_cast<std::uint64_t>(destinationPitch);
        const std::uint32_t rowCapacity =
            static_cast<std::uint32_t>(absolutePitch / layout.bytesPerPixel);
        if (rowCapacity == 0) {
            return;
        }

        const std::uint32_t copyRight = static_cast<std::uint32_t>(
            std::min(static_cast<std::uint64_t>(sourceX) + sourceWidth,
                static_cast<std::uint64_t>(left) + overlay.width));
        const std::uint32_t copyBottom = static_cast<std::uint32_t>(
            std::min(static_cast<std::uint64_t>(sourceY) + sourceHeight,
                static_cast<std::uint64_t>(top) + overlay.height));
        const std::uint32_t copyLeft = std::max(sourceX, left);
        const std::uint32_t copyTop = std::max(sourceY, top);
        if (copyLeft >= copyRight || copyTop >= copyBottom) {
            return;
        }

        auto* destinationBytes = static_cast<std::uint8_t*>(destination);
        if (layout.bytesPerPixel == 4 && !layout.reversed) {
            for (std::uint32_t logicalY = copyTop; logicalY < copyBottom;
                ++logicalY) {
                const std::uint64_t outputY =
                    static_cast<std::uint64_t>(destinationY) + logicalY -
                    sourceY;
                if (outputY >= destinationHeight) {
                    break;
                }
                const std::uint64_t outputX =
                    static_cast<std::uint64_t>(destinationX) + copyLeft -
                    sourceX;
                if (outputX >= rowCapacity) {
                    continue;
                }
                const std::uint32_t pixelsToCopy =
                    static_cast<std::uint32_t>(std::min<std::uint64_t>(
                        copyRight - copyLeft, rowCapacity - outputX));
                auto* destinationRow =
                    destinationBytes +
                    static_cast<std::ptrdiff_t>(outputY) * destinationPitch +
                    static_cast<std::size_t>(outputX) * 4;
                const auto* overlayRow =
                    overlay.pixels.data() +
                    static_cast<std::size_t>(logicalY - top) *
                        overlay.rowPitch +
                    static_cast<std::size_t>(copyLeft - left) * 4;
                std::memcpy(destinationRow,
                    overlayRow,
                    static_cast<std::size_t>(pixelsToCopy) * 4);
            }
            return;
        }

        for (std::uint32_t logicalY = copyTop; logicalY < copyBottom;
            ++logicalY) {
            const std::uint64_t outputY =
                static_cast<std::uint64_t>(destinationY) + logicalY - sourceY;
            if (outputY >= destinationHeight) {
                break;
            }
            auto* destinationRow =
                destinationBytes +
                static_cast<std::ptrdiff_t>(outputY) * destinationPitch;
            const auto* overlayRow =
                overlay.pixels.data() +
                static_cast<std::size_t>(logicalY - top) * overlay.rowPitch;
            for (std::uint32_t logicalX = copyLeft; logicalX < copyRight;
                ++logicalX) {
                const std::uint64_t outputX =
                    static_cast<std::uint64_t>(destinationX) + logicalX -
                    sourceX;
                if (outputX >= rowCapacity) {
                    break;
                }
                const auto* sourcePixel =
                    overlayRow + static_cast<std::size_t>(logicalX - left) * 4;
                const std::uint32_t alpha = sourcePixel[3];
                if (alpha == 0) {
                    continue;
                }
                auto* outputPixel =
                    destinationRow +
                    static_cast<std::size_t>(outputX) * layout.bytesPerPixel;
                const std::size_t blueIndex = layout.reversed ? 2 : 0;
                const std::size_t redIndex = layout.reversed ? 0 : 2;
                outputPixel[blueIndex] = static_cast<std::uint8_t>(
                    (sourcePixel[0] * alpha +
                        outputPixel[blueIndex] * (255 - alpha)) /
                    255);
                outputPixel[1] = static_cast<std::uint8_t>(
                    (sourcePixel[1] * alpha + outputPixel[1] * (255 - alpha)) /
                    255);
                outputPixel[redIndex] = static_cast<std::uint8_t>(
                    (sourcePixel[2] * alpha +
                        outputPixel[redIndex] * (255 - alpha)) /
                    255);
                if (layout.bytesPerPixel == 4) {
                    outputPixel[3] = 255;
                }
            }
        }
    }
} // namespace BinkFrameCompositor
