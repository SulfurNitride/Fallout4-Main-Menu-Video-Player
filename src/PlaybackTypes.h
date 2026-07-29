#pragma once

enum class PlaybackChannel : std::uint8_t
{
    kMainMenu,
    kTelevision,
    kProjector
};

enum class PlaybackState : std::uint8_t
{
    kStopped,
    kPlaying,
    kPaused,
    kNoMedia,
    kError
};

enum class FrameScaling : std::uint8_t
{
    kFill,
    kFit
};

struct VideoFrame
{
    std::vector<std::uint8_t> pixels;
    std::uint32_t width{ 0 };
    std::uint32_t height{ 0 };
    std::uint32_t rowPitch{ 0 };
    std::uint64_t serial{ 0 };
};

struct PlaybackSnapshot
{
    PlaybackState state{ PlaybackState::kStopped };
    std::filesystem::path mediaPath;
    std::string mediaId;
    double positionSeconds{ 0.0 };
    double durationSeconds{ 0.0 };
    bool loop{ false };
    std::uint32_t consumers{ 0 };
};

[[nodiscard]] constexpr std::string_view PlaybackChannelName(
    const PlaybackChannel channel) noexcept
{
    switch (channel) {
    case PlaybackChannel::kMainMenu:
        return "MainMenu";
    case PlaybackChannel::kTelevision:
        return "Television";
    case PlaybackChannel::kProjector:
        return "Projector";
    }
    return "Unknown";
}
