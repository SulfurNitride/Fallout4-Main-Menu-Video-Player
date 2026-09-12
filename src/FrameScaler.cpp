#include "PCH.h"

#include "FrameScaler.h"
#include "VideoScaling.h"

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace VideoScaling
{
    FrameScaler::~FrameScaler() { Reset(); }

    void FrameScaler::ResetBackend() noexcept
    {
        sws_freeContext(sws_);
        sws_ = nullptr;
        av_frame_free(&filtered_);
        avfilter_graph_free(&graph_);
        source_ = nullptr;
        sink_ = nullptr;
        configured_ = false;
    }

    void FrameScaler::Reset() noexcept
    {
        ResetBackend();
        signature_ = {};
        fellBack_ = false;
        directCopy_ = false;
        active_ = ScalingAlgorithm::Bicubic;
        lastError_.clear();
    }

    void FrameScaler::SetError(const std::string_view operation,
                               const int error)
    {
        std::array<char, AV_ERROR_MAX_STRING_SIZE> message{};
        if (error < 0 && av_strerror(error, message.data(), message.size()) >=
                             0) {
            lastError_ = std::format("{}: {}", operation, message.data());
        } else {
            lastError_ = std::format("{}: error {}", operation, error);
        }
    }

    bool FrameScaler::ConfigureBicubic(const Signature& signature)
    {
        sws_ = CreateAutoThreadedBicubicScaler(
            signature.inputWidth,
            signature.inputHeight,
            static_cast<AVPixelFormat>(signature.inputFormat),
            signature.outputWidth,
            signature.outputHeight,
            static_cast<AVPixelFormat>(signature.outputFormat));
        if (!sws_) {
            lastError_ = "libswscale could not create the Bicubic scaler";
            return false;
        }
        active_ = ScalingAlgorithm::Bicubic;
        return true;
    }

    bool FrameScaler::ConfigureSpline36(const Signature& signature)
    {
        if (signature.outputFormat != AV_PIX_FMT_BGRA) {
            lastError_ = "zscale output must be BGRA";
            return false;
        }

        const AVFilter* buffer = avfilter_get_by_name("buffer");
        const AVFilter* zscale = avfilter_get_by_name("zscale");
        const AVFilter* format = avfilter_get_by_name("format");
        const AVFilter* bufferSink = avfilter_get_by_name("buffersink");
        if (!buffer || !zscale || !format || !bufferSink) {
            lastError_ = "FFmpeg was built without the required zscale graph";
            return false;
        }

        graph_ = avfilter_graph_alloc();
        if (!graph_) {
            lastError_ = "FFmpeg could not allocate the zscale graph";
            return false;
        }
        graph_->thread_type = AVFILTER_THREAD_SLICE;
        graph_->nb_threads = 0;

        source_ = avfilter_graph_alloc_filter(graph_, buffer, "source");
        if (!source_) {
            lastError_ = "FFmpeg could not allocate the zscale input";
            return false;
        }

        AVBufferSrcParameters* parameters = av_buffersrc_parameters_alloc();
        if (!parameters) {
            lastError_ = "FFmpeg could not allocate zscale input parameters";
            return false;
        }
        parameters->format = signature.inputFormat;
        parameters->time_base = { 1, 1 };
        parameters->width = signature.inputWidth;
        parameters->height = signature.inputHeight;
        parameters->sample_aspect_ratio = { 1, 1 };
        parameters->color_space =
            static_cast<AVColorSpace>(signature.inputColorSpace);
        parameters->color_range =
            static_cast<AVColorRange>(signature.inputColorRange);
        int result = av_buffersrc_parameters_set(source_, parameters);
        av_free(parameters);
        if (result >= 0) {
            result = avfilter_init_str(source_, nullptr);
        }
        if (result < 0) {
            SetError("FFmpeg could not initialize the zscale input", result);
            return false;
        }

        AVFilterContext* scaler = nullptr;
        AVFilterContext* planarRgb = nullptr;
        // The Bink 32-bit surface has no meaningful alpha channel. Keeping
        // its zero alpha through zscale premultiplies the RGB channels to
        // black, so scale opaque planar RGB and restore alpha at BGRA output.
        result = avfilter_graph_create_filter(&planarRgb, format, "planar-rgb",
                                              "pix_fmts=gbrp", nullptr,
                                              graph_);
        if (result < 0) {
            SetError("FFmpeg could not initialize planar RGB input", result);
            return false;
        }

        const std::string scalerOptions =
            std::format("w={}:h={}:filter=spline36:dither=none",
                        signature.outputWidth, signature.outputHeight);
        result = avfilter_graph_create_filter(&scaler, zscale, "spline36",
                                              scalerOptions.c_str(), nullptr,
                                              graph_);
        if (result < 0) {
            SetError("FFmpeg could not initialize Spline36", result);
            return false;
        }

        AVFilterContext* outputFormat = nullptr;
        result = avfilter_graph_create_filter(&outputFormat, format, "bgra",
                                              "pix_fmts=bgra", nullptr,
                                              graph_);
        if (result < 0) {
            SetError("FFmpeg could not initialize BGRA output", result);
            return false;
        }

        result = avfilter_graph_create_filter(&sink_, bufferSink, "sink",
                                              nullptr, nullptr, graph_);
        if (result >= 0) {
            result = avfilter_link(source_, 0, planarRgb, 0);
        }
        if (result >= 0) {
            result = avfilter_link(planarRgb, 0, scaler, 0);
        }
        if (result >= 0) {
            result = avfilter_link(scaler, 0, outputFormat, 0);
        }
        if (result >= 0) {
            result = avfilter_link(outputFormat, 0, sink_, 0);
        }
        if (result >= 0) {
            result = avfilter_graph_config(graph_, nullptr);
        }
        if (result < 0) {
            SetError("FFmpeg could not configure Spline36", result);
            return false;
        }

        filtered_ = av_frame_alloc();
        if (!filtered_) {
            lastError_ = "FFmpeg could not allocate the Spline36 output frame";
            return false;
        }
        active_ = ScalingAlgorithm::Spline36;
        return true;
    }

    bool FrameScaler::Configure(const Signature& signature)
    {
        ResetBackend();
        signature_ = signature;
        fellBack_ = false;
        directCopy_ = false;
        lastError_.clear();

        if (signature.requested == ScalingAlgorithm::Spline36) {
            if (ConfigureSpline36(signature)) {
                configured_ = true;
                return true;
            }
            const std::string splineError = lastError_;
            ResetBackend();
            lastError_ = splineError;
            fellBack_ = true;
        }
        if (!ConfigureBicubic(signature)) {
            ResetBackend();
            return false;
        }
        configured_ = true;
        return true;
    }

    bool FrameScaler::ScaleBicubic(AVFrame* source, AVFrame* destination)
    {
        const int result = sws_scale_frame(sws_, destination, source);
        if (result < 0) {
            SetError("Bicubic scaling failed", result);
            return false;
        }
        return true;
    }

    bool FrameScaler::ScaleSpline36(AVFrame* source, AVFrame* destination)
    {
        int result = av_buffersrc_add_frame_flags(
            source_, source, AV_BUFFERSRC_FLAG_KEEP_REF);
        if (result < 0) {
            SetError("Spline36 rejected the input frame", result);
            return false;
        }

        av_frame_unref(filtered_);
        result = av_buffersink_get_frame(sink_, filtered_);
        if (result < 0) {
            SetError("Spline36 produced no output frame", result);
            return false;
        }
        if (filtered_->format != destination->format ||
            filtered_->width != destination->width ||
            filtered_->height != destination->height) {
            lastError_ = "Spline36 produced an unexpected output layout";
            return false;
        }

        av_image_copy2(destination->data, destination->linesize,
                       filtered_->data, filtered_->linesize,
                       static_cast<AVPixelFormat>(destination->format),
                       destination->width, destination->height);
        return true;
    }

    bool FrameScaler::Scale(AVFrame* source,
                            AVFrame* destination,
                            const ScalingAlgorithm requested)
    {
        if (!source || !destination || source->width <= 0 ||
            source->height <= 0 || destination->width <= 0 ||
            destination->height <= 0 || source->format == AV_PIX_FMT_NONE ||
            destination->format == AV_PIX_FMT_NONE) {
            lastError_ = "Invalid scaler frame layout";
            return false;
        }

        EnsureColorMetadata(source);
        EnsureColorMetadata(destination);
        const Signature signature{
            .inputWidth = source->width,
            .inputHeight = source->height,
            .inputFormat = source->format,
            .inputPrimaries = source->color_primaries,
            .inputTransfer = source->color_trc,
            .inputColorSpace = source->colorspace,
            .inputColorRange = source->color_range,
            .outputWidth = destination->width,
            .outputHeight = destination->height,
            .outputFormat = destination->format,
            .requested = requested,
        };
        if (source->width == destination->width &&
            source->height == destination->height &&
            source->format == AV_PIX_FMT_BGRA &&
            destination->format == AV_PIX_FMT_BGRA) {
            if (!configured_ || !(signature_ == signature) || !directCopy_) {
                ResetBackend();
                signature_ = signature;
                configured_ = true;
                directCopy_ = true;
                fellBack_ = false;
                active_ = requested;
                lastError_.clear();
            }
            av_image_copy2(destination->data, destination->linesize,
                           source->data, source->linesize, AV_PIX_FMT_BGRA,
                           destination->width, destination->height);
            return true;
        }
        if (!configured_ || !(signature_ == signature)) {
            if (!Configure(signature)) {
                return false;
            }
        }

        if (active_ == ScalingAlgorithm::Bicubic) {
            return ScaleBicubic(source, destination);
        }
        if (ScaleSpline36(source, destination)) {
            return true;
        }

        const std::string splineError = lastError_;
        ResetBackend();
        lastError_ = splineError;
        fellBack_ = true;
        signature_ = signature;
        if (!ConfigureBicubic(signature)) {
            ResetBackend();
            return false;
        }
        configured_ = true;
        return ScaleBicubic(source, destination);
    }

    ScalingAlgorithm FrameScaler::ActiveAlgorithm() const noexcept
    {
        return active_;
    }

    bool FrameScaler::UsedDirectCopy() const noexcept { return directCopy_; }

    bool FrameScaler::FellBackToBicubic() const noexcept
    {
        return fellBack_;
    }

    const std::string& FrameScaler::LastError() const noexcept
    {
        return lastError_;
    }
} // namespace VideoScaling
