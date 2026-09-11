#include "PCH.h"

#include "AudioOutput.h"
#include "Config.h"
#include "FfmpegSupport.h"
#include "MainMenuMedia.h"
#include "PlaybackGate.h"
#include "VideoPlayer.h"
#include "VideoScaling.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace
{
    using FfmpegSupport::AvError;
    using FfmpegSupport::FindDecoder;
    using FfmpegSupport::FindVideoStream;
    using FfmpegSupport::Utf8Path;

} // namespace

VideoPlayer& VideoPlayer::GetSingleton()
{
    static VideoPlayer instance;
    return instance;
}

VideoPlayer::VideoPlayer()
    : audioRandom_(std::random_device{}()),
      worker_([this](std::stop_token stopToken) { Worker(stopToken); }),
      overrideAudioWorker_(
          [this](std::stop_token stopToken) { OverrideAudioWorker(stopToken); })
{
    volume_.store(Config::MainMenuVolume(), std::memory_order_release);
    // A populated MainMenuAudio folder opts into the dedicated soundtrack.
    // If it is empty, selection safely falls back to the video's audio.
    originalAudioPreferred_.store(false, std::memory_order_release);
    originalAudioAudible_.store(false, std::memory_order_release);
}

VideoPlayer::~VideoPlayer()
{
    worker_.request_stop();
    overrideAudioWorker_.request_stop();
    wakeCondition_.notify_all();
    overrideAudioCondition_.notify_all();
}

void VideoPlayer::OnNativeVideoOpened(const std::uint32_t width,
    const std::uint32_t height,
    std::filesystem::path selectedVideo)
{
    {
        std::scoped_lock lock(wakeMutex_);
        selectedVideo_ = std::move(selectedVideo);
    }
    outputWidth_.store(width, std::memory_order_release);
    outputHeight_.store(height, std::memory_order_release);
    nativeVideoActive_.store(true, std::memory_order_release);
    session_.fetch_add(1, std::memory_order_release);
    wakeCondition_.notify_all();
}

void VideoPlayer::OnNativeVideoClosed()
{
    {
        std::scoped_lock lock(wakeMutex_);
        selectedVideo_.reset();
    }
    nativeVideoActive_.store(false, std::memory_order_release);
    outputWidth_.store(0, std::memory_order_release);
    outputHeight_.store(0, std::memory_order_release);
    session_.fetch_add(1, std::memory_order_release);
    wakeCondition_.notify_all();
}

void VideoPlayer::StartOverrideAudio(const std::filesystem::path& path)
{
    {
        std::scoped_lock lock(overrideAudioMutex_);
        overrideAudioPath_ = path;
    }
    overrideAudioActive_.store(true, std::memory_order_release);
    overrideAudioSession_.fetch_add(1, std::memory_order_release);
    overrideAudioCondition_.notify_all();
}

void VideoPlayer::StopOverrideAudio()
{
    {
        std::scoped_lock lock(overrideAudioMutex_);
        overrideAudioPath_.reset();
    }
    overrideAudioActive_.store(false, std::memory_order_release);
    overrideAudioSession_.fetch_add(1, std::memory_order_release);
    overrideAudioCondition_.notify_all();
}

std::optional<std::filesystem::path> VideoPlayer::PickDedicatedAudio()
{
    const auto directory = Config::MainMenuAudioDirectory();
    auto sources = MainMenuMedia::ScanAudioSources(
        directory, Config::RecursiveMediaScan());
    if (sources.empty()) {
        spdlog::warn(
            "No supported dedicated main-menu audio sources were found "
            "in {}",
            Utf8Path(directory));
        return std::nullopt;
    }

    {
        std::scoped_lock lock(audioSelectionMutex_);
        std::ranges::shuffle(sources, audioRandom_);
        if (sources.size() > 1 && sources.front() == previousAudio_) {
            std::swap(sources.front(), sources[1]);
        }
    }
    for (const auto& source : sources) {
        if (!HasDecodableAudioTrack(source)) {
            spdlog::warn("Ignoring dedicated audio source with no usable audio "
                         "track: {}",
                Utf8Path(source));
            continue;
        }
        {
            std::scoped_lock lock(audioSelectionMutex_);
            previousAudio_ = source;
        }
        spdlog::info(
            "Selected dedicated main-menu audio: {}", Utf8Path(source));
        return source;
    }
    spdlog::warn(
        "No decodable dedicated main-menu audio sources were found in {}",
        Utf8Path(directory));
    return std::nullopt;
}

std::optional<std::filesystem::path> VideoPlayer::PickDedicatedAudioForVideo(
    const std::filesystem::path& video,
    const bool allowRandomFallback)
{
    const auto directory = Config::MainMenuAudioDirectory();
    auto sources = MainMenuMedia::ScanAudioSources(
        directory, Config::RecursiveMediaScan());
    if (sources.empty()) {
        if (allowRandomFallback) {
            spdlog::warn(
                "No supported dedicated main-menu audio sources were found "
                "in {}",
                Utf8Path(directory));
        }
        return std::nullopt;
    }

    const std::wstring videoStem = video.stem().wstring();
    for (const auto& source : sources) {
        const bool matching = [&] {
            const std::wstring sourceStem = source.stem().wstring();
            return std::ranges::equal(videoStem,
                sourceStem,
                [](const wchar_t left, const wchar_t right) {
                    return std::towlower(left) == std::towlower(right);
                });
        }();
        if (!matching) {
            continue;
        }
        if (!HasDecodableAudioTrack(source)) {
            spdlog::warn("Ignoring same-name audio source with no usable audio "
                         "track: {}",
                Utf8Path(source));
            continue;
        }
        std::scoped_lock lock(audioSelectionMutex_);
        previousAudio_ = source;
        spdlog::info("Selected same-name main-menu audio for {}: {}",
            Utf8Path(video.filename()),
            Utf8Path(previousAudio_));
        return previousAudio_;
    }

    if (!allowRandomFallback) {
        return std::nullopt;
    }

    {
        std::scoped_lock lock(audioSelectionMutex_);
        std::ranges::shuffle(sources, audioRandom_);
        if (sources.size() > 1 && sources.front() == previousAudio_) {
            std::swap(sources.front(), sources[1]);
        }
    }
    for (const auto& source : sources) {
        if (!HasDecodableAudioTrack(source)) {
            continue;
        }
        {
            std::scoped_lock lock(audioSelectionMutex_);
            previousAudio_ = source;
        }
        spdlog::info("Selected random main-menu audio for silent video {}: {}",
            Utf8Path(video.filename()),
            Utf8Path(source));
        return source;
    }
    spdlog::warn("No decodable dedicated soundtrack was available for {}",
        Utf8Path(video.filename()));
    return std::nullopt;
}

bool VideoPlayer::HasDecodableAudioTrack(
    const std::filesystem::path& path) const
{
    struct ProbeResult
    {
        std::filesystem::file_time_type modified;
        std::uintmax_t size{ 0 };
        bool decodable{ false };
    };
    static std::mutex cacheMutex;
    static std::unordered_map<std::string, ProbeResult> cache;

    AVFormatContext* format = nullptr;
    const std::string nativePath = Utf8Path(path);
    std::error_code fileError;
    const auto modified = std::filesystem::last_write_time(path, fileError);
    const bool hasModifiedTime = !fileError;
    fileError.clear();
    const auto size = std::filesystem::file_size(path, fileError);
    const bool metadataValid = hasModifiedTime && !fileError;
    if (metadataValid) {
        std::scoped_lock lock(cacheMutex);
        const auto found = cache.find(nativePath);
        if (found != cache.end() && found->second.modified == modified &&
            found->second.size == size) {
            return found->second.decodable;
        }
    }

    const int opened =
        avformat_open_input(&format, nativePath.c_str(), nullptr, nullptr);
    if (opened < 0 || !format) {
        avformat_close_input(&format);
        if (metadataValid) {
            std::scoped_lock lock(cacheMutex);
            cache[nativePath] = { modified, size, false };
        }
        return false;
    }

    if (avformat_find_stream_info(format, nullptr) < 0) {
        avformat_close_input(&format);
        if (metadataValid) {
            std::scoped_lock lock(cacheMutex);
            cache[nativePath] = { modified, size, false };
        }
        return false;
    }
    bool decodable = false;
    for (unsigned int index = 0; index < format->nb_streams; ++index) {
        const AVStream* stream = format->streams[index];
        if (!stream || !stream->codecpar ||
            stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
            continue;
        }
        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        AVCodecContext* decoder =
            codec ? avcodec_alloc_context3(codec) : nullptr;
        if (!decoder) {
            continue;
        }
        int result = avcodec_parameters_to_context(decoder, stream->codecpar);
        if (result >= 0) {
            result = avcodec_open2(decoder, codec, nullptr);
        }
        const bool usable = result >= 0 && decoder->sample_rate > 0 &&
                            decoder->channels > 0;
        avcodec_free_context(&decoder);
        if (usable) {
            decodable = true;
            break;
        }
    }
    avformat_close_input(&format);
    if (metadataValid) {
        std::scoped_lock lock(cacheMutex);
        cache[nativePath] = { modified, size, decodable };
    }
    return decodable;
}

void VideoPlayer::SetOriginalAudioPreferred(const bool enabled) noexcept
{
    originalAudioPreferred_.store(enabled, std::memory_order_release);
}

bool VideoPlayer::OriginalAudioPreferred() const noexcept
{
    return originalAudioPreferred_.load(std::memory_order_acquire);
}

void VideoPlayer::SetOriginalAudioAudible(const bool enabled) noexcept
{
    originalAudioAudible_.store(enabled, std::memory_order_release);
}

bool VideoPlayer::OriginalAudioAudible() const noexcept
{
    return originalAudioAudible_.load(std::memory_order_acquire);
}

void VideoPlayer::AdjustVolume(const float delta)
{
    float current = volume_.load(std::memory_order_acquire);
    while (!volume_.compare_exchange_weak(current,
        std::clamp(current + delta, 0.0F, 2.0F),
        std::memory_order_acq_rel)) {
    }
    spdlog::info("Main-menu audio volume set to {:.0f}%", Volume() * 100.0F);
}

float VideoPlayer::Volume() const noexcept
{
    return volume_.load(std::memory_order_acquire);
}

std::shared_ptr<const VideoFrame> VideoPlayer::GetLatestFrame() const
{
    return latestFrame_.load(std::memory_order_acquire);
}

void VideoPlayer::Worker(std::stop_token stopToken)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    std::uint64_t handledSession = 0;

    while (!stopToken.stop_requested()) {
        {
            std::unique_lock lock(wakeMutex_);
            wakeCondition_.wait(lock, stopToken, [&] {
                return session_.load(std::memory_order_acquire) !=
                       handledSession;
            });
        }
        if (stopToken.stop_requested()) {
            return;
        }

        handledSession = session_.load(std::memory_order_acquire);
        latestFrame_.store({}, std::memory_order_release);
        if (!nativeVideoActive_.load(std::memory_order_acquire)) {
            continue;
        }

        std::filesystem::path selected;
        {
            std::scoped_lock lock(wakeMutex_);
            if (selectedVideo_) {
                selected = std::move(*selectedVideo_);
            }
            selectedVideo_.reset();
        }
        if (selected.empty()) {
            continue;
        }
        spdlog::info("Main-menu session {} selected video: {}",
            handledSession,
            Utf8Path(selected));
        DecodeSession(selected, handledSession, stopToken);
        latestFrame_.store({}, std::memory_order_release);
        for (auto& frame : framePool_) {
            frame.reset();
        }
    }
}

bool VideoPlayer::SessionActive(const std::uint64_t session) const
{
    return nativeVideoActive_.load(std::memory_order_acquire) &&
           session_.load(std::memory_order_acquire) == session;
}

bool VideoPlayer::OverrideAudioSessionActive(const std::uint64_t session) const
{
    return overrideAudioActive_.load(std::memory_order_acquire) &&
           overrideAudioSession_.load(std::memory_order_acquire) == session;
}

void VideoPlayer::OverrideAudioWorker(std::stop_token stopToken)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    std::uint64_t handledSession = 0;

    while (!stopToken.stop_requested()) {
        {
            std::unique_lock lock(overrideAudioMutex_);
            overrideAudioCondition_.wait(lock, stopToken, [&] {
                return overrideAudioSession_.load(std::memory_order_acquire) !=
                       handledSession;
            });
        }
        if (stopToken.stop_requested()) {
            return;
        }

        handledSession = overrideAudioSession_.load(std::memory_order_acquire);
        if (!overrideAudioActive_.load(std::memory_order_acquire)) {
            continue;
        }

        std::optional<std::filesystem::path> path;
        {
            std::scoped_lock lock(overrideAudioMutex_);
            path = overrideAudioPath_;
        }
        if (!path) {
            continue;
        }

        spdlog::info("Starting main-menu override audio: {}", Utf8Path(*path));
        const bool started =
            DecodeAudioSession(*path, handledSession, stopToken, true);
        if (!started && OverrideAudioSessionActive(handledSession)) {
            overrideAudioActive_.store(false, std::memory_order_release);
            SetOriginalAudioPreferred(true);
            SetOriginalAudioAudible(true);
            spdlog::warn("Override audio failed; restored the selected video's "
                         "audio");
        }
    }
}

bool VideoPlayer::DecodeAudioSession(const std::filesystem::path& path,
    const std::uint64_t session,
    std::stop_token stopToken,
    const bool overrideAudio)
{
    constexpr int kOutputSampleRate{ 48000 };
    constexpr int kOutputChannels{ 2 };
    constexpr AVSampleFormat kOutputFormat{ AV_SAMPLE_FMT_S16 };

    const auto sessionActive = [&] {
        return overrideAudio ? OverrideAudioSessionActive(session)
                             : SessionActive(session);
    };

    while (!overrideAudio && !stopToken.stop_requested() && sessionActive() &&
           !GetLatestFrame()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (stopToken.stop_requested() || !sessionActive()) {
        return false;
    }

    AVFormatContext* format = nullptr;
    AVCodecContext* decoder = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* decoded = nullptr;
    SwrContext* resampler = nullptr;
    AudioOutput output;

    const auto cleanUp = [&] {
        output.Reset();
        swr_free(&resampler);
        av_frame_free(&decoded);
        av_packet_free(&packet);
        avcodec_free_context(&decoder);
        avformat_close_input(&format);
    };

    const std::string nativePath = Utf8Path(path);
    int result =
        avformat_open_input(&format, nativePath.c_str(), nullptr, nullptr);
    if (result < 0) {
        spdlog::error(
            "Audio decoder could not open {}: {}", nativePath, AvError(result));
        cleanUp();
        return false;
    }

    int audioStream = -1;
    const AVCodec* codec = nullptr;
    const auto openFirstDecodableAudioStream = [&] {
        for (unsigned int index = 0; index < format->nb_streams; ++index) {
            AVStream* stream = format->streams[index];
            if (!stream || !stream->codecpar ||
                stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
                continue;
            }

            const AVCodec* candidate =
                avcodec_find_decoder(stream->codecpar->codec_id);
            AVCodecContext* candidateDecoder =
                candidate ? avcodec_alloc_context3(candidate) : nullptr;
            if (!candidateDecoder) {
                continue;
            }
            int candidateResult = avcodec_parameters_to_context(
                candidateDecoder, stream->codecpar);
            if (candidateResult >= 0) {
                candidateResult =
                    avcodec_open2(candidateDecoder, candidate, nullptr);
            }
            if (candidateResult >= 0 && candidateDecoder->sample_rate > 0 &&
                candidateDecoder->channels > 0) {
                audioStream = static_cast<int>(index);
                codec = candidate;
                decoder = candidateDecoder;
                return true;
            }
            avcodec_free_context(&candidateDecoder);
        }
        return false;
    };

    if (!openFirstDecodableAudioStream()) {
        result = avformat_find_stream_info(format, nullptr);
        if (result < 0 || !openFirstDecodableAudioStream()) {
            spdlog::info(
                "Selected main-menu media has no decodable audio stream");
            cleanUp();
            return false;
        }
    }

    const std::int64_t inputLayout =
        decoder->channel_layout != 0
            ? static_cast<std::int64_t>(decoder->channel_layout)
            : av_get_default_channel_layout(decoder->channels);
    resampler = swr_alloc_set_opts(nullptr,
        AV_CH_LAYOUT_STEREO,
        kOutputFormat,
        kOutputSampleRate,
        inputLayout,
        decoder->sample_fmt,
        decoder->sample_rate,
        0,
        nullptr);
    if (!resampler || swr_init(resampler) < 0) {
        spdlog::error("FFmpeg could not initialize audio resampling");
        cleanUp();
        return false;
    }

    packet = av_packet_alloc();
    decoded = av_frame_alloc();
    if (!packet || !decoded) {
        spdlog::error("FFmpeg could not allocate audio decoding resources");
        cleanUp();
        return false;
    }
    if (!output.Initialize(kOutputSampleRate, kOutputChannels)) {
        cleanUp();
        return false;
    }
    output.SetVolume(overrideAudio || OriginalAudioAudible() ? Volume() : 0.0F);

    spdlog::info("Playing audio stream {} with {} decoder: {} Hz, {} channels",
        audioStream,
        codec->name,
        decoder->sample_rate,
        decoder->channels);

    AVStream* stream = format->streams[audioStream];
    bool playbackFailed = false;
    while (!stopToken.stop_requested() && sessionActive() && !playbackFailed) {
        if (!PlaybackGate::MayAdvance()) {
            output.Pause();
            while (!stopToken.stop_requested() && sessionActive() &&
                   !PlaybackGate::MayAdvance()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            output.Resume();
            if (stopToken.stop_requested() || !sessionActive()) {
                break;
            }
        }

        output.SetVolume(
            overrideAudio || OriginalAudioAudible() ? Volume() : 0.0F);
        result = av_read_frame(format, packet);
        if (result < 0) {
            const std::int64_t seekTarget =
                stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
            if (av_seek_frame(
                    format, audioStream, seekTarget, AVSEEK_FLAG_BACKWARD) <
                0) {
                spdlog::warn("Audio reached EOF and could not seek to loop");
                playbackFailed = true;
                break;
            }
            avcodec_flush_buffers(decoder);
            swr_close(resampler);
            if (swr_init(resampler) < 0) {
                spdlog::warn("Audio resampler could not restart for looping");
                playbackFailed = true;
                break;
            }
            continue;
        }

        if (packet->stream_index != audioStream) {
            av_packet_unref(packet);
            continue;
        }

        result = avcodec_send_packet(decoder, packet);
        av_packet_unref(packet);
        if (result < 0) {
            continue;
        }

        while (
            !playbackFailed && avcodec_receive_frame(decoder, decoded) == 0) {
            if (stopToken.stop_requested() || !sessionActive()) {
                break;
            }
            if (!PlaybackGate::MayAdvance()) {
                output.Pause();
                while (!stopToken.stop_requested() && sessionActive() &&
                       !PlaybackGate::MayAdvance()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                output.Resume();
                if (stopToken.stop_requested() || !sessionActive()) {
                    break;
                }
            }

            const std::int64_t delayedSamples =
                swr_get_delay(resampler, decoder->sample_rate);
            const int outputCapacity = static_cast<int>(
                av_rescale_rnd(delayedSamples + decoded->nb_samples,
                    kOutputSampleRate,
                    decoder->sample_rate,
                    AV_ROUND_UP));
            constexpr int kMaximumOutputSamples = kOutputSampleRate * 10;
            if (outputCapacity <= 0 || outputCapacity > kMaximumOutputSamples) {
                if (outputCapacity > kMaximumOutputSamples) {
                    spdlog::error(
                        "Rejected an implausibly large decoded audio frame "
                        "({} samples)",
                        outputCapacity);
                    playbackFailed = true;
                }
                av_frame_unref(decoded);
                continue;
            }

            std::vector<std::uint8_t> samples;
            try {
                samples.resize(static_cast<std::size_t>(outputCapacity) *
                               kOutputChannels * sizeof(std::int16_t));
            } catch (const std::bad_alloc&) {
                spdlog::error(
                    "Could not allocate a decoded main-menu audio buffer");
                av_frame_unref(decoded);
                playbackFailed = true;
                break;
            }
            std::uint8_t* outputPlanes[]{ samples.data() };
            const int converted = swr_convert(resampler,
                outputPlanes,
                outputCapacity,
                const_cast<const std::uint8_t**>(decoded->extended_data),
                decoded->nb_samples);
            av_frame_unref(decoded);
            if (converted <= 0) {
                continue;
            }

            samples.resize(static_cast<std::size_t>(converted) *
                           kOutputChannels * sizeof(std::int16_t));
            if (!output.Submit(std::move(samples))) {
                spdlog::warn("XAudio2 rejected an audio buffer");
                playbackFailed = true;
                break;
            }
        }
    }

    spdlog::info("Stopped audio for main-menu session {}", session);
    cleanUp();
    return !playbackFailed;
}

bool VideoPlayer::DecodeSession(const std::filesystem::path& path,
    const std::uint64_t session,
    std::stop_token stopToken)
{
    AVFormatContext* format = nullptr;
    AVCodecContext* decoder = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* decoded = nullptr;
    SwsContext* converter = nullptr;

    const auto cleanUp = [&] {
        sws_freeContext(converter);
        av_frame_free(&decoded);
        av_packet_free(&packet);
        avcodec_free_context(&decoder);
        avformat_close_input(&format);
    };

    const std::string nativePath = Utf8Path(path);
    int result =
        avformat_open_input(&format, nativePath.c_str(), nullptr, nullptr);
    if (result < 0) {
        spdlog::error(
            "FFmpeg could not open {}: {}", nativePath, AvError(result));
        cleanUp();
        return false;
    }

    // MP4 and Matroska expose their track list during avformat_open_input.
    // avformat_find_stream_info would decode packets using FFmpeg's default
    // AV1 decoder before we can select libaom, which can stall game startup.
    const int videoStream = FindVideoStream(format);
    const AVCodec* codec =
        videoStream >= 0 ? FindDecoder(format->streams[videoStream]->codecpar)
                         : nullptr;
    if (videoStream < 0 || !codec) {
        spdlog::error(
            "FFmpeg found no decodable video stream in {}", nativePath);
        cleanUp();
        return false;
    }

    spdlog::info(
        "Opening {} decoder for video stream {}", codec->name, videoStream);

    decoder = avcodec_alloc_context3(codec);
    if (!decoder) {
        spdlog::error("FFmpeg could not allocate a video decoder");
        cleanUp();
        return false;
    }

    result = avcodec_parameters_to_context(
        decoder, format->streams[videoStream]->codecpar);
    if (result >= 0) {
        decoder->thread_count = 4;
        decoder->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
        result = avcodec_open2(decoder, codec, nullptr);
    }
    if (result < 0 || decoder->width <= 0 || decoder->height <= 0) {
        spdlog::error("FFmpeg could not initialize decoder {}: {}",
            codec->name,
            AvError(result));
        cleanUp();
        return false;
    }

    packet = av_packet_alloc();
    decoded = av_frame_alloc();
    if (!packet || !decoded) {
        spdlog::error("FFmpeg could not allocate decoding resources");
        cleanUp();
        return false;
    }

    AVStream* stream = format->streams[videoStream];
    const AVRational guessedRate = av_guess_frame_rate(format, stream, nullptr);
    const double framesPerSecond =
        guessedRate.num > 0 && guessedRate.den > 0 ? av_q2d(guessedRate) : 30.0;
    spdlog::info("Opened {} decoder: {}x{}, {:.3f} FPS",
        codec->name,
        decoder->width,
        decoder->height,
        framesPerSecond);

    const std::uint32_t outputWidth =
        outputWidth_.load(std::memory_order_acquire);
    const std::uint32_t outputHeight =
        outputHeight_.load(std::memory_order_acquire);
    if (outputWidth == 0 || outputHeight == 0) {
        spdlog::error("Native main-menu output dimensions are unavailable");
        cleanUp();
        return false;
    }

    std::jthread audioWorker(
        [this, path, session](std::stop_token audioStopToken) {
            (void)DecodeAudioSession(path, session, audioStopToken, false);
        });

    std::int64_t firstTimestamp = AV_NOPTS_VALUE;
    std::uint64_t fallbackFrame = 0;
    auto playbackStart = std::chrono::steady_clock::now();
    int converterWidth = 0;
    int converterHeight = 0;
    AVPixelFormat converterPixelFormat = AV_PIX_FMT_NONE;

    while (!stopToken.stop_requested() && SessionActive(session)) {
        if (!PlaybackGate::MayAdvance()) {
            const auto pauseStart = std::chrono::steady_clock::now();
            while (!stopToken.stop_requested() && SessionActive(session) &&
                   !PlaybackGate::MayAdvance()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            playbackStart += std::chrono::steady_clock::now() - pauseStart;
            if (stopToken.stop_requested() || !SessionActive(session)) {
                break;
            }
        }

        result = av_read_frame(format, packet);
        if (result < 0) {
            const std::int64_t seekTarget =
                stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
            if (av_seek_frame(
                    format, videoStream, seekTarget, AVSEEK_FLAG_BACKWARD) <
                0) {
                spdlog::warn("Video reached EOF and could not seek to loop");
                break;
            }
            avcodec_flush_buffers(decoder);
            firstTimestamp = AV_NOPTS_VALUE;
            fallbackFrame = 0;
            playbackStart = std::chrono::steady_clock::now();
            continue;
        }

        if (packet->stream_index != videoStream) {
            av_packet_unref(packet);
            continue;
        }

        result = avcodec_send_packet(decoder, packet);
        av_packet_unref(packet);
        if (result < 0) {
            continue;
        }

        while (avcodec_receive_frame(decoder, decoded) == 0) {
            if (stopToken.stop_requested() || !SessionActive(session)) {
                break;
            }

            const int decodedWidth = decoded->width;
            const int decodedHeight = decoded->height;
            if (!VideoScaling::ApplyCenterCrop(
                    decoded, outputWidth, outputHeight)) {
                spdlog::error("FFmpeg could not center-crop a decoded frame");
                cleanUp();
                return false;
            }

            const auto pixelFormat =
                static_cast<AVPixelFormat>(decoded->format);
            if (converter && (converterWidth != decoded->width ||
                                 converterHeight != decoded->height ||
                                 converterPixelFormat != pixelFormat)) {
                sws_freeContext(converter);
                converter = nullptr;
                spdlog::info(
                    "Decoded video format changed; rebuilding the scaler");
            }
            if (!converter) {
                converter = sws_getContext(decoded->width,
                    decoded->height,
                    pixelFormat,
                    static_cast<int>(outputWidth),
                    static_cast<int>(outputHeight),
                    AV_PIX_FMT_BGRA,
                    SWS_BILINEAR,
                    nullptr,
                    nullptr,
                    nullptr);
                if (!converter) {
                    spdlog::error(
                        "FFmpeg could not convert decoded pixel format {}",
                        av_get_pix_fmt_name(pixelFormat)
                            ? av_get_pix_fmt_name(pixelFormat)
                            : "unknown");
                    cleanUp();
                    return false;
                }
                converterWidth = decoded->width;
                converterHeight = decoded->height;
                converterPixelFormat = pixelFormat;
                spdlog::info("Preparing decoded frame: {}x{}, pixel format {}; "
                             "scaling to {}x{}",
                    decodedWidth,
                    decodedHeight,
                    av_get_pix_fmt_name(pixelFormat)
                        ? av_get_pix_fmt_name(pixelFormat)
                        : "unknown",
                    outputWidth,
                    outputHeight);
            }

            const std::int64_t timestamp = decoded->best_effort_timestamp;
            if (firstTimestamp == AV_NOPTS_VALUE &&
                timestamp != AV_NOPTS_VALUE) {
                firstTimestamp = timestamp;
            }

            double presentationSeconds =
                static_cast<double>(fallbackFrame++) / framesPerSecond;
            if (timestamp != AV_NOPTS_VALUE &&
                firstTimestamp != AV_NOPTS_VALUE) {
                presentationSeconds =
                    static_cast<double>(timestamp - firstTimestamp) *
                    av_q2d(stream->time_base);
            }

            const auto due =
                playbackStart +
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(
                        std::max(0.0, presentationSeconds)));
            while (!stopToken.stop_requested() && SessionActive(session) &&
                   std::chrono::steady_clock::now() < due) {
                if (!PlaybackGate::MayAdvance()) {
                    const auto pauseStart = std::chrono::steady_clock::now();
                    while (!stopToken.stop_requested() &&
                           SessionActive(session) &&
                           !PlaybackGate::MayAdvance()) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(10));
                    }
                    playbackStart +=
                        std::chrono::steady_clock::now() - pauseStart;
                    break;
                }
                const auto remaining = due - std::chrono::steady_clock::now();
                std::this_thread::sleep_for(std::min(remaining,
                    std::chrono::steady_clock::duration(
                        std::chrono::milliseconds(5))));
            }
            if (stopToken.stop_requested() || !SessionActive(session)) {
                break;
            }

            std::shared_ptr<VideoFrame> frame;
            for (auto& candidate : framePool_) {
                if (!candidate) {
                    try {
                        candidate = std::make_shared<VideoFrame>();
                    } catch (const std::bad_alloc&) {
                        break;
                    }
                }
                if (candidate.use_count() == 1) {
                    frame = candidate;
                    break;
                }
            }
            if (!frame) {
                av_frame_unref(decoded);
                continue;
            }
            frame->width = outputWidth;
            frame->height = outputHeight;
            frame->rowPitch = frame->width * 4;
            try {
                frame->pixels.resize(
                    static_cast<std::size_t>(frame->rowPitch) * frame->height);
            } catch (const std::bad_alloc&) {
                spdlog::error(
                    "Could not allocate a decoded main-menu video frame");
                av_frame_unref(decoded);
                cleanUp();
                return false;
            }
            std::uint8_t* outputPlanes[4]{
                frame->pixels.data(), nullptr, nullptr, nullptr
            };
            int outputStrides[4]{ static_cast<int>(frame->rowPitch), 0, 0, 0 };
            const int scaled = sws_scale(converter,
                decoded->data,
                decoded->linesize,
                0,
                decoded->height,
                outputPlanes,
                outputStrides);
            if (scaled <= 0) {
                spdlog::warn("FFmpeg could not scale a decoded video frame");
                av_frame_unref(decoded);
                continue;
            }
            frame->serial = nextFrameSerial_++;
            latestFrame_.store(std::move(frame), std::memory_order_release);
            av_frame_unref(decoded);
        }
    }

    spdlog::info("Stopped decoding main-menu session {}", session);
    cleanUp();
    return true;
}
