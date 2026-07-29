#pragma once

#include "PlaybackTypes.h"

class VideoPlayer
{
public:
    static VideoPlayer& GetSingleton();

    void OnNativeVideoOpened(
        std::uint32_t width,
        std::uint32_t height,
        std::optional<std::filesystem::path> selectedVideo = std::nullopt);
    void OnNativeVideoClosed();
    void StartSidecarAudio(const std::filesystem::path& path);
    void StopSidecarAudio();
    void AdjustVolume(float delta);
    [[nodiscard]] float Volume() const noexcept;
    [[nodiscard]] std::shared_ptr<const VideoFrame> GetLatestFrame() const;

private:
    VideoPlayer();
    ~VideoPlayer();
    VideoPlayer(const VideoPlayer&) = delete;
    VideoPlayer& operator=(const VideoPlayer&) = delete;

    void Worker(std::stop_token stopToken);
    bool DecodeSession(
        const std::filesystem::path& path,
        std::uint64_t session,
        std::stop_token stopToken);
    void DecodeAudioSession(
        const std::filesystem::path& path,
        std::uint64_t session,
        std::stop_token stopToken,
        bool sidecar);
    void SidecarWorker(std::stop_token stopToken);
    [[nodiscard]] std::vector<std::filesystem::path> FindVideos() const;
    [[nodiscard]] std::filesystem::path PickVideo(
        std::vector<std::filesystem::path> videos);
    [[nodiscard]] bool SessionActive(std::uint64_t session) const;
    [[nodiscard]] bool SidecarSessionActive(
        std::uint64_t session) const;

    mutable std::mutex wakeMutex_;
    std::condition_variable_any wakeCondition_;
    std::optional<std::filesystem::path> selectedVideo_;
    mutable std::mutex sidecarMutex_;
    std::condition_variable_any sidecarCondition_;
    std::optional<std::filesystem::path> sidecarPath_;
    std::filesystem::path previousVideo_;
    std::mt19937_64 random_;
    std::jthread worker_;
    std::jthread sidecarWorker_;
    std::atomic<std::uint64_t> session_{ 0 };
    std::atomic<std::uint64_t> sidecarSession_{ 0 };
    std::atomic<bool> nativeVideoActive_{ false };
    std::atomic<bool> sidecarActive_{ false };
    std::atomic<float> volume_{ 1.0F };
    std::atomic<std::uint32_t> outputWidth_{ 0 };
    std::atomic<std::uint32_t> outputHeight_{ 0 };
    std::atomic<std::shared_ptr<const VideoFrame>> latestFrame_;
    std::uint64_t nextFrameSerial_{ 1 };
};
