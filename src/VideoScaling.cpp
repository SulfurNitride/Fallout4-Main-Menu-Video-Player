#include "VideoScaling.h"

#include <cstddef>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

namespace VideoScaling
{
    namespace
    {
        std::size_t CenteredLeadingCrop(
            const std::size_t removed, const std::size_t alignment)
        {
            const std::size_t center = removed / 2;
            const std::size_t lower = center / alignment * alignment;
            const std::size_t upper = lower + alignment;
            if (upper <= removed && upper - center <= center - lower) {
                return upper;
            }
            return lower;
        }
    }

    bool ApplyCenterCrop(AVFrame* frame,
        const std::uint32_t outputWidth,
        const std::uint32_t outputHeight)
    {
        if (!frame || frame->width <= 0 || frame->height <= 0 ||
            outputWidth == 0 || outputHeight == 0) {
            return false;
        }

        if ((frame->crop_left != 0 || frame->crop_right != 0 ||
                frame->crop_top != 0 || frame->crop_bottom != 0) &&
            av_frame_apply_cropping(frame, AV_FRAME_CROP_UNALIGNED) < 0) {
            return false;
        }

        const AVPixFmtDescriptor* pixelFormat =
            av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
        if (!pixelFormat) {
            return false;
        }

        const std::uint64_t inputAspect =
            static_cast<std::uint64_t>(frame->width) * outputHeight;
        const std::uint64_t outputAspect =
            static_cast<std::uint64_t>(outputWidth) * frame->height;
        std::size_t croppedWidth = static_cast<std::size_t>(frame->width);
        std::size_t croppedHeight = static_cast<std::size_t>(frame->height);
        if (inputAspect > outputAspect) {
            croppedWidth = static_cast<std::size_t>(
                static_cast<std::uint64_t>(frame->height) * outputWidth /
                outputHeight);
            if (croppedWidth == 0) {
                croppedWidth = 1;
            }
            const std::size_t removed =
                static_cast<std::size_t>(frame->width) - croppedWidth;
            frame->crop_left = CenteredLeadingCrop(
                removed, std::size_t{ 1 } << pixelFormat->log2_chroma_w);
            frame->crop_right = removed - frame->crop_left;
        } else if (inputAspect < outputAspect) {
            croppedHeight = static_cast<std::size_t>(
                static_cast<std::uint64_t>(frame->width) * outputHeight /
                outputWidth);
            if (croppedHeight == 0) {
                croppedHeight = 1;
            }
            const std::size_t removed =
                static_cast<std::size_t>(frame->height) - croppedHeight;
            frame->crop_top = CenteredLeadingCrop(
                removed, std::size_t{ 1 } << pixelFormat->log2_chroma_h);
            frame->crop_bottom = removed - frame->crop_top;
        }

        // The crop is aligned to complete chroma samples. Its pointers may not
        // retain FFmpeg's preferred SIMD alignment, which sws_scale supports
        // by selecting its unaligned input path.
        return av_frame_apply_cropping(
                   frame, AV_FRAME_CROP_UNALIGNED) >= 0 &&
               frame->width == static_cast<int>(croppedWidth) &&
               frame->height == static_cast<int>(croppedHeight);
    }
} // namespace VideoScaling
