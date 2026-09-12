#include "VideoScaling.h"

#include <cstddef>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace VideoScaling
{
    namespace
    {
        std::size_t CenteredLeadingCrop(const std::size_t removed,
                                        const std::size_t alignment)
        {
            const std::size_t center = removed / 2;
            const std::size_t lower = center / alignment * alignment;
            const std::size_t upper = lower + alignment;
            if (upper <= removed && upper - center <= center - lower) {
                return upper;
            }
            return lower;
        }
    } // namespace

    void EnsureColorMetadata(AVFrame* frame) noexcept
    {
        if (!frame) {
            return;
        }

        const AVPixFmtDescriptor* pixelFormat =
            av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
        const bool rgb =
            pixelFormat && (pixelFormat->flags & AV_PIX_FMT_FLAG_RGB) != 0;
        const bool highDefinition = frame->width >= 1280 || frame->height > 576;

        if (frame->color_primaries == AVCOL_PRI_RESERVED0 ||
            frame->color_primaries == AVCOL_PRI_RESERVED ||
            frame->color_primaries == AVCOL_PRI_UNSPECIFIED) {
            frame->color_primaries =
                highDefinition || rgb ? AVCOL_PRI_BT709 : AVCOL_PRI_SMPTE170M;
        }
        if (frame->color_trc == AVCOL_TRC_RESERVED0 ||
            frame->color_trc == AVCOL_TRC_RESERVED ||
            frame->color_trc == AVCOL_TRC_UNSPECIFIED) {
            frame->color_trc = rgb ? AVCOL_TRC_IEC61966_2_1
                                   : highDefinition ? AVCOL_TRC_BT709
                                                    : AVCOL_TRC_SMPTE170M;
        }
        if (frame->colorspace == AVCOL_SPC_RESERVED ||
            frame->colorspace == AVCOL_SPC_UNSPECIFIED) {
            frame->colorspace =
                rgb ? AVCOL_SPC_RGB
                    : highDefinition ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M;
        }
        if (frame->color_range == AVCOL_RANGE_UNSPECIFIED) {
            frame->color_range = rgb ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
        }
    }

    SwsContext* CreateAutoThreadedBicubicScaler(
        const int inputWidth,
        const int inputHeight,
        const AVPixelFormat inputFormat,
        const int outputWidth,
        const int outputHeight,
        const AVPixelFormat outputFormat)
    {
        if (inputWidth <= 0 || inputHeight <= 0 || outputWidth <= 0 ||
            outputHeight <= 0 || inputFormat == AV_PIX_FMT_NONE ||
            outputFormat == AV_PIX_FMT_NONE) {
            return nullptr;
        }

        SwsContext* context = sws_alloc_context();
        if (!context) {
            return nullptr;
        }

        const AVPixFmtDescriptor* descriptor =
            av_pix_fmt_desc_get(inputFormat);
        const bool rgb =
            descriptor && (descriptor->flags & AV_PIX_FMT_FLAG_RGB) != 0;
        const int flags =
            SWS_BICUBIC | (rgb ? 0 : SWS_ACCURATE_RND);
        const bool configured =
            av_opt_set_int(context, "sws_flags", flags, 0) >= 0 &&
            // Zero lets libswscale select all useful workers automatically.
            av_opt_set_int(context, "threads", 0, 0) >= 0;
        if (!configured) {
            sws_freeContext(context);
            return nullptr;
        }
        return context;
    }

    bool ApplyCenterCrop(AVFrame* frame, const std::uint32_t outputWidth,
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
        return av_frame_apply_cropping(frame, AV_FRAME_CROP_UNALIGNED) >= 0 &&
               frame->width == static_cast<int>(croppedWidth) &&
               frame->height == static_cast<int>(croppedHeight);
    }
} // namespace VideoScaling
