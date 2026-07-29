#include "PCH.h"

#include "AudioOutput.h"
#include "Config.h"
#include "WorldPlayback.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libavutil/channel_layout.h>
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

    AVCodec* FindDecoder(const AVCodecParameters* parameters)
    {
        if (parameters->codec_id == AV_CODEC_ID_AV1) {
            if (AVCodec* aom = avcodec_find_decoder_by_name("libaom-av1")) {
                return aom;
            }
        }
        return avcodec_find_decoder(parameters->codec_id);
    }

    struct OutputRegion
    {
        std::uint32_t x{ 0 };
        std::uint32_t y{ 0 };
        std::uint32_t width{ 0 };
        std::uint32_t height{ 0 };
    };

    OutputRegion FitRegion(
        const std::uint32_t inputWidth,
        const std::uint32_t inputHeight,
        const std::uint32_t outputWidth,
        const std::uint32_t outputHeight)
    {
        OutputRegion region{
            .width = outputWidth,
            .height = outputHeight
        };
        if (static_cast<std::uint64_t>(inputWidth) * outputHeight >
            static_cast<std::uint64_t>(outputWidth) * inputHeight) {
            region.height = std::max(
                1U,
                static_cast<std::uint32_t>(
                    static_cast<std::uint64_t>(outputWidth) *
                    inputHeight / inputWidth));
            region.y = (outputHeight - region.height) / 2;
        } else {
            region.width = std::max(
                1U,
                static_cast<std::uint32_t>(
                    static_cast<std::uint64_t>(outputHeight) *
                    inputWidth / inputHeight));
            region.x = (outputWidth - region.width) / 2;
        }
        return region;
    }

    bool ApplyCenterCrop(
        AVFrame* frame,
        const std::uint32_t outputWidth,
        const std::uint32_t outputHeight)
    {
        const std::uint64_t inputAspect =
            static_cast<std::uint64_t>(frame->width) * outputHeight;
        const std::uint64_t outputAspect =
            static_cast<std::uint64_t>(outputWidth) * frame->height;
        if (inputAspect > outputAspect) {
            const std::size_t width = static_cast<std::size_t>(
                static_cast<std::uint64_t>(frame->height) *
                outputWidth / outputHeight);
            const std::size_t removed =
                static_cast<std::size_t>(frame->width) - width;
            frame->crop_left = removed / 2;
            frame->crop_right = removed - frame->crop_left;
        } else if (inputAspect < outputAspect) {
            const std::size_t height = static_cast<std::size_t>(
                static_cast<std::uint64_t>(frame->width) *
                outputHeight / outputWidth);
            const std::size_t removed =
                static_cast<std::size_t>(frame->height) - height;
            frame->crop_top = removed / 2;
            frame->crop_bottom = removed - frame->crop_top;
        }
        return av_frame_apply_cropping(frame, 0) >= 0;
    }
}

WorldPlaybackSession::WorldPlaybackSession(Options options) :
    options_(std::move(options)),
    library_(options_.mediaRoot, Config::RecursiveMediaScan()),
    volume_(options_.volume),
    worker_([this](std::stop_token stopToken) {
        Worker(stopToken);
    })
{
    RefreshLibrary();
}

WorldPlaybackSession::~WorldPlaybackSession()
{
    worker_.request_stop();
    generation_.fetch_add(1, std::memory_order_release);
    wakeCondition_.notify_all();
}

void WorldPlaybackSession::RefreshLibrary()
{
    auto media = library_.Scan();
    {
        std::scoped_lock lock(stateMutex_);
        media_ = media;
        shuffle_.Reset(media_);
    }
    spdlog::info(
        "{} library discovered {} video{} in {}",
        PlaybackChannelName(options_.channel),
        media.size(),
        media.size() == 1 ? "" : "s",
        Utf8Path(options_.mediaRoot));
    generation_.fetch_add(1, std::memory_order_release);
    wakeCondition_.notify_all();
}

std::vector<std::string> WorldPlaybackSession::AvailableMedia() const
{
    std::scoped_lock lock(stateMutex_);
    std::vector<std::string> result;
    result.reserve(media_.size());
    for (const auto& path : media_) {
        result.push_back(library_.MediaId(path));
    }
    return result;
}

bool WorldPlaybackSession::Select(const std::string_view mediaId)
{
    const auto path = library_.Resolve(mediaId);
    if (!path) {
        spdlog::warn(
            "{} rejected unavailable media id '{}'",
            PlaybackChannelName(options_.channel),
            mediaId);
        return false;
    }

    {
        std::scoped_lock lock(stateMutex_);
        requestedMedia_ = *path;
    }
    explicitSelectionPending_.store(true, std::memory_order_release);
    stopped_.store(false, std::memory_order_release);
    paused_.store(false, std::memory_order_release);
    generation_.fetch_add(1, std::memory_order_release);
    wakeCondition_.notify_all();
    return true;
}

bool WorldPlaybackSession::Restore(
    const std::string_view mediaId,
    const double positionSeconds,
    const bool paused,
    const bool loop)
{
    resumePositionMilliseconds_.store(
        static_cast<std::int64_t>(
            std::max(0.0, positionSeconds) * 1000.0),
        std::memory_order_release);
    SetLoop(loop);
    if (!Select(mediaId)) {
        resumePositionMilliseconds_.store(0, std::memory_order_release);
        return false;
    }
    paused_.store(paused, std::memory_order_release);
    return true;
}

void WorldPlaybackSession::Play()
{
    stopped_.store(false, std::memory_order_release);
    paused_.store(false, std::memory_order_release);
    {
        std::scoped_lock lock(audioOutputMutex_);
        if (activeAudioOutput_) {
            activeAudioOutput_->Resume();
        }
    }
    wakeCondition_.notify_all();
}

void WorldPlaybackSession::Pause()
{
    paused_.store(true, std::memory_order_release);
    {
        std::scoped_lock lock(audioOutputMutex_);
        if (activeAudioOutput_) {
            activeAudioOutput_->Pause();
        }
    }
    std::scoped_lock lock(stateMutex_);
    if (snapshot_.state == PlaybackState::kPlaying) {
        snapshot_.state = PlaybackState::kPaused;
    }
}

void WorldPlaybackSession::Stop()
{
    stopped_.store(true, std::memory_order_release);
    paused_.store(false, std::memory_order_release);
    generation_.fetch_add(1, std::memory_order_release);
    latestFrame_.store({}, std::memory_order_release);
    UpdateState(PlaybackState::kStopped);
    wakeCondition_.notify_all();
}

void WorldPlaybackSession::Next()
{
    nextRequested_.store(true, std::memory_order_release);
    stopped_.store(false, std::memory_order_release);
    generation_.fetch_add(1, std::memory_order_release);
    wakeCondition_.notify_all();
}

void WorldPlaybackSession::Previous()
{
    previousRequested_.store(true, std::memory_order_release);
    stopped_.store(false, std::memory_order_release);
    generation_.fetch_add(1, std::memory_order_release);
    wakeCondition_.notify_all();
}

void WorldPlaybackSession::SeekBy(const double seconds)
{
    const auto milliseconds = static_cast<std::int64_t>(
        std::clamp(seconds, -3600.0, 3600.0) * 1000.0);
    seekDeltaMilliseconds_.fetch_add(
        milliseconds,
        std::memory_order_release);
    wakeCondition_.notify_all();
}

void WorldPlaybackSession::SetLoop(const bool loop)
{
    loop_.store(loop, std::memory_order_release);
    std::scoped_lock lock(stateMutex_);
    snapshot_.loop = loop;
}

void WorldPlaybackSession::SetConsumers(
    const std::uint32_t consumers)
{
    consumers_.store(consumers, std::memory_order_release);
    {
        std::scoped_lock lock(audioOutputMutex_);
        if (activeAudioOutput_) {
            if (options_.pauseWithoutConsumers && consumers == 0) {
                activeAudioOutput_->Pause();
            } else if (!paused_.load(std::memory_order_acquire)) {
                activeAudioOutput_->Resume();
            }
        }
    }
    {
        std::scoped_lock lock(stateMutex_);
        snapshot_.consumers = consumers;
    }
    wakeCondition_.notify_all();
}

void WorldPlaybackSession::SetSpatialAudio(
    const float volume,
    const float pan)
{
    volume_.store(
        std::clamp(volume, 0.0F, 2.0F),
        std::memory_order_release);
    pan_.store(
        std::clamp(pan, -1.0F, 1.0F),
        std::memory_order_release);
}

PlaybackSnapshot WorldPlaybackSession::Snapshot() const
{
    std::scoped_lock lock(stateMutex_);
    return snapshot_;
}

std::shared_ptr<const VideoFrame>
WorldPlaybackSession::LatestFrame() const
{
    return latestFrame_.load(std::memory_order_acquire);
}

PlaybackChannel WorldPlaybackSession::Channel() const noexcept
{
    return options_.channel;
}

void WorldPlaybackSession::Worker(std::stop_token stopToken)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    std::uint64_t handledGeneration = 0;

    while (!stopToken.stop_requested()) {
        const std::uint64_t generation =
            generation_.load(std::memory_order_acquire);
        if (generation == handledGeneration ||
            stopped_.load(std::memory_order_acquire) ||
            (options_.pauseWithoutConsumers &&
             consumers_.load(std::memory_order_acquire) == 0)) {
            std::unique_lock lock(stateMutex_);
            wakeCondition_.wait(lock, stopToken, [&] {
                return generation_.load(std::memory_order_acquire) !=
                           handledGeneration ||
                       (!stopped_.load(std::memory_order_acquire) &&
                        (!options_.pauseWithoutConsumers ||
                         consumers_.load(std::memory_order_acquire) > 0));
            });
            if (stopToken.stop_requested()) {
                return;
            }
        }

        handledGeneration =
            generation_.load(std::memory_order_acquire);
        if (stopped_.load(std::memory_order_acquire)) {
            continue;
        }

        std::optional<std::filesystem::path> selected;
        {
            std::scoped_lock lock(stateMutex_);
            if (previousRequested_.exchange(
                    false,
                    std::memory_order_acq_rel) &&
                history_.size() > 1) {
                history_.pop_back();
                selected = history_.back();
                requestedMedia_ = selected;
            } else if (requestedMedia_) {
                selected = requestedMedia_;
            }
        }
        const bool explicitSelection =
            explicitSelectionPending_.exchange(
                false,
                std::memory_order_acq_rel);
        if ((!explicitSelection && options_.automaticPlaylist) ||
            nextRequested_.exchange(false, std::memory_order_acq_rel)) {
            selected = SelectAutomaticMedia();
            if (selected) {
                std::scoped_lock lock(stateMutex_);
                requestedMedia_ = selected;
            }
        }

        if (!selected) {
            latestFrame_.store({}, std::memory_order_release);
            bool noMedia = false;
            {
                std::scoped_lock lock(stateMutex_);
                noMedia = media_.empty();
            }
            UpdateState(
                noMedia ? PlaybackState::kNoMedia : PlaybackState::kStopped);
            std::unique_lock lock(stateMutex_);
            wakeCondition_.wait(lock, stopToken, [&] {
                return generation_.load(std::memory_order_acquire) !=
                       handledGeneration;
            });
            continue;
        }

        {
            std::scoped_lock lock(stateMutex_);
            if (history_.empty() || history_.back() != *selected) {
                history_.push_back(*selected);
                if (history_.size() > 100) {
                    history_.erase(history_.begin());
                }
            }
        }

        const DecodeResult result =
            Decode(*selected, handledGeneration, stopToken);
        latestFrame_.store({}, std::memory_order_release);
        if (result == DecodeResult::kInterrupted) {
            continue;
        }
        if (result == DecodeResult::kFailed) {
            UpdateState(PlaybackState::kError, *selected);
        }

        if (options_.automaticPlaylist) {
            generation_.fetch_add(1, std::memory_order_release);
            continue;
        }
        if (loop_.load(std::memory_order_acquire) &&
            result == DecodeResult::kCompleted) {
            generation_.fetch_add(1, std::memory_order_release);
            continue;
        }
        stopped_.store(true, std::memory_order_release);
        UpdateState(
            result == DecodeResult::kFailed ?
                PlaybackState::kError :
                PlaybackState::kStopped,
            *selected);
    }
}

WorldPlaybackSession::DecodeResult WorldPlaybackSession::Decode(
    const std::filesystem::path& path,
    const std::uint64_t generation,
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
    const auto fail = [&](const std::string_view reason) {
        spdlog::error(
            "{} playback failed for {}: {}",
            PlaybackChannelName(options_.channel),
            Utf8Path(path),
            reason);
        cleanUp();
        return DecodeResult::kFailed;
    };

    const std::string nativePath = Utf8Path(path);
    int result = avformat_open_input(
        &format,
        nativePath.c_str(),
        nullptr,
        nullptr);
    if (result < 0) {
        return fail(AvError(result));
    }

    const int videoStream = FindVideoStream(format);
    AVCodec* codec = videoStream >= 0 ?
        FindDecoder(format->streams[videoStream]->codecpar) :
        nullptr;
    if (videoStream < 0 || !codec) {
        return fail("no decodable video stream");
    }

    decoder = avcodec_alloc_context3(codec);
    if (!decoder) {
        return fail("could not allocate a decoder");
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
        return fail(AvError(result));
    }

    packet = av_packet_alloc();
    decoded = av_frame_alloc();
    if (!packet || !decoded) {
        return fail("could not allocate FFmpeg frame resources");
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
    const double durationSeconds =
        format->duration != AV_NOPTS_VALUE ?
            static_cast<double>(format->duration) / AV_TIME_BASE :
            0.0;

    spdlog::info(
        "{} playing {} with {}: {}x{}, {:.3f} FPS",
        PlaybackChannelName(options_.channel),
        nativePath,
        codec->name,
        decoder->width,
        decoder->height,
        framesPerSecond);
    UpdateState(
        PlaybackState::kPlaying,
        path,
        0.0,
        durationSeconds);

    audioSeekTargetMilliseconds_.store(-1, std::memory_order_release);
    std::jthread audioWorker;
    if (options_.audioEnabled) {
        audioWorker = std::jthread(
            [this, path, generation](
                std::stop_token audioStopToken) {
                DecodeAudio(path, generation, audioStopToken);
            });
    }

    const std::int64_t streamStartTimestamp =
        stream->start_time != AV_NOPTS_VALUE ?
            stream->start_time :
            0;
    std::uint64_t fallbackFrame = 0;
    double discardVideoBeforeSeconds = -1.0;
    auto playbackStart = std::chrono::steady_clock::now();
    OutputRegion region{
        .width = options_.outputWidth,
        .height = options_.outputHeight
    };

    const std::int64_t resumeMilliseconds =
        resumePositionMilliseconds_.exchange(
            0,
            std::memory_order_acq_rel);
    if (resumeMilliseconds > 0) {
        const double resumeSeconds =
            static_cast<double>(resumeMilliseconds) / 1000.0;
        const auto target = static_cast<std::int64_t>(
            resumeSeconds / av_q2d(stream->time_base)) +
            streamStartTimestamp;
        if (av_seek_frame(
                format,
                videoStream,
                target,
                AVSEEK_FLAG_BACKWARD) >= 0) {
            avcodec_flush_buffers(decoder);
            discardVideoBeforeSeconds = resumeSeconds;
            fallbackFrame = static_cast<std::uint64_t>(
                resumeSeconds * framesPerSecond);
            playbackStart =
                std::chrono::steady_clock::now() -
                std::chrono::duration_cast<
                    std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(resumeSeconds));
            UpdateState(
                paused_.load(std::memory_order_acquire) ?
                    PlaybackState::kPaused :
                    PlaybackState::kPlaying,
                path,
                resumeSeconds,
                durationSeconds);
            audioSeekTargetMilliseconds_.store(
                resumeMilliseconds,
                std::memory_order_release);
        }
    }

    while (Active(generation, stopToken)) {
        if (ShouldPause()) {
            const auto pauseStart = std::chrono::steady_clock::now();
            std::unique_lock lock(stateMutex_);
            wakeCondition_.wait(lock, stopToken, [&] {
                return !ShouldPause() ||
                       generation_.load(std::memory_order_acquire) !=
                           generation;
            });
            if (!Active(generation, stopToken)) {
                cleanUp();
                return DecodeResult::kInterrupted;
            }
            playbackStart +=
                std::chrono::steady_clock::now() - pauseStart;
            const double positionSeconds = snapshot_.positionSeconds;
            lock.unlock();
            UpdateState(
                PlaybackState::kPlaying,
                path,
                positionSeconds,
                durationSeconds);
        }

        const std::int64_t seekDelta =
            seekDeltaMilliseconds_.exchange(
                0,
                std::memory_order_acq_rel);
        if (seekDelta != 0) {
            const auto snapshot = Snapshot();
            const double targetSeconds = std::clamp(
                snapshot.positionSeconds +
                    static_cast<double>(seekDelta) / 1000.0,
                0.0,
                durationSeconds > 0.0 ?
                    durationSeconds :
                    std::numeric_limits<double>::max());
            const auto target = static_cast<std::int64_t>(
                targetSeconds / av_q2d(stream->time_base)) +
                streamStartTimestamp;
            if (av_seek_frame(
                    format,
                    videoStream,
                    target,
                    AVSEEK_FLAG_BACKWARD) >= 0) {
                avcodec_flush_buffers(decoder);
                discardVideoBeforeSeconds = targetSeconds;
                fallbackFrame = static_cast<std::uint64_t>(
                    targetSeconds * framesPerSecond);
                playbackStart =
                    std::chrono::steady_clock::now() -
                    std::chrono::duration_cast<
                        std::chrono::steady_clock::duration>(
                        std::chrono::duration<double>(targetSeconds));
                audioSeekTargetMilliseconds_.store(
                    static_cast<std::int64_t>(
                        targetSeconds * 1000.0),
                    std::memory_order_release);
            }
        }

        result = av_read_frame(format, packet);
        if (result < 0) {
            cleanUp();
            return DecodeResult::kCompleted;
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
            if (!Active(generation, stopToken)) {
                cleanUp();
                return DecodeResult::kInterrupted;
            }

            if (options_.scaling == FrameScaling::kFill) {
                if (!ApplyCenterCrop(
                        decoded,
                        options_.outputWidth,
                        options_.outputHeight)) {
                    return fail("could not crop a decoded frame");
                }
                region = {
                    .width = options_.outputWidth,
                    .height = options_.outputHeight
                };
            } else {
                region = FitRegion(
                    static_cast<std::uint32_t>(decoded->width),
                    static_cast<std::uint32_t>(decoded->height),
                    options_.outputWidth,
                    options_.outputHeight);
            }

            if (!converter) {
                converter = sws_getContext(
                    decoded->width,
                    decoded->height,
                    static_cast<AVPixelFormat>(decoded->format),
                    static_cast<int>(region.width),
                    static_cast<int>(region.height),
                    AV_PIX_FMT_BGRA,
                    SWS_BILINEAR,
                    nullptr,
                    nullptr,
                    nullptr);
                if (!converter) {
                    return fail("could not initialize pixel conversion");
                }
            }

            const std::int64_t timestamp = decoded->best_effort_timestamp;
            double presentationSeconds =
                static_cast<double>(fallbackFrame++) / framesPerSecond;
            if (timestamp != AV_NOPTS_VALUE) {
                presentationSeconds =
                    static_cast<double>(
                        timestamp - streamStartTimestamp) *
                    av_q2d(stream->time_base);
            }
            if (discardVideoBeforeSeconds >= 0.0 &&
                presentationSeconds +
                        0.5 / std::max(1.0, framesPerSecond) <
                    discardVideoBeforeSeconds) {
                av_frame_unref(decoded);
                continue;
            }
            discardVideoBeforeSeconds = -1.0;

            const auto due = playbackStart +
                std::chrono::duration_cast<
                    std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(
                        std::max(0.0, presentationSeconds)));
            while (Active(generation, stopToken) &&
                   std::chrono::steady_clock::now() < due) {
                if (ShouldPause()) {
                    break;
                }
                std::this_thread::sleep_for(
                    std::min(
                        due - std::chrono::steady_clock::now(),
                        std::chrono::steady_clock::duration(
                            std::chrono::milliseconds(5))));
            }
            if (!Active(generation, stopToken)) {
                cleanUp();
                return DecodeResult::kInterrupted;
            }

            auto frame = std::make_shared<VideoFrame>();
            frame->width = options_.outputWidth;
            frame->height = options_.outputHeight;
            frame->rowPitch = frame->width * 4;
            frame->pixels.assign(
                static_cast<std::size_t>(frame->rowPitch) *
                    frame->height,
                0);
            for (std::size_t index = 3;
                 index < frame->pixels.size();
                 index += 4) {
                frame->pixels[index] = 255;
            }

            std::uint8_t* outputPlanes[4]{
                frame->pixels.data() +
                    static_cast<std::size_t>(region.y) *
                        frame->rowPitch +
                    static_cast<std::size_t>(region.x) * 4,
                nullptr,
                nullptr,
                nullptr
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
            frame->serial = nextFrameSerial_.fetch_add(
                1,
                std::memory_order_relaxed);
            latestFrame_.store(std::move(frame), std::memory_order_release);
            UpdateState(
                paused_.load(std::memory_order_acquire) ?
                    PlaybackState::kPaused :
                    PlaybackState::kPlaying,
                path,
                presentationSeconds,
                durationSeconds);
            av_frame_unref(decoded);
        }
    }

    cleanUp();
    return DecodeResult::kInterrupted;
}

void WorldPlaybackSession::DecodeAudio(
    const std::filesystem::path& path,
    const std::uint64_t generation,
    std::stop_token stopToken)
{
    constexpr int kOutputSampleRate{ 48000 };
    constexpr int kOutputChannels{ 2 };
    constexpr AVSampleFormat kOutputFormat{ AV_SAMPLE_FMT_S16 };

    while (Active(generation, stopToken) && !LatestFrame()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!Active(generation, stopToken)) {
        return;
    }

    AVFormatContext* format = nullptr;
    AVCodecContext* decoder = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* decoded = nullptr;
    SwrContext* resampler = nullptr;
    AudioOutput output;

    const auto cleanUp = [&] {
        {
            std::scoped_lock lock(audioOutputMutex_);
            if (activeAudioOutput_ == &output) {
                activeAudioOutput_ = nullptr;
            }
        }
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
        cleanUp();
        return;
    }

    int audioStream = -1;
    for (unsigned int index = 0; index < format->nb_streams; ++index) {
        const AVStream* stream = format->streams[index];
        if (stream && stream->codecpar &&
            stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audioStream = static_cast<int>(index);
            break;
        }
    }
    AVCodec* codec = audioStream >= 0 ?
        avcodec_find_decoder(
            format->streams[audioStream]->codecpar->codec_id) :
        nullptr;
    if (audioStream < 0 || !codec) {
        spdlog::info(
            "{} media has no decodable audio stream",
            PlaybackChannelName(options_.channel));
        cleanUp();
        return;
    }

    decoder = avcodec_alloc_context3(codec);
    if (!decoder) {
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
    packet = av_packet_alloc();
    decoded = av_frame_alloc();
    if (!resampler ||
        swr_init(resampler) < 0 ||
        !packet ||
        !decoded ||
        !output.Initialize(kOutputSampleRate, kOutputChannels)) {
        cleanUp();
        return;
    }
    output.SetVolume(volume_.load(std::memory_order_acquire));
    output.SetPan(pan_.load(std::memory_order_acquire));
    {
        std::scoped_lock lock(audioOutputMutex_);
        activeAudioOutput_ = &output;
        if (ShouldPause()) {
            activeAudioOutput_->Pause();
        }
    }

    AVStream* stream = format->streams[audioStream];
    const std::int64_t streamStartTimestamp =
        stream->start_time != AV_NOPTS_VALUE ?
            stream->start_time :
            0;
    double discardAudioBeforeSeconds = -1.0;
    while (Active(generation, stopToken)) {
        if (ShouldPause()) {
            output.Pause();
            std::unique_lock lock(stateMutex_);
            wakeCondition_.wait(lock, stopToken, [&] {
                return !ShouldPause() ||
                       !Active(generation, stopToken);
            });
            output.Resume();
            if (!Active(generation, stopToken)) {
                break;
            }
        }

        const std::int64_t seekTarget =
            audioSeekTargetMilliseconds_.exchange(
                -1,
                std::memory_order_acq_rel);
        if (seekTarget >= 0) {
            const double seconds =
                static_cast<double>(seekTarget) / 1000.0;
            const auto timestamp = static_cast<std::int64_t>(
                seconds / av_q2d(stream->time_base)) +
                streamStartTimestamp;
            if (av_seek_frame(
                    format,
                    audioStream,
                    timestamp,
                    AVSEEK_FLAG_BACKWARD) >= 0) {
                avcodec_flush_buffers(decoder);
                swr_close(resampler);
                swr_init(resampler);
                output.Reset();
                if (!output.Initialize(
                        kOutputSampleRate,
                        kOutputChannels)) {
                    break;
                }
                output.SetVolume(
                    volume_.load(std::memory_order_acquire));
                output.SetPan(
                    pan_.load(std::memory_order_acquire));
                discardAudioBeforeSeconds = seconds;
                spdlog::info(
                    "{} audio seek synchronized to {:.3f} seconds",
                    PlaybackChannelName(options_.channel),
                    seconds);
            }
        }

        output.SetVolume(volume_.load(std::memory_order_acquire));
        output.SetPan(pan_.load(std::memory_order_acquire));
        result = av_read_frame(format, packet);
        if (result < 0) {
            break;
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
            if (!Active(generation, stopToken)) {
                break;
            }
            int inputSamples = decoded->nb_samples;
            int skippedSamples = 0;
            const std::int64_t decodedTimestamp =
                decoded->best_effort_timestamp;
            if (discardAudioBeforeSeconds >= 0.0 &&
                decodedTimestamp != AV_NOPTS_VALUE) {
                const double frameStartSeconds =
                    static_cast<double>(
                        decodedTimestamp - streamStartTimestamp) *
                    av_q2d(stream->time_base);
                const double frameEndSeconds =
                    frameStartSeconds +
                    static_cast<double>(decoded->nb_samples) /
                        decoder->sample_rate;
                if (frameEndSeconds <= discardAudioBeforeSeconds) {
                    av_frame_unref(decoded);
                    continue;
                }
                if (frameStartSeconds < discardAudioBeforeSeconds) {
                    skippedSamples = std::clamp(
                        static_cast<int>(std::ceil(
                            (discardAudioBeforeSeconds -
                             frameStartSeconds) *
                            decoder->sample_rate)),
                        0,
                        decoded->nb_samples);
                    inputSamples -= skippedSamples;
                }
                discardAudioBeforeSeconds = -1.0;
            }
            if (inputSamples <= 0) {
                av_frame_unref(decoded);
                continue;
            }

            const std::int64_t delayedSamples =
                swr_get_delay(resampler, decoder->sample_rate);
            const int outputCapacity = static_cast<int>(av_rescale_rnd(
                delayedSamples + inputSamples,
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
            const bool planar =
                av_sample_fmt_is_planar(decoder->sample_fmt) != 0;
            const int bytesPerSample =
                av_get_bytes_per_sample(decoder->sample_fmt);
            const int inputPlaneCount =
                planar ? decoder->channels : 1;
            std::vector<const std::uint8_t*> inputPlanes(
                static_cast<std::size_t>(inputPlaneCount));
            const std::size_t skipBytes =
                static_cast<std::size_t>(skippedSamples) *
                static_cast<std::size_t>(bytesPerSample) *
                static_cast<std::size_t>(
                    planar ? 1 : decoder->channels);
            for (int plane = 0; plane < inputPlaneCount; ++plane) {
                inputPlanes[static_cast<std::size_t>(plane)] =
                    decoded->extended_data[plane] + skipBytes;
            }
            const int converted = swr_convert(
                resampler,
                outputPlanes,
                outputCapacity,
                inputPlanes.data(),
                inputSamples);
            av_frame_unref(decoded);
            if (converted <= 0) {
                continue;
            }
            samples.resize(
                static_cast<std::size_t>(converted) *
                kOutputChannels *
                sizeof(std::int16_t));
            output.Submit(std::move(samples));
        }
    }
    cleanUp();
}

bool WorldPlaybackSession::Active(
    const std::uint64_t generation,
    const std::stop_token stopToken) const
{
    return !stopToken.stop_requested() &&
           !stopped_.load(std::memory_order_acquire) &&
           generation_.load(std::memory_order_acquire) == generation;
}

bool WorldPlaybackSession::ShouldPause() const noexcept
{
    return paused_.load(std::memory_order_acquire) ||
           (options_.pauseWithoutConsumers &&
            consumers_.load(std::memory_order_acquire) == 0);
}

std::optional<std::filesystem::path>
WorldPlaybackSession::SelectAutomaticMedia()
{
    std::scoped_lock lock(stateMutex_);
    return shuffle_.Next();
}

void WorldPlaybackSession::UpdateState(
    const PlaybackState state,
    const std::filesystem::path& path,
    const double positionSeconds,
    const double durationSeconds)
{
    std::scoped_lock lock(stateMutex_);
    snapshot_.state = state;
    if (!path.empty()) {
        snapshot_.mediaPath = path;
        snapshot_.mediaId = library_.MediaId(path);
    } else if (state == PlaybackState::kStopped ||
               state == PlaybackState::kNoMedia) {
        snapshot_.mediaPath.clear();
        snapshot_.mediaId.clear();
    }
    snapshot_.positionSeconds = positionSeconds;
    snapshot_.durationSeconds = durationSeconds;
    snapshot_.loop = loop_.load(std::memory_order_acquire);
    snapshot_.consumers = consumers_.load(std::memory_order_acquire);
}

WorldPlayback& WorldPlayback::GetSingleton()
{
    static WorldPlayback instance;
    return instance;
}

bool WorldPlayback::Initialize()
{
    if (Initialized()) {
        return true;
    }
    if (!Config::EnableWorldScreens() &&
        !Config::EnablePipBoyPlayer()) {
        spdlog::info(
            "World and Pip-Boy playback are disabled by configuration");
        return true;
    }

    television_ = std::make_unique<WorldPlaybackSession>(
        WorldPlaybackSession::Options{
            .channel = PlaybackChannel::kTelevision,
            .mediaRoot = Config::TelevisionDirectory(),
            .outputWidth = Config::TelevisionWidth(),
            .outputHeight = Config::TelevisionHeight(),
            .scaling = FrameScaling::kFit,
            .automaticPlaylist = true,
            .pauseWithoutConsumers = true,
            .audioEnabled = true,
            .volume = Config::TelevisionVolume()
        });
    projector_ = std::make_unique<WorldPlaybackSession>(
        WorldPlaybackSession::Options{
            .channel = PlaybackChannel::kProjector,
            .mediaRoot = Config::MovieDirectory(),
            .outputWidth = Config::ProjectorWidth(),
            .outputHeight = Config::ProjectorHeight(),
            .scaling = FrameScaling::kFit,
            .automaticPlaylist = false,
            .pauseWithoutConsumers = true,
            .audioEnabled = true,
            .volume = Config::MovieVolume()
        });
    spdlog::info(
        "Initialized TV/movie playback sessions (workshop screens={})",
        Config::EnableWorldScreens());
    return true;
}

void WorldPlayback::Shutdown()
{
    projector_.reset();
    television_.reset();
}

bool WorldPlayback::Initialized() const noexcept
{
    return television_ && projector_;
}

WorldPlaybackSession* WorldPlayback::Television() noexcept
{
    return television_.get();
}

WorldPlaybackSession* WorldPlayback::Projector() noexcept
{
    return projector_.get();
}

std::shared_ptr<const VideoFrame> WorldPlayback::Frame(
    const PlaybackChannel channel) const
{
    switch (channel) {
    case PlaybackChannel::kTelevision:
        return television_ ? television_->LatestFrame() : nullptr;
    case PlaybackChannel::kProjector:
        return projector_ ? projector_->LatestFrame() : nullptr;
    case PlaybackChannel::kMainMenu:
        return nullptr;
    }
    return nullptr;
}
