#include "PCH.h"

#include "BinkFrameCompositor.h"
#include "BinkFrameScaler.h"
#include "BinkHook.h"
#include "Config.h"
#include "EngineSettings.h"
#include "InputRouter.h"
#include "MainMenuMedia.h"
#include "VideoLayout.h"
#include "VideoPlayer.h"
#include "VideoScaling.h"

#include <MinHook.h>

namespace BinkHook
{
    namespace
    {
        using BinkOpen = void*(__stdcall*)(const char*, std::uint32_t);
        using BinkClose = void(__stdcall*)(void*);
        using BinkPause = std::int32_t(__stdcall*)(void*, std::int32_t);
        using BinkSetVolume = void(__stdcall*)(void*, std::uint32_t,
                                               std::int32_t);
        using BinkGetTrackID = std::uint32_t(__stdcall*)(void*, std::uint32_t);
        using BinkSetSoundOnOff = std::int32_t(__stdcall*)(void*, std::int32_t);
        using BinkDoFrame = std::int32_t(__stdcall*)(void*);
        using BinkNextFrame = void(__stdcall*)(void*);
        using BinkWait = std::int32_t(__stdcall*)(void*);
        using BinkShouldSkip = std::int32_t(__stdcall*)(void*);
        using BinkCopyToBufferRect = std::int32_t(__stdcall*)(
            void*, void*, std::int32_t, std::uint32_t, std::uint32_t,
            std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
            std::uint32_t, std::uint32_t);

        struct PublicBinkHeader
        {
            std::uint32_t width;
            std::uint32_t height;
            std::array<std::byte, 0x140> fieldsBeforeTrackCount;
            std::uint32_t numberOfTracks;
        };
        static_assert(offsetof(PublicBinkHeader, numberOfTracks) == 0x148);

        constexpr std::uint32_t kSurfaceMask{ 15 };
        constexpr std::uint32_t kSurface32{ 3 };
        constexpr std::uint64_t kMaximumCapturedPixels = 4096ULL * 2160ULL;

        BinkOpen originalOpen{ nullptr };
        BinkClose originalClose{ nullptr };
        BinkPause originalPause{ nullptr };
        BinkSetVolume originalSetVolume{ nullptr };
        BinkGetTrackID originalGetTrackID{ nullptr };
        BinkSetSoundOnOff originalSetSoundOnOff{ nullptr };
        BinkDoFrame originalDoFrame{ nullptr };
        BinkNextFrame originalNextFrame{ nullptr };
        BinkWait originalWait{ nullptr };
        BinkShouldSkip originalShouldSkip{ nullptr };
        BinkCopyToBufferRect originalCopy{ nullptr };
        std::array<void*, 9> hookTargets{};
        std::array<bool, 9> hookCreated{};
        std::array<bool, 9> hookEnabled{};

        std::atomic<void*> mainMenuBink{ nullptr };
        void* activeBink{ nullptr };
        std::mutex activeBinkMutex;
        std::uint32_t mainMenuOpenFlags{ 0 };
        std::atomic<bool> activeBinkSelection{ false };
        std::atomic<std::shared_ptr<const VideoFrame>> activeBinkFrame;
        std::array<std::shared_ptr<VideoFrame>, 3> activeBinkFramePool;
        BinkFrameScaler::Scaler activeBinkScaler;
        std::atomic<std::uint64_t> activeBinkFrameSerial{ 1 };
        bool loggedFrameAllocationFailure{ false };
        bool loggedFrameScalingInfo{ false };
        bool loggedFrameScalingFailure{ false };
        bool loggedFrameScalingFallback{ false };
        std::atomic<std::uint32_t> mainMenuWidth{ 0 };
        std::atomic<std::uint32_t> mainMenuHeight{ 0 };
        std::atomic<std::uint32_t> presentationWidth{ 0 };
        std::atomic<std::uint32_t> presentationHeight{ 0 };
        std::atomic<std::uint32_t> loggedCopyCalls{ 0 };
        std::atomic<bool> loggedUnsupportedSurface{ false };
        std::atomic<bool> replaceMainMenuVideo{ false };
        std::atomic<bool> mainMenuStopped{ false };
        std::atomic<bool> pendingOverrideAudioStart{ false };
        std::atomic<std::uint64_t> helpVisibleUntil{ 0 };
        std::atomic<std::uint64_t> helpRevision{ 1 };
        std::unordered_map<std::uint32_t, std::int32_t> carrierTrackVolumes;
        std::vector<std::uint32_t> selectedBinkTrackIds;
        std::optional<bool> appliedBinkAudioAudible;
        bool loggedTrackVolumeLimit{ false };
        std::mutex selectionMutex;
        std::filesystem::path previousSelection;
        std::filesystem::path currentSelection;
        std::optional<std::filesystem::path> pendingOverrideAudio;
        std::mt19937_64 selectionRandom{ std::random_device{}() };

        using MainMenuMedia::FindXwmSidecar;
        using MainMenuMedia::IsBinkVideo;
        using MainMenuMedia::IsCarrierPath;
        using MainMenuMedia::Utf8Path;

        [[nodiscard]] VideoLayout::OutputRect
        CurrentContentRect(const std::uint32_t carrierWidth,
                           const std::uint32_t carrierHeight) noexcept
        {
            if (!Config::MatchWindowAspect()) {
                return VideoLayout::ComputeAspectFitRect(
                    carrierWidth, carrierHeight, carrierWidth, carrierHeight);
            }

            return VideoLayout::ComputeAspectFitRect(
                carrierWidth, carrierHeight,
                presentationWidth.load(std::memory_order_acquire),
                presentationHeight.load(std::memory_order_acquire));
        }

        std::optional<std::filesystem::path>
        SelectMainMenuVideo(const bool includeBink = true)
        {
            auto directory = Config::MainMenuDirectory();
            auto candidates = MainMenuMedia::ScanVideos(
                directory, Config::RecursiveMediaScan(), includeBink);
            if (candidates.empty() &&
                directory != std::filesystem::path("Data/MainMenuVideos")) {
                directory = "Data/MainMenuVideos";
                candidates = MainMenuMedia::ScanVideos(
                    directory, Config::RecursiveMediaScan(), includeBink);
            }
            if (candidates.empty()) {
                return std::nullopt;
            }

            std::scoped_lock lock(selectionMutex);
            std::ranges::shuffle(candidates, selectionRandom);
            if (candidates.size() > 1 &&
                candidates.front() == previousSelection) {
                std::swap(candidates.front(), candidates[1]);
            }
            previousSelection = candidates.front();
            return candidates.front();
        }

        void SetCurrentSelection(const std::filesystem::path& selection)
        {
            std::scoped_lock lock(selectionMutex);
            currentSelection = selection;
        }

        void ShowHelp(const std::uint32_t milliseconds)
        {
            helpVisibleUntil.store(GetTickCount64() + milliseconds,
                                   std::memory_order_release);
            helpRevision.fetch_add(1, std::memory_order_release);
        }

        std::string VirtualKeyName(const std::uint32_t key)
        {
            switch (key) {
            case 0:
                return "DISABLED";
            case VK_BACK:
                return "BACKSPACE";
            case VK_TAB:
                return "TAB";
            case VK_PRIOR:
                return "PAGE UP";
            case VK_NEXT:
                return "PAGE DOWN";
            case VK_SPACE:
                return "SPACE";
            case VK_ESCAPE:
                return "ESC";
            default:
                if (key >= 'A' && key <= 'Z') {
                    return std::string(1, static_cast<char>(key));
                }
                if (key >= VK_F1 && key <= VK_F24) {
                    return std::format("F{}", key - VK_F1 + 1);
                }
                return std::format("VK {}", key);
            }
        }

        std::array<std::uint8_t, 7> Glyph(const char character)
        {
            switch (static_cast<char>(
                std::toupper(static_cast<unsigned char>(character)))) {
            case 'A':
                return { 14, 17, 17, 31, 17, 17, 17 };
            case 'B':
                return { 30, 17, 17, 30, 17, 17, 30 };
            case 'C':
                return { 14, 17, 16, 16, 16, 17, 14 };
            case 'D':
                return { 30, 17, 17, 17, 17, 17, 30 };
            case 'E':
                return { 31, 16, 16, 30, 16, 16, 31 };
            case 'F':
                return { 31, 16, 16, 30, 16, 16, 16 };
            case 'G':
                return { 14, 17, 16, 23, 17, 17, 15 };
            case 'H':
                return { 17, 17, 17, 31, 17, 17, 17 };
            case 'I':
                return { 31, 4, 4, 4, 4, 4, 31 };
            case 'J':
                return { 1, 1, 1, 1, 17, 17, 14 };
            case 'K':
                return { 17, 18, 20, 24, 20, 18, 17 };
            case 'L':
                return { 16, 16, 16, 16, 16, 16, 31 };
            case 'M':
                return { 17, 27, 21, 21, 17, 17, 17 };
            case 'N':
                return { 17, 25, 21, 19, 17, 17, 17 };
            case 'O':
                return { 14, 17, 17, 17, 17, 17, 14 };
            case 'P':
                return { 30, 17, 17, 30, 16, 16, 16 };
            case 'Q':
                return { 14, 17, 17, 17, 21, 18, 13 };
            case 'R':
                return { 30, 17, 17, 30, 20, 18, 17 };
            case 'S':
                return { 15, 16, 16, 14, 1, 1, 30 };
            case 'T':
                return { 31, 4, 4, 4, 4, 4, 4 };
            case 'U':
                return { 17, 17, 17, 17, 17, 17, 14 };
            case 'V':
                return { 17, 17, 17, 17, 17, 10, 4 };
            case 'W':
                return { 17, 17, 17, 21, 21, 21, 10 };
            case 'X':
                return { 17, 17, 10, 4, 10, 17, 17 };
            case 'Y':
                return { 17, 17, 10, 4, 4, 4, 4 };
            case 'Z':
                return { 31, 1, 2, 4, 8, 16, 31 };
            case '0':
                return { 14, 17, 19, 21, 25, 17, 14 };
            case '1':
                return { 4, 12, 4, 4, 4, 4, 14 };
            case '2':
                return { 14, 17, 1, 2, 4, 8, 31 };
            case '3':
                return { 30, 1, 1, 14, 1, 1, 30 };
            case '4':
                return { 2, 6, 10, 18, 31, 2, 2 };
            case '5':
                return { 31, 16, 16, 30, 1, 1, 30 };
            case '6':
                return { 14, 16, 16, 30, 17, 17, 14 };
            case '7':
                return { 31, 1, 2, 4, 8, 8, 8 };
            case '8':
                return { 14, 17, 17, 14, 17, 17, 14 };
            case '9':
                return { 14, 17, 17, 15, 1, 1, 14 };
            case '.':
                return { 0, 0, 0, 0, 0, 12, 12 };
            case '-':
                return { 0, 0, 0, 31, 0, 0, 0 };
            case '_':
                return { 0, 0, 0, 0, 0, 0, 31 };
            case '/':
                return { 1, 2, 2, 4, 8, 8, 16 };
            case '\\':
                return { 16, 8, 8, 4, 2, 2, 1 };
            case ':':
                return { 0, 12, 12, 0, 12, 12, 0 };
            case '%':
                return { 17, 2, 4, 8, 16, 17, 0 };
            case ' ':
                return {};
            default:
                return { 14, 17, 1, 2, 4, 0, 4 };
            }
        }

        void OverlayRectangle(VideoFrame& frame, int left, int top, int right,
                              int bottom, const std::uint8_t blue,
                              const std::uint8_t green, const std::uint8_t red,
                              const std::uint8_t alpha)
        {
            left = std::clamp(left, 0, static_cast<int>(frame.width));
            right = std::clamp(right, 0, static_cast<int>(frame.width));
            top = std::clamp(top, 0, static_cast<int>(frame.height));
            bottom = std::clamp(bottom, 0, static_cast<int>(frame.height));
            for (int y = top; y < bottom; ++y) {
                for (int x = left; x < right; ++x) {
                    auto* pixel =
                        frame.pixels.data() +
                        (static_cast<std::size_t>(y) * frame.width + x) * 4;
                    pixel[0] = blue;
                    pixel[1] = green;
                    pixel[2] = red;
                    pixel[3] = alpha;
                }
            }
        }

        void OverlayText(VideoFrame& frame, const int left, const int top,
                         const std::string_view text, const int scale)
        {
            int x = left;
            for (const char character : text) {
                const auto glyph = Glyph(character);
                for (int row = 0; row < 7; ++row) {
                    for (int column = 0; column < 5; ++column) {
                        if ((glyph[row] & (1U << (4 - column))) != 0) {
                            OverlayRectangle(
                                frame, x + column * scale, top + row * scale,
                                x + (column + 1) * scale,
                                top + (row + 1) * scale, 210, 255, 210, 255);
                        }
                    }
                }
                x += 6 * scale;
                if (x >= static_cast<int>(frame.width) - 6 * scale) {
                    break;
                }
            }
        }

        VideoFrame BuildHelpOverlay(const std::uint32_t outputWidth)
        {
            std::filesystem::path selection;
            {
                std::scoped_lock lock(selectionMutex);
                selection = currentSelection;
            }
            const std::string status =
                mainMenuStopped.load(std::memory_order_acquire)
                    ? "PLAYBACK STOPPED"
                    : std::format("NOW PLAYING: {}",
                                  selection.filename().string());
            std::vector<std::string> lines{
                status,
                std::format("{}  NEW RANDOM VIDEO",
                            VirtualKeyName(Config::MainMenuNextKey())),
                std::format("{}  STOP VIDEO",
                            VirtualKeyName(Config::MainMenuStopKey())),
                std::format("{} / {}  VOLUME",
                            VirtualKeyName(Config::MainMenuVolumeUpKey()),
                            VirtualKeyName(Config::MainMenuVolumeDownKey())),
                std::format("VOLUME: {:.0f}%",
                            VideoPlayer::GetSingleton().Volume() * 100.0F)
            };
            lines.push_back(
                std::format("{}  NEXT SOUNDTRACK",
                            VirtualKeyName(Config::MainMenuNextAudioKey())));
            lines.push_back(std::format(
                "{}  AUDIO: {}",
                VirtualKeyName(Config::MainMenuToggleOriginalAudioKey()),
                VideoPlayer::GetSingleton().OriginalAudioAudible()
                    ? "VIDEO"
                    : "DEDICATED"));
            const int scale = outputWidth >= 1600 ? 3 : 2;
            std::size_t maximumCharacters = 0;
            for (const auto& line : lines) {
                maximumCharacters = std::max(maximumCharacters, line.size());
            }
            VideoFrame frame{};
            frame.width = std::min(
                outputWidth > 48 ? outputWidth - 48 : outputWidth,
                static_cast<std::uint32_t>(maximumCharacters * 6 * scale + 36));
            frame.height = static_cast<std::uint32_t>(
                28 + lines.size() * (7 * scale + 10));
            frame.rowPitch = frame.width * 4;
            frame.pixels.assign(
                static_cast<std::size_t>(frame.rowPitch) * frame.height, 0);
            OverlayRectangle(frame, 0, 0, static_cast<int>(frame.width),
                             static_cast<int>(frame.height), 0, 0, 0, 255);
            int y = 14;
            for (const auto& line : lines) {
                OverlayText(frame, 18, y, line, scale);
                y += 7 * scale + 10;
            }
            return frame;
        }

        bool ValidBinkDimensions(const void* handle)
        {
            if (!handle) {
                return false;
            }
            const auto* header = static_cast<const PublicBinkHeader*>(handle);
            return header->width > 0 && header->height > 0 &&
                   header->width <= 16384 && header->height <= 16384 &&
                   static_cast<std::uint64_t>(header->width) * header->height <=
                       kMaximumCapturedPixels;
        }

        struct CarrierChoice
        {
            void* handle{ nullptr };
            std::filesystem::path path;
        };

        CarrierChoice OpenAspectMatchedCarrier(
            const std::optional<std::filesystem::path>& selected,
            const InputRouter::ClientSizeSnapshot& client,
            const std::uint32_t flags)
        {
            if (!Config::MatchWindowAspect() || !client.available ||
                !selected) {
                return {};
            }

            std::vector<std::filesystem::path> candidates;
            if (IsBinkVideo(*selected)) {
                candidates.push_back(*selected);
            }
            auto directory = Config::MainMenuDirectory();
            auto videos = MainMenuMedia::ScanVideos(
                directory, Config::RecursiveMediaScan(), true);
            if (videos.empty() &&
                directory != std::filesystem::path("Data/MainMenuVideos")) {
                videos = MainMenuMedia::ScanVideos(
                    "Data/MainMenuVideos", Config::RecursiveMediaScan(),
                    true);
            }
            for (const auto& video : videos) {
                if (IsBinkVideo(video) &&
                    !std::ranges::contains(candidates, video)) {
                    candidates.push_back(video);
                }
            }

            for (const auto& candidate : candidates) {
                const std::string path = Utf8Path(candidate);
                void* handle = originalOpen(path.c_str(), flags);
                if (!handle) {
                    continue;
                }
                const auto* header =
                    static_cast<const PublicBinkHeader*>(handle);
                if (ValidBinkDimensions(handle) &&
                    header->numberOfTracks <= 64 &&
                    VideoLayout::IsCarrierAspectMatch(
                        header->width, header->height, client.width,
                        client.height)) {
                    return { handle, candidate };
                }
                originalClose(handle);
            }
            return {};
        }

        std::int32_t ScaledBinkVolume(const std::int32_t sourceVolume) noexcept
        {
            const double multiplier = std::clamp(
                static_cast<double>(VideoPlayer::GetSingleton().Volume()), 0.0,
                2.0);
            const double scaled =
                static_cast<double>(sourceVolume) * multiplier;
            return static_cast<std::int32_t>(std::clamp(
                scaled,
                static_cast<double>(std::numeric_limits<std::int32_t>::min()),
                static_cast<double>(std::numeric_limits<std::int32_t>::max())));
        }

        void RememberCarrierTrackVolumeLocked(const std::uint32_t track,
                                              const std::int32_t volume)
        {
            constexpr std::size_t kMaximumRememberedTracks{ 64 };
            const auto existing = carrierTrackVolumes.find(track);
            if (existing != carrierTrackVolumes.end()) {
                existing->second = volume;
                return;
            }
            if (carrierTrackVolumes.size() >= kMaximumRememberedTracks) {
                if (!loggedTrackVolumeLimit) {
                    loggedTrackVolumeLimit = true;
                    spdlog::warn("Ignoring excess main-menu Bink audio-track "
                                 "volume calls after {} distinct track IDs",
                                 kMaximumRememberedTracks);
                }
                return;
            }
            carrierTrackVolumes.emplace(track, volume);
            spdlog::debug("Observed carrier Bink audio track ID {} at source "
                          "volume {}",
                          track, volume);
        }

        std::int32_t
        SourceVolumeForSelectedTrackLocked(const std::uint32_t track) noexcept
        {
            if (const auto matching = carrierTrackVolumes.find(track);
                matching != carrierTrackVolumes.end()) {
                return matching->second;
            }
            if (carrierTrackVolumes.size() == 1) {
                return carrierTrackVolumes.begin()->second;
            }
            // Fallout normally assigns its main-menu Bink track 0x8000.
            // Use that neutral source value until the carrier reports one.
            return 32768;
        }

        void DiscoverSelectedBinkTracksLocked(void* handle)
        {
            selectedBinkTrackIds.clear();
            if (!handle || !originalGetTrackID) {
                return;
            }

            constexpr std::uint32_t kMaximumTracks{ 64 };
            const auto* header = static_cast<const PublicBinkHeader*>(handle);
            const std::uint32_t count = header->numberOfTracks;
            if (count > kMaximumTracks) {
                spdlog::warn(
                    "Selected BK2 reports an invalid audio-track count {}; "
                    "embedded volume control is disabled for this file",
                    count);
                return;
            }

            selectedBinkTrackIds.reserve(count);
            for (std::uint32_t index = 0; index < count; ++index) {
                const std::uint32_t track = originalGetTrackID(handle, index);
                if (!std::ranges::contains(selectedBinkTrackIds, track)) {
                    selectedBinkTrackIds.push_back(track);
                }
            }

            if (!selectedBinkTrackIds.empty()) {
                std::string tracks;
                for (const std::uint32_t track : selectedBinkTrackIds) {
                    if (!tracks.empty()) {
                        tracks += ", ";
                    }
                    tracks += std::to_string(track);
                }
                spdlog::info("Selected BK2 exposes {} audio track(s), IDs [{}]",
                             selectedBinkTrackIds.size(), tracks);
            }
        }

        void ApplyActiveBinkVolumesLocked()
        {
            if (!activeBink ||
                !activeBinkSelection.load(std::memory_order_acquire) ||
                !originalSetVolume) {
                return;
            }

            const bool audible =
                VideoPlayer::GetSingleton().OriginalAudioAudible();
            appliedBinkAudioAudible = audible;
            for (const std::uint32_t track : selectedBinkTrackIds) {
                const std::int32_t sourceVolume =
                    SourceVolumeForSelectedTrackLocked(track);
                originalSetVolume(activeBink, track,
                                  audible ? ScaledBinkVolume(sourceVolume) : 0);
            }
            if (!selectedBinkTrackIds.empty()) {
                spdlog::debug(
                    "Applied {} MMVP volume {:.0f}% to {} selected BK2 "
                    "audio track(s)",
                    audible ? "audible" : "muted",
                    VideoPlayer::GetSingleton().Volume() * 100.0F,
                    selectedBinkTrackIds.size());
            }
        }

        void* DetachSecondaryBinkLocked(void* owner)
        {
            void* detached =
                activeBink && activeBink != owner ? activeBink : nullptr;
            activeBink = owner;
            activeBinkSelection.store(false, std::memory_order_release);
            activeBinkFrame.store({}, std::memory_order_release);
            for (auto& frame : activeBinkFramePool) {
                frame.reset();
            }
            activeBinkScaler.Reset();
            loggedFrameAllocationFailure = false;
            loggedFrameScalingInfo = false;
            loggedFrameScalingFailure = false;
            loggedFrameScalingFallback = false;
            selectedBinkTrackIds.clear();
            appliedBinkAudioAudible.reset();
            return detached;
        }

        void CloseDetachedBink(void* detached)
        {
            if (detached) {
                // BinkClose can synchronously call BinkPause/BinkWait. Do not
                // hold activeBinkMutex while it re-enters those hooks.
                originalClose(detached);
            }
        }

        bool ActivateSelection(const std::filesystem::path& selection)
        {
            void* owner = mainMenuBink.load(std::memory_order_acquire);
            if (!owner) {
                return false;
            }

            auto& player = VideoPlayer::GetSingleton();
            player.OnNativeVideoClosed();
            player.StopOverrideAudio();
            pendingOverrideAudioStart.store(false, std::memory_order_release);
            {
                std::scoped_lock lock(selectionMutex);
                pendingOverrideAudio.reset();
            }

            const bool binkSelection = IsBinkVideo(selection);
            std::optional<std::filesystem::path> overrideAudio;
            bool dedicatedAudio = false;
            if (!player.OriginalAudioPreferred()) {
                overrideAudio =
                    player.PickDedicatedAudioForVideo(selection, false);
                dedicatedAudio = overrideAudio.has_value();
            }
            if (!overrideAudio && !player.OriginalAudioPreferred() &&
                binkSelection) {
                overrideAudio = FindXwmSidecar(selection);
                if (overrideAudio &&
                    !player.HasDecodableAudioTrack(*overrideAudio)) {
                    spdlog::warn("Ignoring undecodable XWM sidecar: {}",
                                 Utf8Path(*overrideAudio));
                    overrideAudio.reset();
                }
            }

            if (binkSelection) {
                const std::string path = Utf8Path(selection);
                void* selectedBink =
                    originalOpen(path.c_str(), mainMenuOpenFlags);
                if (!selectedBink || !ValidBinkDimensions(selectedBink)) {
                    if (selectedBink) {
                        originalClose(selectedBink);
                    }
                    spdlog::warn(
                        "Could not open selected BK2 as an MMVP overlay: {}",
                        path);
                    return false;
                }

                const auto* header =
                    static_cast<const PublicBinkHeader*>(selectedBink);
                constexpr std::uint32_t kMaximumAudioTracks{ 64 };
                if (header->numberOfTracks > kMaximumAudioTracks) {
                    spdlog::warn(
                        "Rejected selected BK2 with invalid audio-track "
                        "count {}: {}",
                        header->numberOfTracks, path);
                    originalClose(selectedBink);
                    return false;
                }
                if (!overrideAudio && header->numberOfTracks == 0) {
                    overrideAudio =
                        player.PickDedicatedAudioForVideo(selection, true);
                    dedicatedAudio = overrideAudio.has_value();
                }
                player.SetOriginalAudioAudible(!overrideAudio.has_value());

                // Keep Bink's sound path running even while it is inaudible.
                // Some files use the audio clock to advance video frames and
                // freeze if BinkSetSoundOnOff disables that path entirely.
                originalSetSoundOnOff(selectedBink, 1);
                if (overrideAudio) {
                    if (dedicatedAudio) {
                        spdlog::info("Muting embedded BK2 tracks in favor of "
                                     "dedicated soundtrack: {}",
                                     Utf8Path(*overrideAudio));
                    } else {
                        spdlog::info(
                            "Muting embedded BK2 tracks in favor of XWM "
                            "sidecar: {}",
                            Utf8Path(*overrideAudio));
                    }
                }

                void* replaced = nullptr;
                {
                    std::scoped_lock lock(activeBinkMutex);
                    replaced = DetachSecondaryBinkLocked(owner);
                    activeBink = selectedBink;
                    activeBinkSelection.store(true, std::memory_order_release);
                    DiscoverSelectedBinkTracksLocked(selectedBink);
                    ApplyActiveBinkVolumesLocked();
                }
                CloseDetachedBink(replaced);
                spdlog::info("Opened BK2 overlay {} ({}x{}) over carrier {}",
                             path, header->width, header->height, owner);
            } else {
                if (!overrideAudio &&
                    !player.HasDecodableAudioTrack(selection)) {
                    overrideAudio =
                        player.PickDedicatedAudioForVideo(selection, true);
                    dedicatedAudio = overrideAudio.has_value();
                }
                player.SetOriginalAudioAudible(!overrideAudio.has_value());
                void* replaced = nullptr;
                {
                    std::scoped_lock lock(activeBinkMutex);
                    replaced = DetachSecondaryBinkLocked(owner);
                }
                CloseDetachedBink(replaced);
                player.OnNativeVideoOpened(
                    mainMenuWidth.load(std::memory_order_acquire),
                    mainMenuHeight.load(std::memory_order_acquire),
                    CurrentContentRect(
                        mainMenuWidth.load(std::memory_order_acquire),
                        mainMenuHeight.load(std::memory_order_acquire)),
                    selection);
            }

            {
                std::scoped_lock lock(selectionMutex);
                pendingOverrideAudio = overrideAudio;
            }
            pendingOverrideAudioStart.store(overrideAudio.has_value(),
                                            std::memory_order_release);
            if (overrideAudio) {
                spdlog::info("Queued {} main-menu audio override: {}",
                             dedicatedAudio ? "dedicated" : "sidecar",
                             Utf8Path(*overrideAudio));
            }

            SetCurrentSelection(selection);
            mainMenuStopped.store(false, std::memory_order_release);
            replaceMainMenuVideo.store(true, std::memory_order_release);
            loggedCopyCalls.store(0, std::memory_order_relaxed);
            ShowHelp(Config::MainMenuHelpMilliseconds());
            return true;
        }

        void StopActiveSelection()
        {
            void* owner = mainMenuBink.load(std::memory_order_acquire);
            void* detached = nullptr;
            {
                std::scoped_lock lock(activeBinkMutex);
                detached = DetachSecondaryBinkLocked(owner);
            }
            CloseDetachedBink(detached);
            VideoPlayer::GetSingleton().OnNativeVideoClosed();
            VideoPlayer::GetSingleton().StopOverrideAudio();
            pendingOverrideAudioStart.store(false, std::memory_order_release);
            {
                std::scoped_lock lock(selectionMutex);
                pendingOverrideAudio.reset();
            }
            mainMenuStopped.store(true, std::memory_order_release);
            replaceMainMenuVideo.store(true, std::memory_order_release);
            ShowHelp(Config::MainMenuHelpMilliseconds());
        }

        void ClearPendingOverrideAudio()
        {
            pendingOverrideAudioStart.store(false, std::memory_order_release);
            std::scoped_lock lock(selectionMutex);
            pendingOverrideAudio.reset();
        }

        void ApplySelectedBinkAudioState()
        {
            std::scoped_lock lock(activeBinkMutex);
            if (activeBink &&
                activeBinkSelection.load(std::memory_order_acquire)) {
                ApplyActiveBinkVolumesLocked();
            }
        }

        bool StartNextDedicatedAudio()
        {
            auto& player = VideoPlayer::GetSingleton();
            const auto selected = player.PickDedicatedAudio();
            if (!selected) {
                return false;
            }

            ClearPendingOverrideAudio();
            player.StopOverrideAudio();
            player.SetOriginalAudioPreferred(false);
            player.SetOriginalAudioAudible(false);
            ApplySelectedBinkAudioState();
            player.StartOverrideAudio(*selected);
            spdlog::info("Started next dedicated main-menu soundtrack: {}",
                         Utf8Path(*selected));
            return true;
        }

        void RestoreOriginalVideoAudio()
        {
            auto& player = VideoPlayer::GetSingleton();
            ClearPendingOverrideAudio();
            player.StopOverrideAudio();
            player.SetOriginalAudioPreferred(true);
            player.SetOriginalAudioAudible(true);
            ApplySelectedBinkAudioState();
            spdlog::info("Restored the selected video's original audio");
        }

        void CaptureActiveBinkFrameLocked(void* handle)
        {
            if (!handle ||
                !activeBinkSelection.load(std::memory_order_acquire) ||
                !ValidBinkDimensions(handle)) {
                return;
            }
            const auto* header = static_cast<const PublicBinkHeader*>(handle);
            const std::uint32_t outputWidth =
                mainMenuWidth.load(std::memory_order_acquire);
            const std::uint32_t outputHeight =
                mainMenuHeight.load(std::memory_order_acquire);
            const auto destination =
                CurrentContentRect(outputWidth, outputHeight);
            const BinkFrameScaler::CoverCrop crop =
                BinkFrameScaler::ComputeCoverCrop(header->width, header->height,
                                                  destination.width,
                                                  destination.height);
            if (crop.width == 0 || crop.height == 0 || outputWidth == 0 ||
                outputHeight == 0 || destination.width == 0 ||
                destination.height == 0 ||
                static_cast<std::uint64_t>(outputWidth) * outputHeight >
                    kMaximumCapturedPixels) {
                return;
            }

            std::shared_ptr<VideoFrame> frame;
            for (auto& candidate : activeBinkFramePool) {
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
                if (!loggedFrameAllocationFailure) {
                    loggedFrameAllocationFailure = true;
                    spdlog::warn(
                        "Skipping a selected BK2 frame because the bounded "
                        "capture pool is exhausted");
                }
                return;
            }
            frame->width = outputWidth;
            frame->height = outputHeight;
            frame->rowPitch = frame->width * 4;
            const auto byteCount =
                static_cast<std::size_t>(frame->rowPitch) * frame->height;
            try {
                frame->pixels.resize(byteCount);
            } catch (const std::bad_alloc&) {
                if (!loggedFrameAllocationFailure) {
                    loggedFrameAllocationFailure = true;
                    spdlog::error(
                        "Could not allocate {} bytes for selected BK2 frame "
                        "capture",
                        byteCount);
                }
                return;
            }

            constexpr std::uint32_t kCopyAll{ 0x80000000U };
            const bool directCopy = header->width == outputWidth &&
                                    header->height == outputHeight &&
                                    destination.x == 0 && destination.y == 0 &&
                                    destination.width == outputWidth &&
                                    destination.height == outputHeight;

            if (directCopy) {
                originalCopy(handle, frame->pixels.data(),
                             static_cast<std::int32_t>(frame->rowPitch),
                             frame->height, 0, 0, 0, 0, frame->width,
                             frame->height, kCopyAll | kSurface32);
            } else {
                // A pool is rebuilt whenever the selection changes. Vector
                // growth initializes the padding to black, and the stable
                // content rectangle never writes into it afterward.
                const auto sourceRowPitch =
                    static_cast<std::size_t>(header->width) * 4;
                const auto sourceByteCount =
                    sourceRowPitch * static_cast<std::size_t>(header->height);
                if (!activeBinkScaler.PrepareSource(header->width,
                                                    header->height)) {
                    if (!loggedFrameAllocationFailure) {
                        loggedFrameAllocationFailure = true;
                        spdlog::error(
                            "Could not allocate {} bytes for selected BK2 "
                            "frame capture scratch",
                            sourceByteCount);
                    }
                    return;
                }

                originalCopy(handle, activeBinkScaler.SourceData(),
                             static_cast<std::int32_t>(sourceRowPitch),
                             header->height, 0, 0, 0, 0, header->width,
                             header->height, kCopyAll | kSurface32);
                if (!activeBinkScaler.Scale(crop, destination, *frame)) {
                    if (!loggedFrameScalingFailure) {
                        loggedFrameScalingFailure = true;
                        spdlog::error(
                            "Could not scale selected BK2 frame "
                            "({}x{} to {}x{}): {}",
                            crop.width, crop.height, destination.width,
                            destination.height, activeBinkScaler.LastError());
                    }
                    return;
                }
                if (activeBinkScaler.FellBackToBicubic() &&
                    !loggedFrameScalingFallback) {
                    loggedFrameScalingFallback = true;
                    spdlog::warn(
                        "Spline36 was unavailable for selected BK2; falling "
                        "back to Bicubic: {}",
                        activeBinkScaler.LastError());
                }
                if (!loggedFrameScalingInfo) {
                    spdlog::info(
                        "Selected BK2 {} scaling: original {}x{}, cover crop "
                        "({}, {}) {}x{}, output {}x{}, content rect ({}, {}) "
                        "{}x{}{}",
                        ScalingAlgorithmName(
                            activeBinkScaler.ActiveAlgorithm()),
                        header->width, header->height, crop.x, crop.y,
                        crop.width, crop.height, outputWidth, outputHeight,
                        destination.x, destination.y, destination.width,
                        destination.height,
                        presentationWidth.load(std::memory_order_acquire) !=
                                    0 &&
                                presentationHeight.load(
                                    std::memory_order_acquire) != 0
                            ? std::format(" for client {}x{}",
                                          presentationWidth.load(
                                              std::memory_order_acquire),
                                          presentationHeight.load(
                                              std::memory_order_acquire))
                            : "");
                    loggedFrameScalingInfo = true;
                }
            }
            frame->serial =
                activeBinkFrameSerial.fetch_add(1, std::memory_order_relaxed);
            activeBinkFrame.store(std::move(frame), std::memory_order_release);
        }

        void* RoutedBinkLocked(void* handle)
        {
            return handle == mainMenuBink.load(std::memory_order_acquire) &&
                           activeBink
                       ? activeBink
                       : handle;
        }

        std::int32_t __stdcall HookedPause(void* handle,
                                           const std::int32_t paused)
        {
            std::scoped_lock lock(activeBinkMutex);
            return originalPause(RoutedBinkLocked(handle), paused);
        }

        void __stdcall HookedSetVolume(void* handle, const std::uint32_t track,
                                       const std::int32_t volume)
        {
            std::scoped_lock lock(activeBinkMutex);
            const bool mainMenuCall =
                handle == mainMenuBink.load(std::memory_order_acquire);
            if (mainMenuCall) {
                RememberCarrierTrackVolumeLocked(track, volume);
                // The carrier is only a presentation surface. Preserve its
                // audio clock, but never play a second copy of its soundtrack.
                originalSetVolume(handle, track, 0);
                ApplyActiveBinkVolumesLocked();
                return;
            }
            originalSetVolume(handle, track, volume);
        }

        std::int32_t __stdcall HookedDoFrame(void* handle)
        {
            std::scoped_lock lock(activeBinkMutex);
            void* routed = RoutedBinkLocked(handle);
            const std::int32_t result = originalDoFrame(routed);
            if (handle == mainMenuBink.load(std::memory_order_acquire)) {
                const bool audible =
                    VideoPlayer::GetSingleton().OriginalAudioAudible();
                if (!appliedBinkAudioAudible ||
                    *appliedBinkAudioAudible != audible) {
                    ApplyActiveBinkVolumesLocked();
                }
                CaptureActiveBinkFrameLocked(routed);
            }
            return result;
        }

        void __stdcall HookedNextFrame(void* handle)
        {
            std::scoped_lock lock(activeBinkMutex);
            originalNextFrame(RoutedBinkLocked(handle));
        }

        std::int32_t __stdcall HookedWait(void* handle)
        {
            std::scoped_lock lock(activeBinkMutex);
            return originalWait(RoutedBinkLocked(handle));
        }

        std::int32_t __stdcall HookedShouldSkip(void* handle)
        {
            std::scoped_lock lock(activeBinkMutex);
            return originalShouldSkip(RoutedBinkLocked(handle));
        }

        void* __stdcall HookedOpen(const char* name, const std::uint32_t flags)
        {
            const bool isMainMenuVideo = IsCarrierPath(name);
            std::optional<std::filesystem::path> selected;
            InputRouter::ClientSizeSnapshot client;
            if (isMainMenuVideo && !EngineSettings::Apply()) {
                spdlog::warn("Could not apply live Fallout settings before "
                             "opening the main-menu Bink");
            }
            if (isMainMenuVideo) {
                EngineSettings::BeginMainMenu();
                // The native window is the only version-independent display
                // signal available to this minimal F4SE plugin. Initialize
                // its cached client size before choosing the presentation
                // layout. Failure simply retains the carrier's aspect.
                InputRouter::Install();
                client = InputRouter::GetClientSize();
                selected = SelectMainMenuVideo(true);
            }

            // Fallout keeps this handle for the lifetime of MainMenu.swf. Keep
            // one aspect-matched BK2 open as a stable carrier and route its
            // playback calls to whichever file is currently selected. A
            // 16:9 carrier cannot fill an ultrawide Fallout presentation quad
            // merely by changing the rectangle copied inside it.
            CarrierChoice carrier;
            if (isMainMenuVideo) {
                carrier = OpenAspectMatchedCarrier(selected, client, flags);
            }
            void* handle = carrier.handle ? carrier.handle
                                          : originalOpen(name, flags);
            if (!handle && isMainMenuVideo) {
                EngineSettings::EndMainMenu();
            }
            if (!handle || !isMainMenuVideo) {
                return handle;
            }

            const auto* header = static_cast<const PublicBinkHeader*>(handle);
            const std::uint32_t width = header->width;
            const std::uint32_t height = header->height;
            if (width == 0 || height == 0 || width > 16384 || height > 16384 ||
                static_cast<std::uint64_t>(width) * height >
                    kMaximumCapturedPixels) {
                spdlog::warn(
                    "Opened the main-menu Bink, but its public dimensions "
                    "look invalid: {}x{}",
                    width, height);
                EngineSettings::EndMainMenu();
                return handle;
            }

            mainMenuOpenFlags = flags;
            mainMenuWidth.store(width, std::memory_order_release);
            mainMenuHeight.store(height, std::memory_order_release);
            presentationWidth.store(client.available ? client.width : 0,
                                    std::memory_order_release);
            presentationHeight.store(client.available ? client.height : 0,
                                     std::memory_order_release);
            mainMenuBink.store(handle, std::memory_order_release);
            if (carrier.handle) {
                // Keep Bink's audio clock alive for this handle, including
                // when an ordinary video is selected later, while silencing
                // the carrier's own embedded tracks.
                for (std::uint32_t index = 0;
                     index < header->numberOfTracks; ++index) {
                    originalSetVolume(handle,
                                      originalGetTrackID(handle, index), 0);
                }
            }
            void* staleSecondary = nullptr;
            {
                std::scoped_lock lock(activeBinkMutex);
                carrierTrackVolumes.clear();
                loggedTrackVolumeLimit = false;
                staleSecondary = DetachSecondaryBinkLocked(handle);
            }
            CloseDetachedBink(staleSecondary);
            mainMenuStopped.store(true, std::memory_order_release);
            replaceMainMenuVideo.store(true, std::memory_order_release);
            loggedCopyCalls.store(0, std::memory_order_relaxed);
            loggedUnsupportedSurface.store(false, std::memory_order_relaxed);
            InputRouter::Install();
            spdlog::info("Opened stable main-menu Bink carrier {} "
                         "({}x{}, flags {:08X}){}",
                         handle, width, height, flags,
                         carrier.handle
                             ? std::format(" from aspect-matched BK2 {}",
                                           Utf8Path(carrier.path))
                             : " from packaged loop");
            const auto content = CurrentContentRect(width, height);
            if (content.x != 0 || content.y != 0 || content.width != width ||
                content.height != height) {
                spdlog::info(
                    "Matching Fallout client {}x{} with carrier content "
                    "rect ({}, {}) {}x{}",
                    client.width, client.height, content.x, content.y,
                    content.width, content.height);
            }

            if (!selected || !ActivateSelection(*selected)) {
                if (selected) {
                    spdlog::warn(
                        "Could not activate initial main-menu selection {}; "
                        "trying an ordinary decoded video",
                        Utf8Path(*selected));
                }
                const auto fallback = SelectMainMenuVideo(false);
                if (!fallback || !ActivateSelection(*fallback)) {
                    SetCurrentSelection(
                        selected.value_or(std::filesystem::path(name)));
                    StopActiveSelection();
                    spdlog::warn(
                        "No playable main-menu media was available; "
                        "the carrier will be covered with a black frame");
                }
            }
            return handle;
        }

        void __stdcall HookedClose(void* handle)
        {
            void* expected = handle;
            if (mainMenuBink.compare_exchange_strong(
                    expected, nullptr, std::memory_order_acq_rel)) {
                void* detached = nullptr;
                {
                    std::scoped_lock lock(activeBinkMutex);
                    detached = DetachSecondaryBinkLocked(handle);
                    activeBink = nullptr;
                    carrierTrackVolumes.clear();
                    loggedTrackVolumeLimit = false;
                }
                CloseDetachedBink(detached);
                mainMenuOpenFlags = 0;
                mainMenuWidth.store(0, std::memory_order_release);
                mainMenuHeight.store(0, std::memory_order_release);
                presentationWidth.store(0, std::memory_order_release);
                presentationHeight.store(0, std::memory_order_release);
                replaceMainMenuVideo.store(false, std::memory_order_release);
                mainMenuStopped.store(false, std::memory_order_release);
                pendingOverrideAudioStart.store(false,
                                                std::memory_order_release);
                {
                    std::scoped_lock lock(selectionMutex);
                    pendingOverrideAudio.reset();
                    currentSelection.clear();
                }
                VideoPlayer::GetSingleton().OnNativeVideoClosed();
                VideoPlayer::GetSingleton().StopOverrideAudio();
                EngineSettings::EndMainMenu();
                spdlog::info("Released native main-menu Bink handle");
            }
            originalClose(handle);
        }

        void CopyVideoPixels(const VideoFrame& frame, void* destination,
                             const std::int32_t destinationPitch,
                             const std::uint32_t destinationHeight,
                             const std::uint32_t destinationX,
                             const std::uint32_t destinationY,
                             const std::uint32_t sourceX,
                             const std::uint32_t sourceY,
                             const std::uint32_t sourceWidth,
                             const std::uint32_t sourceHeight,
                             const std::uint32_t flags)
        {
            const auto result = BinkFrameCompositor::CopyCoverFrame(
                frame, destination, destinationPitch, destinationHeight,
                destinationX, destinationY, sourceX, sourceY, sourceWidth,
                sourceHeight, mainMenuWidth.load(std::memory_order_acquire),
                mainMenuHeight.load(std::memory_order_acquire), flags);
            if (result ==
                BinkFrameCompositor::CopyResult::kUnsupportedSurface) {
                if (!loggedUnsupportedSurface.exchange(
                        true, std::memory_order_relaxed)) {
                    spdlog::warn(
                        "Cannot replace Bink surface type {} (flags {:08X})",
                        flags & kSurfaceMask, flags);
                }
            }
        }

        void BlendHelpOverlay(
            void* destination, const std::int32_t destinationPitch,
            const std::uint32_t destinationHeight,
            const std::uint32_t destinationX, const std::uint32_t destinationY,
            const std::uint32_t sourceX, const std::uint32_t sourceY,
            const std::uint32_t sourceWidth, const std::uint32_t sourceHeight,
            const std::uint32_t flags)
        {
            if (!destination || destinationPitch == 0 ||
                destinationPitch == std::numeric_limits<std::int32_t>::min() ||
                destinationHeight == 0 ||
                GetTickCount64() >=
                    helpVisibleUntil.load(std::memory_order_acquire)) {
                return;
            }

            const std::uint32_t outputWidth =
                mainMenuWidth.load(std::memory_order_acquire);
            const std::uint32_t outputHeight =
                mainMenuHeight.load(std::memory_order_acquire);
            if (outputWidth == 0 || outputHeight == 0) {
                return;
            }
            const auto content = CurrentContentRect(outputWidth, outputHeight);
            if (content.width == 0 || content.height == 0) {
                return;
            }

            static std::mutex overlayMutex;
            static std::uint64_t cachedRevision = 0;
            static std::uint32_t cachedContentWidth = 0;
            static bool cachedAudioAudible = false;
            static VideoFrame overlay;
            std::scoped_lock lock(overlayMutex);
            const auto revision = helpRevision.load(std::memory_order_acquire);
            const bool audioAudible =
                VideoPlayer::GetSingleton().OriginalAudioAudible();
            if (cachedRevision != revision ||
                cachedContentWidth != content.width ||
                cachedAudioAudible != audioAudible) {
                overlay = BuildHelpOverlay(content.width);
                cachedRevision = revision;
                cachedContentWidth = content.width;
                cachedAudioAudible = audioAudible;
            }
            if (overlay.pixels.empty()) {
                return;
            }

            const std::uint32_t left =
                content.x + std::max(16U, content.width / 60U);
            const std::uint32_t top =
                content.y + std::max(16U, content.height / 34U);
            BinkFrameCompositor::BlendOverlay(
                overlay, left, top, destination, destinationPitch,
                destinationHeight, destinationX, destinationY, sourceX, sourceY,
                sourceWidth, sourceHeight, flags);
        }

        std::int32_t __stdcall HookedCopy(void* handle, void* destination,
                                          const std::int32_t destinationPitch,
                                          const std::uint32_t destinationHeight,
                                          const std::uint32_t destinationX,
                                          const std::uint32_t destinationY,
                                          const std::uint32_t sourceX,
                                          const std::uint32_t sourceY,
                                          const std::uint32_t sourceWidth,
                                          const std::uint32_t sourceHeight,
                                          const std::uint32_t flags)
        {
            const bool isMainMenu =
                handle == mainMenuBink.load(std::memory_order_acquire);
            const bool selectedBink =
                isMainMenu &&
                activeBinkSelection.load(std::memory_order_acquire);
            const std::int32_t result =
                selectedBink
                    ? 0
                    : originalCopy(handle, destination, destinationPitch,
                                   destinationHeight, destinationX,
                                   destinationY, sourceX, sourceY, sourceWidth,
                                   sourceHeight, flags);

            if (!isMainMenu) {
                return result;
            }
            if (pendingOverrideAudioStart.exchange(false,
                                                   std::memory_order_acq_rel)) {
                std::optional<std::filesystem::path> overrideAudio;
                {
                    std::scoped_lock lock(selectionMutex);
                    overrideAudio = pendingOverrideAudio;
                }
                if (overrideAudio) {
                    VideoPlayer::GetSingleton().StartOverrideAudio(
                        *overrideAudio);
                }
            }

            if (replaceMainMenuVideo.load(std::memory_order_acquire)) {
                const std::uint32_t call =
                    loggedCopyCalls.fetch_add(1, std::memory_order_relaxed);
                if (call < 3) {
                    spdlog::info(
                        "Main-menu Bink copy #{}: pitch {}, buffer height {}, "
                        "dst ({}, {}), src ({}, {}) {}x{}, surface {}, "
                        "flags {:08X}",
                        call + 1, destinationPitch, destinationHeight,
                        destinationX, destinationY, sourceX, sourceY,
                        sourceWidth, sourceHeight, flags & kSurfaceMask, flags);
                }

                const auto frame =
                    selectedBink
                        ? activeBinkFrame.load(std::memory_order_acquire)
                        : VideoPlayer::GetSingleton().GetLatestFrame();
                static const VideoFrame blackFrame{ .pixels = { 0, 0, 0, 255 },
                                                    .width = 1,
                                                    .height = 1,
                                                    .rowPitch = 4,
                                                    .serial = 0 };
                CopyVideoPixels(
                    frame && !mainMenuStopped.load(std::memory_order_acquire)
                        ? *frame
                        : blackFrame,
                    destination, destinationPitch, destinationHeight,
                    destinationX, destinationY, sourceX, sourceY, sourceWidth,
                    sourceHeight, flags);
            }

            BlendHelpOverlay(destination, destinationPitch, destinationHeight,
                             destinationX, destinationY, sourceX, sourceY,
                             sourceWidth, sourceHeight, flags);
            return result;
        }

        bool CreateHook(HMODULE module, const char* exportName, void* hook,
                        void** original, void*& target, bool& created)
        {
            created = false;
            target =
                reinterpret_cast<void*>(GetProcAddress(module, exportName));
            if (!target) {
                spdlog::error("Could not locate {} in bink2w64.dll",
                              exportName);
                return false;
            }
            const MH_STATUS result = MH_CreateHook(target, hook, original);
            if (result != MH_OK) {
                spdlog::error("MH_CreateHook({}) failed: {}", exportName,
                              MH_StatusToString(result));
                return false;
            }
            created = true;
            return true;
        }

        void RollBackHooks(const bool uninitialize)
        {
            bool cleanupSucceeded = true;
            for (std::size_t index = hookTargets.size(); index-- > 0;) {
                if (hookEnabled[index]) {
                    const MH_STATUS result = MH_DisableHook(hookTargets[index]);
                    if (result == MH_OK || result == MH_ERROR_DISABLED) {
                        hookEnabled[index] = false;
                    } else {
                        cleanupSucceeded = false;
                        spdlog::error(
                            "MH_DisableHook({}) failed during rollback: {}",
                            hookTargets[index], MH_StatusToString(result));
                    }
                }
                if (hookCreated[index] && !hookEnabled[index]) {
                    const MH_STATUS result = MH_RemoveHook(hookTargets[index]);
                    if (result == MH_OK || result == MH_ERROR_NOT_CREATED) {
                        hookCreated[index] = false;
                    } else {
                        cleanupSucceeded = false;
                        spdlog::error(
                            "MH_RemoveHook({}) failed during rollback: {}",
                            hookTargets[index], MH_StatusToString(result));
                    }
                }
                if (!hookCreated[index] && !hookEnabled[index]) {
                    hookTargets[index] = nullptr;
                }
            }
            if (uninitialize && cleanupSucceeded) {
                const MH_STATUS result = MH_Uninitialize();
                if (result != MH_OK && result != MH_ERROR_NOT_INITIALIZED) {
                    spdlog::error("MH_Uninitialize failed during rollback: {}",
                                  MH_StatusToString(result));
                }
            } else if (uninitialize && !cleanupSucceeded) {
                spdlog::error(
                    "Retaining MinHook state because Bink-hook rollback was "
                    "incomplete");
            }
        }
    } // namespace

    bool HandleWindowMessage(const UINT message, const WPARAM wParam,
                             const LPARAM lParam)
    {
        if (message != WM_KEYDOWN ||
            !mainMenuBink.load(std::memory_order_acquire)) {
            return false;
        }

        const auto key = static_cast<std::uint32_t>(wParam);
        const bool repeated = (lParam & (1LL << 30)) != 0;
        auto& player = VideoPlayer::GetSingleton();

        if (Config::MainMenuNextKey() != 0 &&
            key == Config::MainMenuNextKey()) {
            if (!repeated) {
                const auto selected = SelectMainMenuVideo(true);
                if (!selected) {
                    spdlog::warn(
                        "The next-video hotkey found no main-menu media");
                } else if (ActivateSelection(*selected)) {
                    spdlog::info("Main-menu next hotkey selected {}",
                                 Utf8Path(*selected));
                } else {
                    spdlog::warn("The next-video hotkey could not activate {}",
                                 Utf8Path(*selected));
                }
            }
            return true;
        }

        if (Config::MainMenuStopKey() != 0 &&
            key == Config::MainMenuStopKey()) {
            if (!repeated) {
                StopActiveSelection();
                spdlog::info("Main-menu playback stopped by hotkey");
            }
            return true;
        }

        if (Config::MainMenuNextAudioKey() != 0 &&
            key == Config::MainMenuNextAudioKey()) {
            if (!repeated && !mainMenuStopped.load(std::memory_order_acquire)) {
                if (!StartNextDedicatedAudio()) {
                    spdlog::warn(
                        "The next-soundtrack hotkey found no supported "
                        "audio sources");
                }
                if (Config::MainMenuHelpMilliseconds() != 0) {
                    ShowHelp(3000);
                }
            }
            return true;
        }

        if (Config::MainMenuToggleOriginalAudioKey() != 0 &&
            key == Config::MainMenuToggleOriginalAudioKey()) {
            if (!repeated && !mainMenuStopped.load(std::memory_order_acquire)) {
                if (player.OriginalAudioPreferred()) {
                    if (!StartNextDedicatedAudio()) {
                        spdlog::warn("Cannot switch to dedicated audio because "
                                     "the audio library is empty");
                    }
                } else {
                    RestoreOriginalVideoAudio();
                }
                if (Config::MainMenuHelpMilliseconds() != 0) {
                    ShowHelp(3000);
                }
            }
            return true;
        }

        if (Config::MainMenuVolumeUpKey() != 0 &&
            key == Config::MainMenuVolumeUpKey()) {
            player.AdjustVolume(Config::MainMenuVolumeStep());
            {
                std::scoped_lock lock(activeBinkMutex);
                ApplyActiveBinkVolumesLocked();
            }
            if (Config::MainMenuHelpMilliseconds() != 0) {
                ShowHelp(2000);
            }
            return true;
        }

        if (Config::MainMenuVolumeDownKey() != 0 &&
            key == Config::MainMenuVolumeDownKey()) {
            player.AdjustVolume(-Config::MainMenuVolumeStep());
            {
                std::scoped_lock lock(activeBinkMutex);
                ApplyActiveBinkVolumesLocked();
            }
            if (Config::MainMenuHelpMilliseconds() != 0) {
                ShowHelp(2000);
            }
            return true;
        }

        return false;
    }

    bool Install()
    {
        if (std::ranges::any_of(hookEnabled,
                                [](const bool enabled) { return enabled; })) {
            spdlog::info("Native Bink hooks are already installed");
            return true;
        }
        hookTargets.fill(nullptr);
        hookCreated.fill(false);
        hookEnabled.fill(false);

        const MH_STATUS initializeResult = MH_Initialize();
        const bool ownsMinHook = initializeResult == MH_OK;
        if (initializeResult != MH_OK &&
            initializeResult != MH_ERROR_ALREADY_INITIALIZED) {
            spdlog::error("MH_Initialize failed: {}",
                          MH_StatusToString(initializeResult));
            return false;
        }

        HMODULE bink = GetModuleHandleW(L"bink2w64.dll");
        if (!bink) {
            bink = LoadLibraryW(L"bink2w64.dll");
        }
        if (!bink) {
            spdlog::error("Could not load bink2w64.dll");
            RollBackHooks(ownsMinHook);
            return false;
        }

        originalGetTrackID = reinterpret_cast<BinkGetTrackID>(
            GetProcAddress(bink, "BinkGetTrackID"));
        originalSetSoundOnOff = reinterpret_cast<BinkSetSoundOnOff>(
            GetProcAddress(bink, "BinkSetSoundOnOff"));
        if (!originalGetTrackID || !originalSetSoundOnOff) {
            spdlog::error("Could not locate BinkGetTrackID or "
                          "BinkSetSoundOnOff in bink2w64.dll");
            RollBackHooks(ownsMinHook);
            return false;
        }

        if (!CreateHook(bink, "BinkOpen", reinterpret_cast<void*>(&HookedOpen),
                        reinterpret_cast<void**>(&originalOpen), hookTargets[0],
                        hookCreated[0]) ||
            !CreateHook(bink, "BinkClose",
                        reinterpret_cast<void*>(&HookedClose),
                        reinterpret_cast<void**>(&originalClose),
                        hookTargets[1], hookCreated[1]) ||
            !CreateHook(bink, "BinkCopyToBufferRect",
                        reinterpret_cast<void*>(&HookedCopy),
                        reinterpret_cast<void**>(&originalCopy), hookTargets[2],
                        hookCreated[2]) ||
            !CreateHook(bink, "BinkPause",
                        reinterpret_cast<void*>(&HookedPause),
                        reinterpret_cast<void**>(&originalPause),
                        hookTargets[3], hookCreated[3]) ||
            !CreateHook(bink, "BinkSetVolume",
                        reinterpret_cast<void*>(&HookedSetVolume),
                        reinterpret_cast<void**>(&originalSetVolume),
                        hookTargets[4], hookCreated[4]) ||
            !CreateHook(bink, "BinkDoFrame",
                        reinterpret_cast<void*>(&HookedDoFrame),
                        reinterpret_cast<void**>(&originalDoFrame),
                        hookTargets[5], hookCreated[5]) ||
            !CreateHook(bink, "BinkNextFrame",
                        reinterpret_cast<void*>(&HookedNextFrame),
                        reinterpret_cast<void**>(&originalNextFrame),
                        hookTargets[6], hookCreated[6]) ||
            !CreateHook(bink, "BinkWait", reinterpret_cast<void*>(&HookedWait),
                        reinterpret_cast<void**>(&originalWait), hookTargets[7],
                        hookCreated[7]) ||
            !CreateHook(bink, "BinkShouldSkip",
                        reinterpret_cast<void*>(&HookedShouldSkip),
                        reinterpret_cast<void**>(&originalShouldSkip),
                        hookTargets[8], hookCreated[8])) {
            RollBackHooks(ownsMinHook);
            return false;
        }

        for (std::size_t index = 0; index < hookTargets.size(); ++index) {
            void* target = hookTargets[index];
            const MH_STATUS enableResult = MH_EnableHook(target);
            if (enableResult != MH_OK && enableResult != MH_ERROR_ENABLED) {
                spdlog::error("MH_EnableHook({}) failed: {}", target,
                              MH_StatusToString(enableResult));
                RollBackHooks(ownsMinHook);
                return false;
            }
            hookEnabled[index] = true;
        }

        spdlog::info("Installed native Bink carrier-overlay hooks from {}",
                     reinterpret_cast<void*>(bink));
        return true;
    }
} // namespace BinkHook
