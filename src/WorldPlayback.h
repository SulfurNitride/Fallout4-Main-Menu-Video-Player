#pragma once

#include "MediaLibrary.h"
#include "PlaybackTypes.h"

#include <optional>

class AudioOutput;

struct MediaProgress
{
    std::string mediaId;
    double positionSeconds{ 0.0 };
    double durationSeconds{ 0.0 };
    std::uint64_t lastPlayedMilliseconds{ 0 };
    bool completed{ false };
};

class WorldPlaybackSession
{
public:
    struct Options
    {
        PlaybackChannel channel{ PlaybackChannel::kTelevision };
        std::filesystem::path mediaRoot;
        std::uint32_t outputWidth{ 1024 };
        std::uint32_t outputHeight{ 576 };
        FrameScaling scaling{ FrameScaling::kFit };
        bool automaticPlaylist{ false };
        bool pauseWithoutConsumers{ true };
        bool audioEnabled{ true };
        float volume{ 1.0F };
    };

    explicit WorldPlaybackSession(Options options);
    ~WorldPlaybackSession();
    WorldPlaybackSession(const WorldPlaybackSession&) = delete;
    WorldPlaybackSession& operator=(const WorldPlaybackSession&) = delete;

    void RefreshLibrary();
    void RefreshCatalog();
    [[nodiscard]] std::vector<MediaLibrary::Item> AvailableMedia() const;
    [[nodiscard]] std::size_t MediaCount() const;
    [[nodiscard]] std::optional<MediaLibrary::Item> MediaAt(
        std::size_t index) const;
    [[nodiscard]] std::optional<MediaProgress> Progress(
        std::string_view mediaId) const;
    [[nodiscard]] std::vector<MediaProgress> ProgressHistory() const;
    [[nodiscard]] std::optional<MediaProgress> MostRecentProgress() const;
    void RestoreProgressHistory(std::vector<MediaProgress> progress);
    void ClearProgressHistory();
    [[nodiscard]] bool Select(std::string_view mediaId);
    [[nodiscard]] bool Restore(
        std::string_view mediaId,
        double positionSeconds,
        bool paused,
        bool loop);
    void Play();
    void Pause();
    void Stop();
    void Next();
    void Previous();
    void SeekBy(double seconds);
    void SetLoop(bool loop);
    void SetConsumers(std::uint32_t consumers);
    void SetSpatialAudio(float volume, float pan);

    [[nodiscard]] PlaybackSnapshot Snapshot() const;
    [[nodiscard]] std::shared_ptr<const VideoFrame> LatestFrame() const;
    [[nodiscard]] PlaybackChannel Channel() const noexcept;

private:
    enum class DecodeResult
    {
        kCompleted,
        kInterrupted,
        kFailed
    };

    void Worker(std::stop_token stopToken);
    DecodeResult Decode(
        const std::filesystem::path& path,
        std::uint64_t generation,
        std::stop_token stopToken);
    void DecodeAudio(
        const std::filesystem::path& path,
        std::uint64_t generation,
        std::stop_token stopToken);
    [[nodiscard]] bool Active(
        std::uint64_t generation,
        std::stop_token stopToken) const;
    [[nodiscard]] bool ShouldPause() const noexcept;
    [[nodiscard]] std::optional<std::filesystem::path>
        SelectAutomaticMedia();
    [[nodiscard]] bool QueueSelection(
        std::string_view mediaId,
        double positionSeconds,
        bool paused,
        bool markRecent);
    void UpdateState(
        PlaybackState state,
        const std::filesystem::path& path = {},
        double positionSeconds = 0.0,
        double durationSeconds = 0.0);

    Options options_;
    MediaLibrary library_;
    ShuffleBag shuffle_;
    mutable std::mutex stateMutex_;
    std::condition_variable_any wakeCondition_;
    std::vector<std::filesystem::path> media_;
    std::vector<MediaLibrary::Item> catalog_;
    std::vector<std::filesystem::path> history_;
    std::unordered_map<std::string, MediaProgress> progress_;
    std::optional<std::filesystem::path> requestedMedia_;
    PlaybackSnapshot snapshot_;
    std::atomic<float> volume_{ 1.0F };
    std::atomic<float> pan_{ 0.0F };
    std::atomic<std::uint64_t> generation_{ 1 };
    std::atomic<std::uint32_t> consumers_{ 0 };
    std::atomic<bool> paused_{ false };
    std::atomic<bool> stopped_{ false };
    std::atomic<bool> loop_{ false };
    std::atomic<bool> nextRequested_{ false };
    std::atomic<bool> previousRequested_{ false };
    std::atomic<bool> explicitSelectionPending_{ false };
    std::atomic<std::int64_t> seekDeltaMilliseconds_{ 0 };
    std::atomic<std::int64_t> resumePositionMilliseconds_{ 0 };
    std::atomic<std::int64_t> audioSeekTargetMilliseconds_{ -1 };
    std::atomic<std::shared_ptr<const VideoFrame>> latestFrame_;
    std::atomic<std::uint64_t> nextFrameSerial_{ 1 };
    mutable std::mutex audioOutputMutex_;
    AudioOutput* activeAudioOutput_{ nullptr };
    std::jthread worker_;
};

class WorldPlayback
{
public:
    static WorldPlayback& GetSingleton();

    bool Initialize();
    void Shutdown();
    [[nodiscard]] bool Initialized() const noexcept;
    [[nodiscard]] WorldPlaybackSession* Television() noexcept;
    [[nodiscard]] WorldPlaybackSession* Projector() noexcept;
    [[nodiscard]] std::shared_ptr<const VideoFrame> Frame(
        PlaybackChannel channel) const;

private:
    std::unique_ptr<WorldPlaybackSession> television_;
    std::unique_ptr<WorldPlaybackSession> projector_;
};
