#include "PCH.h"

#include "AudioOutput.h"
#include "Config.h"
#include "EngineSettings.h"
#include "MediaLibrary.h"
#include "VideoPlayer.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace
{
    std::string Utf8Path(const std::filesystem::path& path)
    {
        const auto utf8 = path.u8string();
        return {
            reinterpret_cast<const char*>(utf8.data()),
            utf8.size()
        };
    }

    std::string AvError(const int code)
    {
        std::array<char, AV_ERROR_MAX_STRING_SIZE> message{};
        av_strerror(code, message.data(), message.size());
        return message.data();
    }

    int FindVideoStream(const AVFormatContext* format)
    {
        for (unsigned int index = 0; index < format->nb_streams; ++index) {
            const AVStream* stream = format->streams[index];
            if (stream && stream->codecpar &&
                stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                return static_cast<int>(index);
            }
        }
        return -1;
    }

    int FindAudioStream(const AVFormatContext* format)
    {
        for (unsigned int index = 0; index < format->nb_streams; ++index) {
            const AVStream* stream = format->streams[index];
            if (stream && stream->codecpar &&
                stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                return static_cast<int>(index);
            }
        }
        return -1;
    }

    AVCodec* FindDecoder(const AVCodecParameters* parameters)
    {
        // FFmpeg 4.4's native AV1 decoder fails on some 10-bit streams under
        // Wine. Use the external libaom decoder for AV1 instead.
        if (parameters->codec_id == AV_CODEC_ID_AV1) {
            if (AVCodec* aom = avcodec_find_decoder_by_name("libaom-av1")) {
                return aom;
            }
        }
        return avcodec_find_decoder(parameters->codec_id);
    }

    HWND FindGameWindow()
    {
        struct Search
        {
            DWORD processId;
            HWND window;
        } search{ GetCurrentProcessId(), nullptr };

        EnumWindows(
            [](const HWND window, const LPARAM parameter) -> BOOL {
                auto& candidate = *reinterpret_cast<Search*>(parameter);
                DWORD processId = 0;
                GetWindowThreadProcessId(window, &processId);
                if (processId == candidate.processId &&
                    IsWindowVisible(window) &&
                    GetWindow(window, GW_OWNER) == nullptr) {
                    RECT rect{};
                    RECT previous{};
                    GetWindowRect(window, &rect);
                    GetWindowRect(candidate.window, &previous);
                    const auto area = static_cast<std::int64_t>(
                        rect.right - rect.left) *
                        (rect.bottom - rect.top);
                    const auto previousArea = static_cast<std::int64_t>(
                        previous.right - previous.left) *
                        (previous.bottom - previous.top);
                    if (area > previousArea) {
                        candidate.window = window;
                    }
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&search));
        return search.window;
    }

    HWND GameWindow()
    {
        static std::atomic<HWND> cachedWindow{ nullptr };
        HWND window = cachedWindow.load(std::memory_order_acquire);
        if (!window || !IsWindow(window)) {
            window = FindGameWindow();
            cachedWindow.store(window, std::memory_order_release);
        }
        return window;
    }

    bool IsBorderlessFullscreen(const HWND window)
    {
        if (!window || IsIconic(window)) {
            return false;
        }

        RECT windowRect{};
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(MONITORINFO);
        const HMONITOR monitor = MonitorFromWindow(
            window,
            MONITOR_DEFAULTTONEAREST);
        if (!GetWindowRect(window, &windowRect) ||
            !GetMonitorInfoW(monitor, &monitorInfo)) {
            return false;
        }

        constexpr LONG kEdgeTolerance{ 2 };
        return windowRect.left <=
                   monitorInfo.rcMonitor.left + kEdgeTolerance &&
               windowRect.top <=
                   monitorInfo.rcMonitor.top + kEdgeTolerance &&
               windowRect.right >=
                   monitorInfo.rcMonitor.right - kEdgeTolerance &&
               windowRect.bottom >=
                   monitorInfo.rcMonitor.bottom - kEdgeTolerance;
    }

    bool PlaybackMayAdvance()
    {
        enum class State
        {
            kForeground,
            kBorderlessBackground,
            kPaused
        };
        static std::atomic<State> previousState{ State::kForeground };

        const HWND foreground = GetForegroundWindow();
        if (foreground) {
            DWORD processId = 0;
            GetWindowThreadProcessId(foreground, &processId);
            if (processId == GetCurrentProcessId()) {
                const State previous = previousState.exchange(
                    State::kForeground,
                    std::memory_order_relaxed);
                if (previous != State::kForeground) {
                    spdlog::info("Fallout regained focus; playback active");
                }
                return true;
            }
        }

        const bool keepPlaying =
            Config::KeepPlayingWhenBorderless() &&
            (EngineSettings::IsBorderlessMode() ||
             IsBorderlessFullscreen(GameWindow()));
        const State state = keepPlaying ?
            State::kBorderlessBackground :
            State::kPaused;
        const State previous = previousState.exchange(
            state,
            std::memory_order_relaxed);
        if (previous != state) {
            if (keepPlaying) {
                spdlog::info(
                    "Fallout lost focus in borderless mode; "
                    "playback remains active");
            } else {
                spdlog::info(
                    "Fallout lost focus outside borderless mode; "
                    "playback paused");
            }
        }
        return keepPlaying;
    }

    bool ApplyCenterCrop(
        AVFrame* frame,
        const std::uint32_t outputWidth,
        const std::uint32_t outputHeight)
    {
        if (!frame ||
            frame->width <= 0 ||
            frame->height <= 0 ||
            outputWidth == 0 ||
            outputHeight == 0) {
            return false;
        }

        const std::uint64_t inputAspect =
            static_cast<std::uint64_t>(frame->width) * outputHeight;
        const std::uint64_t outputAspect =
            static_cast<std::uint64_t>(outputWidth) * frame->height;
        if (inputAspect > outputAspect) {
            const std::size_t croppedWidth =
                static_cast<std::size_t>(
                    static_cast<std::uint64_t>(frame->height) *
                    outputWidth / outputHeight);
            const std::size_t removed =
                static_cast<std::size_t>(frame->width) - croppedWidth;
            frame->crop_left = removed / 2;
            frame->crop_right = removed - frame->crop_left;
        } else if (inputAspect < outputAspect) {
            const std::size_t croppedHeight =
                static_cast<std::size_t>(
                    static_cast<std::uint64_t>(frame->width) *
                    outputHeight / outputWidth);
            const std::size_t removed =
                static_cast<std::size_t>(frame->height) - croppedHeight;
            frame->crop_top = removed / 2;
            frame->crop_bottom = removed - frame->crop_top;
        }

        return av_frame_apply_cropping(frame, 0) >= 0;
    }
}

VideoPlayer& VideoPlayer::GetSingleton()
{
    static VideoPlayer instance;
    return instance;
}

VideoPlayer::VideoPlayer() :
    random_(std::random_device{}()),
    worker_([this](std::stop_token stopToken) {
        Worker(stopToken);
    }),
    sidecarWorker_([this](std::stop_token stopToken) {
        SidecarWorker(stopToken);
    })
{
    volume_.store(Config::MainMenuVolume(), std::memory_order_release);
}

VideoPlayer::~VideoPlayer()
{
    worker_.request_stop();
    sidecarWorker_.request_stop();
    wakeCondition_.notify_all();
    sidecarCondition_.notify_all();
}

void VideoPlayer::OnNativeVideoOpened(
    const std::uint32_t width,
    const std::uint32_t height,
    std::optional<std::filesystem::path> selectedVideo)
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

void VideoPlayer::StartSidecarAudio(
    const std::filesystem::path& path)
{
    {
        std::scoped_lock lock(sidecarMutex_);
        sidecarPath_ = path;
    }
    sidecarActive_.store(true, std::memory_order_release);
    sidecarSession_.fetch_add(1, std::memory_order_release);
    sidecarCondition_.notify_all();
}

void VideoPlayer::StopSidecarAudio()
{
    {
        std::scoped_lock lock(sidecarMutex_);
        sidecarPath_.reset();
    }
    sidecarActive_.store(false, std::memory_order_release);
    sidecarSession_.fetch_add(1, std::memory_order_release);
    sidecarCondition_.notify_all();
}

void VideoPlayer::AdjustVolume(const float delta)
{
    float current = volume_.load(std::memory_order_acquire);
    while (!volume_.compare_exchange_weak(
        current,
        std::clamp(current + delta, 0.0F, 2.0F),
        std::memory_order_acq_rel)) {}
    spdlog::info(
        "Main-menu audio volume set to {:.0f}%",
        Volume() * 100.0F);
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

        std::optional<std::filesystem::path> selected;
        {
            std::scoped_lock lock(wakeMutex_);
            selected = std::move(selectedVideo_);
            selectedVideo_.reset();
        }
        if (!selected) {
            auto videos = FindVideos();
            if (videos.empty()) {
                spdlog::error(
                    "No supported videos found in Data\\MainMenuVideos");
                continue;
            }
            selected = PickVideo(std::move(videos));
        }
        spdlog::info(
            "Main-menu session {} selected video: {}",
            handledSession,
            Utf8Path(*selected));
        DecodeSession(*selected, handledSession, stopToken);
        latestFrame_.store({}, std::memory_order_release);
    }
}

std::vector<std::filesystem::path> VideoPlayer::FindVideos() const
{
    const auto directory = Config::MainMenuDirectory();
    const MediaLibrary library(
        directory,
        Config::RecursiveMediaScan());
    auto videos = library.Scan();
    if (videos.empty() &&
        directory != std::filesystem::path("Data/MainMenuVideos")) {
        const MediaLibrary legacyLibrary(
            "Data/MainMenuVideos",
            Config::RecursiveMediaScan());
        videos = legacyLibrary.Scan();
        if (!videos.empty()) {
            spdlog::info(
                "Using legacy Data\\MainMenuVideos directory for "
                "backward compatibility");
        }
    }
    spdlog::info(
        "Discovered {} main-menu video{} in {}",
        videos.size(),
        videos.size() == 1 ? "" : "s",
        Utf8Path(directory));
    return videos;
}

std::filesystem::path VideoPlayer::PickVideo(
    std::vector<std::filesystem::path> videos)
{
    std::ranges::shuffle(videos, random_);
    if (videos.size() > 1 && videos.front() == previousVideo_) {
        std::swap(videos.front(), videos[1]);
    }
    previousVideo_ = videos.front();
    return videos.front();
}

bool VideoPlayer::SessionActive(const std::uint64_t session) const
{
    return nativeVideoActive_.load(std::memory_order_acquire) &&
           session_.load(std::memory_order_acquire) == session;
}

bool VideoPlayer::SidecarSessionActive(
    const std::uint64_t session) const
{
    return sidecarActive_.load(std::memory_order_acquire) &&
           sidecarSession_.load(std::memory_order_acquire) == session;
}

void VideoPlayer::SidecarWorker(std::stop_token stopToken)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    std::uint64_t handledSession = 0;

    while (!stopToken.stop_requested()) {
        {
            std::unique_lock lock(sidecarMutex_);
            sidecarCondition_.wait(lock, stopToken, [&] {
                return sidecarSession_.load(std::memory_order_acquire) !=
                       handledSession;
            });
        }
        if (stopToken.stop_requested()) {
            return;
        }

        handledSession =
            sidecarSession_.load(std::memory_order_acquire);
        if (!sidecarActive_.load(std::memory_order_acquire)) {
            continue;
        }

        std::optional<std::filesystem::path> path;
        {
            std::scoped_lock lock(sidecarMutex_);
            path = sidecarPath_;
        }
        if (!path) {
            continue;
        }

        spdlog::info(
            "Starting main-menu XWM sidecar: {}",
            Utf8Path(*path));
        DecodeAudioSession(*path, handledSession, stopToken, true);
    }
}

void VideoPlayer::DecodeAudioSession(
    const std::filesystem::path& path,
    const std::uint64_t session,
    std::stop_token stopToken,
    const bool sidecar)
{
    constexpr int kOutputSampleRate{ 48000 };
    constexpr int kOutputChannels{ 2 };
    constexpr AVSampleFormat kOutputFormat{ AV_SAMPLE_FMT_S16 };

    const auto sessionActive = [&] {
        return sidecar ?
            SidecarSessionActive(session) :
            SessionActive(session);
    };

    while (!sidecar &&
           !stopToken.stop_requested() &&
           sessionActive() &&
           !GetLatestFrame()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (stopToken.stop_requested() || !sessionActive()) {
        return;
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
    int result = avformat_open_input(
        &format,
        nativePath.c_str(),
        nullptr,
        nullptr);
    if (result < 0) {
        spdlog::error(
            "Audio decoder could not open {}: {}",
            nativePath,
            AvError(result));
        cleanUp();
        return;
    }

    const int audioStream = FindAudioStream(format);
    AVCodec* codec = audioStream >= 0 ?
        avcodec_find_decoder(
            format->streams[audioStream]->codecpar->codec_id) :
        nullptr;
    if (audioStream < 0 || !codec) {
        spdlog::info(
            "Selected main-menu video has no decodable audio stream");
        cleanUp();
        return;
    }

    decoder = avcodec_alloc_context3(codec);
    if (!decoder) {
        spdlog::error("FFmpeg could not allocate the audio decoder");
        cleanUp();
        return;
    }
    result = avcodec_parameters_to_context(
        decoder,
        format->streams[audioStream]->codecpar);
    if (result >= 0) {
        result = avcodec_open2(decoder, codec, nullptr);
    }
    if (result < 0 ||
        decoder->sample_rate <= 0 ||
        decoder->channels <= 0) {
        spdlog::error(
            "FFmpeg could not initialize audio decoder {}: {}",
            codec->name,
            AvError(result));
        cleanUp();
        return;
    }

    const std::int64_t inputLayout =
        decoder->channel_layout != 0 ?
            static_cast<std::int64_t>(decoder->channel_layout) :
            av_get_default_channel_layout(decoder->channels);
    resampler = swr_alloc_set_opts(
        nullptr,
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
        return;
    }

    packet = av_packet_alloc();
    decoded = av_frame_alloc();
    if (!packet || !decoded) {
        spdlog::error("FFmpeg could not allocate audio decoding resources");
        cleanUp();
        return;
    }
    if (!output.Initialize(kOutputSampleRate, kOutputChannels)) {
        cleanUp();
        return;
    }
    output.SetVolume(Volume());

    spdlog::info(
        "Playing audio stream {} with {} decoder: {} Hz, {} channels",
        audioStream,
        codec->name,
        decoder->sample_rate,
        decoder->channels);

    AVStream* stream = format->streams[audioStream];
    while (!stopToken.stop_requested() && sessionActive()) {
        if (!PlaybackMayAdvance()) {
            output.Pause();
            while (!stopToken.stop_requested() &&
                   sessionActive() &&
                   !PlaybackMayAdvance()) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(10));
            }
            output.Resume();
            if (stopToken.stop_requested() || !sessionActive()) {
                break;
            }
        }

        output.SetVolume(Volume());
        result = av_read_frame(format, packet);
        if (result < 0) {
            const std::int64_t seekTarget =
                stream->start_time == AV_NOPTS_VALUE ?
                    0 :
                    stream->start_time;
            if (av_seek_frame(
                    format,
                    audioStream,
                    seekTarget,
                    AVSEEK_FLAG_BACKWARD) < 0) {
                spdlog::warn(
                    "Audio reached EOF and could not seek to loop");
                break;
            }
            avcodec_flush_buffers(decoder);
            swr_close(resampler);
            if (swr_init(resampler) < 0) {
                spdlog::warn(
                    "Audio resampler could not restart for looping");
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

        while (avcodec_receive_frame(decoder, decoded) == 0) {
            if (stopToken.stop_requested() || !sessionActive()) {
                break;
            }
            if (!PlaybackMayAdvance()) {
                output.Pause();
                while (!stopToken.stop_requested() &&
                       sessionActive() &&
                       !PlaybackMayAdvance()) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(10));
                }
                output.Resume();
                if (stopToken.stop_requested() ||
                    !sessionActive()) {
                    break;
                }
            }

            const std::int64_t delayedSamples =
                swr_get_delay(resampler, decoder->sample_rate);
            const int outputCapacity = static_cast<int>(av_rescale_rnd(
                delayedSamples + decoded->nb_samples,
                kOutputSampleRate,
                decoder->sample_rate,
                AV_ROUND_UP));
            if (outputCapacity <= 0) {
                av_frame_unref(decoded);
                continue;
            }

            std::vector<std::uint8_t> samples(
                static_cast<std::size_t>(outputCapacity) *
                kOutputChannels *
                sizeof(std::int16_t));
            std::uint8_t* outputPlanes[]{ samples.data() };
            const int converted = swr_convert(
                resampler,
                outputPlanes,
                outputCapacity,
                const_cast<const std::uint8_t**>(
                    decoded->extended_data),
                decoded->nb_samples);
            av_frame_unref(decoded);
            if (converted <= 0) {
                continue;
            }

            samples.resize(
                static_cast<std::size_t>(converted) *
                kOutputChannels *
                sizeof(std::int16_t));
            if (!output.Submit(std::move(samples))) {
                spdlog::warn("XAudio2 rejected an audio buffer");
            }
        }
    }

    spdlog::info("Stopped audio for main-menu session {}", session);
    cleanUp();
}

bool VideoPlayer::DecodeSession(
    const std::filesystem::path& path,
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
    int result = avformat_open_input(
        &format,
        nativePath.c_str(),
        nullptr,
        nullptr);
    if (result < 0) {
        spdlog::error(
            "FFmpeg could not open {}: {}",
            nativePath,
            AvError(result));
        cleanUp();
        return false;
    }

    // MP4 and Matroska expose their track list during avformat_open_input.
    // avformat_find_stream_info would decode packets using FFmpeg's default
    // AV1 decoder before we can select libaom, which can stall game startup.
    const int videoStream = FindVideoStream(format);
    AVCodec* codec = videoStream >= 0 ?
        FindDecoder(format->streams[videoStream]->codecpar) :
        nullptr;
    if (videoStream < 0 || !codec) {
        spdlog::error(
            "FFmpeg found no decodable video stream in {}",
            nativePath);
        cleanUp();
        return false;
    }

    spdlog::info(
        "Opening {} decoder for video stream {}",
        codec->name,
        videoStream);

    decoder = avcodec_alloc_context3(codec);
    if (!decoder) {
        spdlog::error("FFmpeg could not allocate a video decoder");
        cleanUp();
        return false;
    }

    result = avcodec_parameters_to_context(
        decoder,
        format->streams[videoStream]->codecpar);
    if (result >= 0) {
        decoder->thread_count = 4;
        decoder->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
        result = avcodec_open2(decoder, codec, nullptr);
    }
    if (result < 0 || decoder->width <= 0 || decoder->height <= 0) {
        spdlog::error(
            "FFmpeg could not initialize decoder {}: {}",
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
    const AVRational guessedRate = av_guess_frame_rate(
        format,
        stream,
        nullptr);
    const double framesPerSecond =
        guessedRate.num > 0 && guessedRate.den > 0 ?
            av_q2d(guessedRate) :
            30.0;
    spdlog::info(
        "Opened {} decoder: {}x{}, {:.3f} FPS",
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
            DecodeAudioSession(path, session, audioStopToken, false);
        });

    std::int64_t firstTimestamp = AV_NOPTS_VALUE;
    std::uint64_t fallbackFrame = 0;
    auto playbackStart = std::chrono::steady_clock::now();

    while (!stopToken.stop_requested() && SessionActive(session)) {
        if (!PlaybackMayAdvance()) {
            const auto pauseStart = std::chrono::steady_clock::now();
            while (!stopToken.stop_requested() &&
                   SessionActive(session) &&
                   !PlaybackMayAdvance()) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(10));
            }
            playbackStart +=
                std::chrono::steady_clock::now() - pauseStart;
            if (stopToken.stop_requested() || !SessionActive(session)) {
                break;
            }
        }

        result = av_read_frame(format, packet);
        if (result < 0) {
            const std::int64_t seekTarget =
                stream->start_time == AV_NOPTS_VALUE ?
                    0 :
                    stream->start_time;
            if (av_seek_frame(
                    format,
                    videoStream,
                    seekTarget,
                    AVSEEK_FLAG_BACKWARD) < 0) {
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
            if (!ApplyCenterCrop(decoded, outputWidth, outputHeight)) {
                spdlog::error(
                    "FFmpeg could not center-crop a decoded frame");
                cleanUp();
                return false;
            }

            if (!converter) {
                const auto pixelFormat =
                    static_cast<AVPixelFormat>(decoded->format);
                converter = sws_getContext(
                    decoded->width,
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
                        av_get_pix_fmt_name(pixelFormat) ?
                            av_get_pix_fmt_name(pixelFormat) :
                            "unknown");
                    cleanUp();
                    return false;
                }
                spdlog::info(
                    "Received first decoded frame: {}x{}, pixel format {}; "
                    "scaling to {}x{}",
                    decodedWidth,
                    decodedHeight,
                    av_get_pix_fmt_name(pixelFormat) ?
                        av_get_pix_fmt_name(pixelFormat) :
                        "unknown",
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

            const auto due = playbackStart +
                std::chrono::duration_cast<
                    std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(
                        std::max(0.0, presentationSeconds)));
            while (!stopToken.stop_requested() &&
                   SessionActive(session) &&
                   std::chrono::steady_clock::now() < due) {
                if (!PlaybackMayAdvance()) {
                    const auto pauseStart =
                        std::chrono::steady_clock::now();
                    while (!stopToken.stop_requested() &&
                           SessionActive(session) &&
                           !PlaybackMayAdvance()) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(10));
                    }
                    playbackStart +=
                        std::chrono::steady_clock::now() - pauseStart;
                    break;
                }
                const auto remaining =
                    due - std::chrono::steady_clock::now();
                std::this_thread::sleep_for(
                    std::min(
                        remaining,
                        std::chrono::steady_clock::duration(
                            std::chrono::milliseconds(5))));
            }
            if (stopToken.stop_requested() || !SessionActive(session)) {
                break;
            }

            auto frame = std::make_shared<VideoFrame>();
            frame->width = outputWidth;
            frame->height = outputHeight;
            frame->rowPitch = frame->width * 4;
            frame->pixels.resize(
                static_cast<std::size_t>(frame->rowPitch) *
                frame->height);
            std::uint8_t* outputPlanes[4]{
                frame->pixels.data(), nullptr, nullptr, nullptr
            };
            int outputStrides[4]{
                static_cast<int>(frame->rowPitch), 0, 0, 0
            };
            sws_scale(
                converter,
                decoded->data,
                decoded->linesize,
                0,
                decoded->height,
                outputPlanes,
                outputStrides);
            frame->serial = nextFrameSerial_++;
            latestFrame_.store(std::move(frame), std::memory_order_release);
            av_frame_unref(decoded);
        }
    }

    spdlog::info("Stopped decoding main-menu session {}", session);
    cleanUp();
    return true;
}
