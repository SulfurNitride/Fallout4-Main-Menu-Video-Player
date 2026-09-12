#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <string_view>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace
{
    constexpr int kWarmupFrames = 4;
    constexpr int kMeasuredFrames = 16;

    struct ScalingCase
    {
        const char* key;
        const char* name;
        int inputWidth;
        int inputHeight;
        int outputWidth;
        int outputHeight;
    };

    struct Filter
    {
        const char* name;
        int flags;
    };

    struct Context
    {
        SwsContext* value{};

        ~Context() { sws_freeContext(value); }

        Context(const Context&) = delete;
        Context& operator=(const Context&) = delete;
        Context() = default;

        Context(Context&& other) noexcept : value(other.value)
        {
            other.value = nullptr;
        }

        Context& operator=(Context&& other) noexcept
        {
            if (this != &other) {
                sws_freeContext(value);
                value = other.value;
                other.value = nullptr;
            }
            return *this;
        }
    };

    bool SetInteger(SwsContext* context, const char* name, std::int64_t value)
    {
        if (av_opt_set_int(context, name, value, 0) >= 0) {
            return true;
        }
        std::fprintf(stderr, "Could not set swscale option %s\n", name);
        return false;
    }

    Context MakeContext(const Filter& filter,
        const int requestedThreads)
    {
        Context result;
        result.value = sws_alloc_context();
        if (!result.value) {
            return result;
        }

        const bool configured =
            SetInteger(result.value, "sws_flags", filter.flags) &&
            SetInteger(result.value, "threads", requestedThreads);
        if (!configured) {
            sws_freeContext(result.value);
            result.value = nullptr;
        }
        return result;
    }

    double Percentile(std::vector<double> samples, const double fraction)
    {
        std::sort(samples.begin(), samples.end());
        const auto index = static_cast<std::size_t>(
            fraction * static_cast<double>(samples.size() - 1));
        return samples[index];
    }

    std::uint64_t FrameHash(const std::vector<std::uint8_t>& pixels)
    {
        std::uint64_t hash = 1469598103934665603ULL;
        for (const std::uint8_t value : pixels) {
            hash ^= value;
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    bool Benchmark(const ScalingCase& scaling,
        const Filter& filter,
        const int requestedThreads,
        const std::vector<std::uint8_t>& source,
        std::vector<std::uint8_t>& destination)
    {
        auto context = MakeContext(filter, requestedThreads);
        if (!context.value) {
            std::fprintf(stderr, "Could not initialize %s context\n",
                filter.name);
            return false;
        }

        std::int64_t effectiveThreads = -1;
        av_opt_get_int(context.value, "threads", 0, &effectiveThreads);

        AVFrame input{};
        AVFrame output{};
        input.format = AV_PIX_FMT_BGRA;
        input.width = scaling.inputWidth;
        input.height = scaling.inputHeight;
        input.color_primaries = AVCOL_PRI_BT709;
        input.color_trc = AVCOL_TRC_IEC61966_2_1;
        input.colorspace = AVCOL_SPC_RGB;
        input.color_range = AVCOL_RANGE_JPEG;
        input.data[0] = const_cast<std::uint8_t*>(source.data());
        input.linesize[0] = scaling.inputWidth * 4;

        output.format = AV_PIX_FMT_BGRA;
        output.width = scaling.outputWidth;
        output.height = scaling.outputHeight;
        output.color_primaries = input.color_primaries;
        output.color_trc = input.color_trc;
        output.colorspace = input.colorspace;
        output.color_range = input.color_range;
        output.data[0] = destination.data();
        output.linesize[0] = scaling.outputWidth * 4;

        std::vector<double> samples;
        samples.reserve(kMeasuredFrames);
        for (int frame = -kWarmupFrames; frame < kMeasuredFrames; ++frame) {
            const auto start = std::chrono::steady_clock::now();
            const int result = sws_scale_frame(context.value, &output, &input);
            const auto end = std::chrono::steady_clock::now();
            if (result < 0) {
                std::fprintf(stderr, "sws_scale_frame failed: %d\n", result);
                return false;
            }
            if (frame >= 0) {
                samples.push_back(
                    std::chrono::duration<double, std::milli>(end - start)
                        .count());
            }
        }

        const double average =
            std::accumulate(samples.begin(), samples.end(), 0.0) /
            samples.size();
        std::printf(
            "%-17s requested=%2d effective=%2lld average=%8.3f ms "
            "median=%8.3f ms p95=%8.3f ms\n",
            filter.name,
            requestedThreads,
            static_cast<long long>(effectiveThreads),
            average,
            Percentile(samples, 0.50),
            Percentile(samples, 0.95));
        if (requestedThreads == 0) {
            std::printf("%-17s output-hash=%016llx\n",
                filter.name,
                static_cast<unsigned long long>(FrameHash(destination)));
        }

        return true;
    }
} // namespace

int main(const int argumentCount, const char* const* arguments)
{
    constexpr ScalingCase scalingCases[]{
        { "1080to1440", "1920x1080 -> 2560x1440", 1920, 1080, 2560,
            1440 },
        { "1080to4k", "1920x1080 -> 3840x2160", 1920, 1080, 3840,
            2160 },
        { "1440to4k", "2560x1440 -> 3840x2160", 2560, 1440, 3840,
            2160 },
        { "ultrawide", "3440x1440 -> 3840x1607", 3440, 1440, 3840,
            1607 },
    };
    constexpr Filter filters[]{
        { "bilinear", SWS_BILINEAR },
        { "bicubic", SWS_BICUBIC },
        { "bicubic+accurate", SWS_BICUBIC | SWS_ACCURATE_RND },
        { "lanczos", SWS_LANCZOS },
        { "lanczos+accurate", SWS_LANCZOS | SWS_ACCURATE_RND },
        { "spline", SWS_SPLINE },
        { "spline+accurate", SWS_SPLINE | SWS_ACCURATE_RND },
    };
    constexpr int threadCounts[]{ 1, 2, 4, 8, 0 };

    if (argumentCount > 2) {
        std::fprintf(stderr, "usage: mmvp_sws_benchmark [case]\n");
        return EXIT_FAILURE;
    }

    bool matchedCase = argumentCount == 1;
    for (const auto& scaling : scalingCases) {
        if (argumentCount == 2 &&
            std::string_view(arguments[1]) != scaling.key) {
            continue;
        }
        matchedCase = true;
        std::printf("\nBGRA %s\n", scaling.name);
        std::vector<std::uint8_t> source(
            static_cast<std::size_t>(scaling.inputWidth) *
            scaling.inputHeight * 4);
        std::vector<std::uint8_t> destination(
            static_cast<std::size_t>(scaling.outputWidth) *
            scaling.outputHeight * 4);
        for (std::size_t index = 0; index < source.size(); ++index) {
            source[index] = static_cast<std::uint8_t>(
                (index * 37U + index / 97U) & 0xFFU);
        }

        for (const auto& filter : filters) {
            for (const int threads : threadCounts) {
                if (!Benchmark(
                        scaling, filter, threads, source, destination)) {
                    return EXIT_FAILURE;
                }
            }
        }
    }

    if (!matchedCase) {
        std::fprintf(stderr, "unknown benchmark case: %s\n", arguments[1]);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
