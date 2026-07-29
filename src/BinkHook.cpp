#include "PCH.h"

#include "BinkHook.h"
#include "Config.h"
#include "EngineSettings.h"
#include "InputRouter.h"
#include "MediaLibrary.h"
#include "VideoPlayer.h"
#include "WorldTextureBridge.h"

#include <MinHook.h>

namespace BinkHook
{
    namespace
    {
        using BinkOpen = void* (__stdcall*)(const char*, std::uint32_t);
        using BinkClose = void (__stdcall*)(void*);
        using BinkPause = std::int32_t (__stdcall*)(
            void*,
            std::int32_t);
        using BinkSetVolume = std::int32_t (__stdcall*)(
            void*,
            std::uint32_t,
            std::int32_t);
        using BinkDoFrame = std::int32_t (__stdcall*)(void*);
        using BinkNextFrame = void (__stdcall*)(void*);
        using BinkWait = std::int32_t (__stdcall*)(void*);
        using BinkShouldSkip = std::int32_t (__stdcall*)(void*);
        using BinkCopyToBufferRect = std::int32_t (__stdcall*)(
            void*,
            void*,
            std::int32_t,
            std::uint32_t,
            std::uint32_t,
            std::uint32_t,
            std::uint32_t,
            std::uint32_t,
            std::uint32_t,
            std::uint32_t,
            std::uint32_t);

        struct PublicBinkHeader
        {
            std::uint32_t width;
            std::uint32_t height;
        };

        constexpr std::uint32_t kSurfaceMask{ 15 };
        constexpr std::uint32_t kSurface24{ 1 };
        constexpr std::uint32_t kSurface24Reversed{ 2 };
        constexpr std::uint32_t kSurface32{ 3 };
        constexpr std::uint32_t kSurface32Reversed{ 4 };
        constexpr std::uint32_t kSurface32Alpha{ 5 };
        constexpr std::uint32_t kSurface32ReversedAlpha{ 6 };

        BinkOpen originalOpen{ nullptr };
        BinkClose originalClose{ nullptr };
        BinkPause originalPause{ nullptr };
        BinkSetVolume originalSetVolume{ nullptr };
        BinkDoFrame originalDoFrame{ nullptr };
        BinkNextFrame originalNextFrame{ nullptr };
        BinkWait originalWait{ nullptr };
        BinkShouldSkip originalShouldSkip{ nullptr };
        BinkCopyToBufferRect originalCopy{ nullptr };
        std::array<void*, 9> hookTargets{};

        std::atomic<void*> mainMenuBink{ nullptr };
        void* activeBink{ nullptr };
        std::mutex activeBinkMutex;
        std::uint32_t mainMenuOpenFlags{ 0 };
        std::atomic<bool> activeBinkSelection{ false };
        std::atomic<std::shared_ptr<const VideoFrame>> activeBinkFrame;
        std::atomic<std::uint64_t> activeBinkFrameSerial{ 1 };
        std::atomic<std::uint32_t> mainMenuWidth{ 0 };
        std::atomic<std::uint32_t> mainMenuHeight{ 0 };
        std::atomic<std::uint32_t> loggedCopyCalls{ 0 };
        std::atomic<bool> loggedUnsupportedSurface{ false };
        std::atomic<bool> replaceMainMenuVideo{ false };
        std::atomic<bool> mainMenuStopped{ false };
        std::atomic<bool> pendingSidecarStart{ false };
        std::atomic<std::uint64_t> helpVisibleUntil{ 0 };
        std::atomic<std::uint64_t> helpRevision{ 1 };
        std::mutex selectionMutex;
        std::filesystem::path previousSelection;
        std::filesystem::path currentSelection;
        std::optional<std::filesystem::path> pendingSidecar;
        std::mt19937_64 selectionRandom{ std::random_device{}() };

        struct ScaleMap
        {
            void Update(
                const VideoFrame& frame,
                const std::uint32_t outputWidth,
                const std::uint32_t outputHeight)
            {
                if (inputWidth == frame.width &&
                    inputHeight == frame.height &&
                    targetWidth == outputWidth &&
                    targetHeight == outputHeight) {
                    return;
                }

                inputWidth = frame.width;
                inputHeight = frame.height;
                targetWidth = outputWidth;
                targetHeight = outputHeight;

                std::uint32_t cropX = 0;
                std::uint32_t cropY = 0;
                std::uint32_t cropWidth = frame.width;
                std::uint32_t cropHeight = frame.height;
                if (static_cast<std::uint64_t>(frame.width) *
                        outputHeight >
                    static_cast<std::uint64_t>(outputWidth) *
                        frame.height) {
                    cropWidth = static_cast<std::uint32_t>(
                        static_cast<std::uint64_t>(frame.height) *
                        outputWidth / outputHeight);
                    cropX = (frame.width - cropWidth) / 2;
                } else {
                    cropHeight = static_cast<std::uint32_t>(
                        static_cast<std::uint64_t>(frame.width) *
                        outputHeight / outputWidth);
                    cropY = (frame.height - cropHeight) / 2;
                }

                x.resize(outputWidth);
                y.resize(outputHeight);
                for (std::uint32_t column = 0;
                     column < outputWidth;
                     ++column) {
                    x[column] = std::min(
                        cropX + static_cast<std::uint32_t>(
                            static_cast<std::uint64_t>(column) *
                            cropWidth / outputWidth),
                        frame.width - 1);
                }
                for (std::uint32_t row = 0;
                     row < outputHeight;
                     ++row) {
                    y[row] = std::min(
                        cropY + static_cast<std::uint32_t>(
                            static_cast<std::uint64_t>(row) *
                            cropHeight / outputHeight),
                        frame.height - 1);
                }
            }

            [[nodiscard]] bool IsIdentity() const noexcept
            {
                return inputWidth == targetWidth &&
                       inputHeight == targetHeight;
            }

            std::uint32_t inputWidth{ 0 };
            std::uint32_t inputHeight{ 0 };
            std::uint32_t targetWidth{ 0 };
            std::uint32_t targetHeight{ 0 };
            std::vector<std::uint32_t> x;
            std::vector<std::uint32_t> y;
        };

        thread_local ScaleMap scaleMap;

        bool IsMainMenuVideo(const char* name)
        {
            if (!name) {
                return false;
            }

            constexpr std::size_t kMaximumPathLength{ 4096 };
            const std::size_t length = strnlen(name, kMaximumPathLength);
            if (length == 0 || length == kMaximumPathLength) {
                return false;
            }

            std::string path(name, length);
            std::ranges::transform(
                path,
                path.begin(),
                [](const unsigned char character) {
                    return static_cast<char>(std::tolower(character));
                });
            return path.contains("mainmenuloop.bk2");
        }

        bool IsBinkVideo(const std::filesystem::path& path)
        {
            std::string extension = path.extension().string();
            std::ranges::transform(
                extension,
                extension.begin(),
                [](const unsigned char character) {
                    return static_cast<char>(std::tolower(character));
                });
            return extension == ".bk2";
        }

        std::string Utf8Path(const std::filesystem::path& path)
        {
            const auto utf8 = path.u8string();
            return {
                reinterpret_cast<const char*>(utf8.data()),
                utf8.size()
            };
        }

        std::vector<std::filesystem::path> FindBinkVideos(
            const std::filesystem::path& directory)
        {
            std::vector<std::filesystem::path> videos;
            std::error_code error;
            const auto append =
                [&](const std::filesystem::directory_entry& entry) {
                    std::error_code entryError;
                    if (entry.is_regular_file(entryError) &&
                        !entryError &&
                        IsBinkVideo(entry.path())) {
                        videos.push_back(
                            entry.path().lexically_normal());
                    }
                };
            if (Config::RecursiveMediaScan()) {
                for (std::filesystem::recursive_directory_iterator iterator(
                         directory,
                         std::filesystem::directory_options::
                             skip_permission_denied,
                         error),
                     end;
                     !error && iterator != end;
                     iterator.increment(error)) {
                    append(*iterator);
                }
            } else {
                for (std::filesystem::directory_iterator iterator(
                         directory,
                         std::filesystem::directory_options::
                             skip_permission_denied,
                         error),
                     end;
                     !error && iterator != end;
                     iterator.increment(error)) {
                    append(*iterator);
                }
            }
            return videos;
        }

        std::optional<std::filesystem::path> SelectMainMenuVideo(
            const bool includeBink = true)
        {
            auto directory = Config::MainMenuDirectory();
            MediaLibrary ordinaryLibrary(
                directory,
                Config::RecursiveMediaScan());
            auto ordinary = ordinaryLibrary.Scan();
            auto bink = includeBink ?
                FindBinkVideos(directory) :
                std::vector<std::filesystem::path>{};
            if (ordinary.empty() && bink.empty() &&
                directory != std::filesystem::path(
                    "Data/MainMenuVideos")) {
                directory = "Data/MainMenuVideos";
                ordinaryLibrary.SetRoot(directory);
                ordinary = ordinaryLibrary.Scan();
                bink = includeBink ?
                    FindBinkVideos(directory) :
                    std::vector<std::filesystem::path>{};
            }

            std::vector<std::filesystem::path> candidates;
            candidates.reserve(ordinary.size() + bink.size());
            std::ranges::move(ordinary, std::back_inserter(candidates));
            std::ranges::move(bink, std::back_inserter(candidates));
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

        bool EqualsInsensitive(
            const std::wstring_view left,
            const std::wstring_view right)
        {
            return std::ranges::equal(
                left,
                right,
                [](const wchar_t a, const wchar_t b) {
                    return std::towlower(a) == std::towlower(b);
                });
        }

        std::optional<std::filesystem::path> FindXwmSidecar(
            const std::filesystem::path& video)
        {
            auto direct = video;
            direct.replace_extension(L".xwm");
            std::error_code error;
            if (std::filesystem::is_regular_file(direct, error) && !error) {
                return direct.lexically_normal();
            }

            error.clear();
            const auto parent = video.parent_path();
            for (std::filesystem::directory_iterator iterator(
                     parent,
                     std::filesystem::directory_options::
                         skip_permission_denied,
                     error),
                 end;
                 !error && iterator != end;
                 iterator.increment(error)) {
                std::error_code entryError;
                if (!iterator->is_regular_file(entryError) || entryError) {
                    continue;
                }
                const auto& candidate = iterator->path();
                if (EqualsInsensitive(
                        candidate.extension().wstring(),
                        L".xwm") &&
                    EqualsInsensitive(
                        candidate.stem().wstring(),
                        video.stem().wstring())) {
                    return candidate.lexically_normal();
                }
            }
            return std::nullopt;
        }

        void SetCurrentSelection(
            const std::filesystem::path& selection)
        {
            std::scoped_lock lock(selectionMutex);
            currentSelection = selection;
        }

        void ShowHelp(const std::uint32_t milliseconds)
        {
            helpVisibleUntil.store(
                GetTickCount64() + milliseconds,
                std::memory_order_release);
            helpRevision.fetch_add(1, std::memory_order_release);
        }

        std::string VirtualKeyName(const std::uint32_t key)
        {
            switch (key) {
            case 0: return "DISABLED";
            case VK_BACK: return "BACKSPACE";
            case VK_TAB: return "TAB";
            case VK_PRIOR: return "PAGE UP";
            case VK_NEXT: return "PAGE DOWN";
            case VK_SPACE: return "SPACE";
            case VK_ESCAPE: return "ESC";
            default:
                if (key >= 'A' && key <= 'Z') {
                    return std::string(
                        1,
                        static_cast<char>(key));
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
            case 'A': return { 14, 17, 17, 31, 17, 17, 17 };
            case 'B': return { 30, 17, 17, 30, 17, 17, 30 };
            case 'C': return { 14, 17, 16, 16, 16, 17, 14 };
            case 'D': return { 30, 17, 17, 17, 17, 17, 30 };
            case 'E': return { 31, 16, 16, 30, 16, 16, 31 };
            case 'F': return { 31, 16, 16, 30, 16, 16, 16 };
            case 'G': return { 14, 17, 16, 23, 17, 17, 15 };
            case 'H': return { 17, 17, 17, 31, 17, 17, 17 };
            case 'I': return { 31, 4, 4, 4, 4, 4, 31 };
            case 'J': return { 1, 1, 1, 1, 17, 17, 14 };
            case 'K': return { 17, 18, 20, 24, 20, 18, 17 };
            case 'L': return { 16, 16, 16, 16, 16, 16, 31 };
            case 'M': return { 17, 27, 21, 21, 17, 17, 17 };
            case 'N': return { 17, 25, 21, 19, 17, 17, 17 };
            case 'O': return { 14, 17, 17, 17, 17, 17, 14 };
            case 'P': return { 30, 17, 17, 30, 16, 16, 16 };
            case 'Q': return { 14, 17, 17, 17, 21, 18, 13 };
            case 'R': return { 30, 17, 17, 30, 20, 18, 17 };
            case 'S': return { 15, 16, 16, 14, 1, 1, 30 };
            case 'T': return { 31, 4, 4, 4, 4, 4, 4 };
            case 'U': return { 17, 17, 17, 17, 17, 17, 14 };
            case 'V': return { 17, 17, 17, 17, 17, 10, 4 };
            case 'W': return { 17, 17, 17, 21, 21, 21, 10 };
            case 'X': return { 17, 17, 10, 4, 10, 17, 17 };
            case 'Y': return { 17, 17, 10, 4, 4, 4, 4 };
            case 'Z': return { 31, 1, 2, 4, 8, 16, 31 };
            case '0': return { 14, 17, 19, 21, 25, 17, 14 };
            case '1': return { 4, 12, 4, 4, 4, 4, 14 };
            case '2': return { 14, 17, 1, 2, 4, 8, 31 };
            case '3': return { 30, 1, 1, 14, 1, 1, 30 };
            case '4': return { 2, 6, 10, 18, 31, 2, 2 };
            case '5': return { 31, 16, 16, 30, 1, 1, 30 };
            case '6': return { 14, 16, 16, 30, 17, 17, 14 };
            case '7': return { 31, 1, 2, 4, 8, 8, 8 };
            case '8': return { 14, 17, 17, 14, 17, 17, 14 };
            case '9': return { 14, 17, 17, 15, 1, 1, 14 };
            case '.': return { 0, 0, 0, 0, 0, 12, 12 };
            case '-': return { 0, 0, 0, 31, 0, 0, 0 };
            case '_': return { 0, 0, 0, 0, 0, 0, 31 };
            case '/': return { 1, 2, 2, 4, 8, 8, 16 };
            case '\\': return { 16, 8, 8, 4, 2, 2, 1 };
            case ':': return { 0, 12, 12, 0, 12, 12, 0 };
            case '%': return { 17, 2, 4, 8, 16, 17, 0 };
            case ' ': return {};
            default: return { 14, 17, 1, 2, 4, 0, 4 };
            }
        }

        void OverlayRectangle(
            VideoFrame& frame,
            int left,
            int top,
            int right,
            int bottom,
            const std::uint8_t blue,
            const std::uint8_t green,
            const std::uint8_t red,
            const std::uint8_t alpha)
        {
            left = std::clamp(left, 0, static_cast<int>(frame.width));
            right = std::clamp(right, 0, static_cast<int>(frame.width));
            top = std::clamp(top, 0, static_cast<int>(frame.height));
            bottom = std::clamp(bottom, 0, static_cast<int>(frame.height));
            for (int y = top; y < bottom; ++y) {
                for (int x = left; x < right; ++x) {
                    auto* pixel = frame.pixels.data() +
                        (static_cast<std::size_t>(y) * frame.width + x) * 4;
                    pixel[0] = blue;
                    pixel[1] = green;
                    pixel[2] = red;
                    pixel[3] = alpha;
                }
            }
        }

        void OverlayText(
            VideoFrame& frame,
            const int left,
            const int top,
            const std::string_view text,
            const int scale)
        {
            int x = left;
            for (const char character : text) {
                const auto glyph = Glyph(character);
                for (int row = 0; row < 7; ++row) {
                    for (int column = 0; column < 5; ++column) {
                        if ((glyph[row] & (1U << (4 - column))) != 0) {
                            OverlayRectangle(
                                frame,
                                x + column * scale,
                                top + row * scale,
                                x + (column + 1) * scale,
                                top + (row + 1) * scale,
                                210,
                                255,
                                210,
                                255);
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
            const std::string status = mainMenuStopped.load(
                std::memory_order_acquire) ?
                "PLAYBACK STOPPED" :
                std::format(
                    "NOW PLAYING: {}",
                    selection.filename().string());
            const std::array<std::string, 5> lines{
                status,
                std::format(
                    "{}  NEW RANDOM VIDEO",
                    VirtualKeyName(Config::MainMenuNextKey())),
                std::format(
                    "{}  STOP VIDEO",
                    VirtualKeyName(Config::MainMenuStopKey())),
                std::format(
                    "{} / {}  VOLUME",
                    VirtualKeyName(Config::MainMenuVolumeUpKey()),
                    VirtualKeyName(Config::MainMenuVolumeDownKey())),
                std::format(
                    "VOLUME: {:.0f}%",
                    VideoPlayer::GetSingleton().Volume() * 100.0F)
            };
            const int scale = outputWidth >= 1600 ? 3 : 2;
            std::size_t maximumCharacters = 0;
            for (const auto& line : lines) {
                maximumCharacters =
                    std::max(maximumCharacters, line.size());
            }
            VideoFrame frame{};
            frame.width = std::min(
                outputWidth > 48 ? outputWidth - 48 : outputWidth,
                static_cast<std::uint32_t>(
                    maximumCharacters * 6 * scale + 36));
            frame.height = static_cast<std::uint32_t>(
                28 + lines.size() * (7 * scale + 10));
            frame.rowPitch = frame.width * 4;
            frame.pixels.assign(
                static_cast<std::size_t>(frame.rowPitch) * frame.height,
                0);
            OverlayRectangle(
                frame,
                0,
                0,
                static_cast<int>(frame.width),
                static_cast<int>(frame.height),
                0,
                0,
                0,
                255);
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
            const auto* header =
                static_cast<const PublicBinkHeader*>(handle);
            return header->width > 0 &&
                   header->height > 0 &&
                   header->width <= 16384 &&
                   header->height <= 16384;
        }

        void* DetachSecondaryBinkLocked(void* owner)
        {
            void* detached =
                activeBink && activeBink != owner ?
                    activeBink :
                    nullptr;
            activeBink = owner;
            activeBinkSelection.store(false, std::memory_order_release);
            activeBinkFrame.store({}, std::memory_order_release);
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

        bool ActivateSelection(
            const std::filesystem::path& selection)
        {
            void* owner =
                mainMenuBink.load(std::memory_order_acquire);
            if (!owner) {
                return false;
            }

            auto& player = VideoPlayer::GetSingleton();
            player.OnNativeVideoClosed();
            player.StopSidecarAudio();
            pendingSidecarStart.store(false, std::memory_order_release);
            {
                std::scoped_lock lock(selectionMutex);
                pendingSidecar.reset();
            }

            if (IsBinkVideo(selection)) {
                const std::string path = Utf8Path(selection);
                void* selectedBink = originalOpen(
                    path.c_str(),
                    mainMenuOpenFlags);
                if (!selectedBink || !ValidBinkDimensions(selectedBink)) {
                    if (selectedBink) {
                        originalClose(selectedBink);
                    }
                    spdlog::warn(
                        "Could not open selected BK2 as an MMVP overlay: {}",
                        path);
                    return false;
                }

                void* replaced = nullptr;
                {
                    std::scoped_lock lock(activeBinkMutex);
                    replaced = DetachSecondaryBinkLocked(owner);
                    activeBink = selectedBink;
                    activeBinkSelection.store(
                        true,
                        std::memory_order_release);
                }
                CloseDetachedBink(replaced);
                const auto* header =
                    static_cast<const PublicBinkHeader*>(selectedBink);
                spdlog::info(
                    "Opened BK2 overlay {} ({}x{}) over carrier {}",
                    path,
                    header->width,
                    header->height,
                    owner);

                const auto sidecar = FindXwmSidecar(selection);
                {
                    std::scoped_lock lock(selectionMutex);
                    pendingSidecar = sidecar;
                }
                pendingSidecarStart.store(
                    sidecar.has_value(),
                    std::memory_order_release);
                if (sidecar) {
                    spdlog::info(
                        "Matched XWM sidecar for BK2 overlay: {}",
                        Utf8Path(*sidecar));
                }
            } else {
                void* replaced = nullptr;
                {
                    std::scoped_lock lock(activeBinkMutex);
                    replaced = DetachSecondaryBinkLocked(owner);
                }
                CloseDetachedBink(replaced);
                player.OnNativeVideoOpened(
                    mainMenuWidth.load(std::memory_order_acquire),
                    mainMenuHeight.load(std::memory_order_acquire),
                    selection);
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
            void* owner =
                mainMenuBink.load(std::memory_order_acquire);
            void* detached = nullptr;
            {
                std::scoped_lock lock(activeBinkMutex);
                detached = DetachSecondaryBinkLocked(owner);
            }
            CloseDetachedBink(detached);
            VideoPlayer::GetSingleton().OnNativeVideoClosed();
            VideoPlayer::GetSingleton().StopSidecarAudio();
            pendingSidecarStart.store(false, std::memory_order_release);
            mainMenuStopped.store(true, std::memory_order_release);
            replaceMainMenuVideo.store(true, std::memory_order_release);
            ShowHelp(Config::MainMenuHelpMilliseconds());
        }

        void CaptureActiveBinkFrameLocked(void* handle)
        {
            if (!handle ||
                !activeBinkSelection.load(std::memory_order_acquire) ||
                !ValidBinkDimensions(handle)) {
                return;
            }
            const auto* header =
                static_cast<const PublicBinkHeader*>(handle);
            auto frame = std::make_shared<VideoFrame>();
            frame->width = header->width;
            frame->height = header->height;
            frame->rowPitch = frame->width * 4;
            frame->pixels.assign(
                static_cast<std::size_t>(frame->rowPitch) *
                    frame->height,
                0);
            constexpr std::uint32_t kCopyAll{ 0x80000000U };
            originalCopy(
                handle,
                frame->pixels.data(),
                static_cast<std::int32_t>(frame->rowPitch),
                frame->height,
                0,
                0,
                0,
                0,
                frame->width,
                frame->height,
                kCopyAll | kSurface32);
            frame->serial = activeBinkFrameSerial.fetch_add(
                1,
                std::memory_order_relaxed);
            activeBinkFrame.store(
                std::move(frame),
                std::memory_order_release);
        }

        void* RoutedBinkLocked(void* handle)
        {
            return handle ==
                    mainMenuBink.load(std::memory_order_acquire) &&
                   activeBink ?
                activeBink :
                handle;
        }

        std::int32_t __stdcall HookedPause(
            void* handle,
            const std::int32_t paused)
        {
            std::scoped_lock lock(activeBinkMutex);
            return originalPause(RoutedBinkLocked(handle), paused);
        }

        std::int32_t __stdcall HookedSetVolume(
            void* handle,
            const std::uint32_t track,
            const std::int32_t volume)
        {
            std::scoped_lock lock(activeBinkMutex);
            return originalSetVolume(
                RoutedBinkLocked(handle),
                track,
                volume);
        }

        std::int32_t __stdcall HookedDoFrame(void* handle)
        {
            std::scoped_lock lock(activeBinkMutex);
            void* routed = RoutedBinkLocked(handle);
            const std::int32_t result = originalDoFrame(routed);
            if (handle ==
                mainMenuBink.load(std::memory_order_acquire)) {
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

        void* __stdcall HookedOpen(
            const char* name,
            const std::uint32_t flags)
        {
            const bool isMainMenuVideo = IsMainMenuVideo(name);
            if (isMainMenuVideo && !EngineSettings::Apply()) {
                spdlog::warn(
                    "Could not apply live Fallout settings before "
                    "opening the main-menu Bink");
            }
            if (isMainMenuVideo) {
                EngineSettings::BeginMainMenu();
            }

            // Fallout keeps this handle for the lifetime of MainMenu.swf. Keep
            // its packaged loop as a stable carrier and route its playback
            // calls to whichever BK2 is currently selected.
            void* handle = originalOpen(name, flags);
            if (!handle && isMainMenuVideo) {
                EngineSettings::EndMainMenu();
            }
            if (!handle || !isMainMenuVideo) {
                return handle;
            }

            const auto* header = static_cast<const PublicBinkHeader*>(handle);
            const std::uint32_t width = header->width;
            const std::uint32_t height = header->height;
            if (width == 0 || height == 0 ||
                width > 16384 || height > 16384) {
                spdlog::warn(
                    "Opened the main-menu Bink, but its public dimensions "
                    "look invalid: {}x{}",
                    width,
                    height);
                return handle;
            }

            mainMenuOpenFlags = flags;
            mainMenuWidth.store(width, std::memory_order_release);
            mainMenuHeight.store(height, std::memory_order_release);
            mainMenuBink.store(handle, std::memory_order_release);
            void* staleSecondary = nullptr;
            {
                std::scoped_lock lock(activeBinkMutex);
                staleSecondary = DetachSecondaryBinkLocked(handle);
            }
            CloseDetachedBink(staleSecondary);
            mainMenuStopped.store(true, std::memory_order_release);
            replaceMainMenuVideo.store(true, std::memory_order_release);
            loggedCopyCalls.store(0, std::memory_order_relaxed);
            loggedUnsupportedSurface.store(false, std::memory_order_relaxed);
            InputRouter::Install();
            WorldTextureBridge::RequestInstall();
            spdlog::info(
                "Opened stable main-menu Bink carrier {} "
                "({}x{}, flags {:08X})",
                handle,
                width,
                height,
                flags);

            const auto selected = SelectMainMenuVideo(true);
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
                    expected,
                    nullptr,
                    std::memory_order_acq_rel)) {
                void* detached = nullptr;
                {
                    std::scoped_lock lock(activeBinkMutex);
                    detached = DetachSecondaryBinkLocked(handle);
                    activeBink = nullptr;
                }
                CloseDetachedBink(detached);
                mainMenuOpenFlags = 0;
                mainMenuWidth.store(0, std::memory_order_release);
                mainMenuHeight.store(0, std::memory_order_release);
                replaceMainMenuVideo.store(false, std::memory_order_release);
                mainMenuStopped.store(false, std::memory_order_release);
                pendingSidecarStart.store(false, std::memory_order_release);
                {
                    std::scoped_lock lock(selectionMutex);
                    pendingSidecar.reset();
                    currentSelection.clear();
                }
                VideoPlayer::GetSingleton().OnNativeVideoClosed();
                VideoPlayer::GetSingleton().StopSidecarAudio();
                EngineSettings::EndMainMenu();
                spdlog::info("Released native main-menu Bink handle");
            }
            originalClose(handle);
        }

        void CopyVideoPixels(
            const VideoFrame& frame,
            void* destination,
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
            if (!destination || destinationPitch == 0 ||
                destinationPitch == std::numeric_limits<std::int32_t>::min() ||
                destinationHeight == 0 ||
                frame.width == 0 || frame.height == 0 ||
                frame.rowPitch < frame.width * 4) {
                return;
            }

            const std::uint32_t surface = flags & kSurfaceMask;
            const bool reversed =
                surface == kSurface24Reversed ||
                surface == kSurface32Reversed ||
                surface == kSurface32ReversedAlpha;
            const std::uint32_t bytesPerPixel =
                surface == kSurface24 ||
                surface == kSurface24Reversed ? 3 :
                surface == kSurface32 ||
                surface == kSurface32Reversed ||
                surface == kSurface32Alpha ||
                surface == kSurface32ReversedAlpha ? 4 :
                0;
            if (bytesPerPixel == 0) {
                if (!loggedUnsupportedSurface.exchange(
                        true,
                        std::memory_order_relaxed)) {
                    spdlog::warn(
                        "Cannot replace Bink surface type {} (flags {:08X})",
                        surface,
                        flags);
                }
                return;
            }

            const std::uint64_t absolutePitch =
                destinationPitch < 0 ?
                    static_cast<std::uint64_t>(-destinationPitch) :
                    static_cast<std::uint64_t>(destinationPitch);
            const std::uint32_t rowCapacity =
                static_cast<std::uint32_t>(
                    absolutePitch / bytesPerPixel);
            if (destinationX >= rowCapacity ||
                destinationY >= destinationHeight) {
                return;
            }

            const std::uint32_t binkWidth =
                mainMenuWidth.load(std::memory_order_acquire);
            const std::uint32_t binkHeight =
                mainMenuHeight.load(std::memory_order_acquire);
            if (binkWidth == 0 || binkHeight == 0 ||
                sourceX >= binkWidth || sourceY >= binkHeight) {
                return;
            }

            const std::uint32_t copyWidth = std::min({
                sourceWidth,
                rowCapacity - destinationX,
                binkWidth - sourceX
            });
            const std::uint32_t copyHeight = std::min({
                sourceHeight,
                destinationHeight - destinationY,
                binkHeight - sourceY
            });
            if (copyWidth == 0 || copyHeight == 0) {
                return;
            }

            scaleMap.Update(frame, binkWidth, binkHeight);
            const bool directCopy =
                bytesPerPixel == 4 &&
                !reversed &&
                scaleMap.IsIdentity();

            auto* destinationBytes =
                static_cast<std::uint8_t*>(destination);
            for (std::uint32_t row = 0; row < copyHeight; ++row) {
                const std::uint32_t logicalY = sourceY + row;
                const std::uint32_t frameY = scaleMap.y[logicalY];
                const auto* sourceRow =
                    frame.pixels.data() +
                    static_cast<std::size_t>(frameY) * frame.rowPitch;
                auto* destinationRow =
                    destinationBytes +
                    static_cast<std::ptrdiff_t>(destinationY + row) *
                    destinationPitch +
                    static_cast<std::size_t>(destinationX) * bytesPerPixel;

                if (directCopy) {
                    std::memcpy(
                        destinationRow,
                        sourceRow +
                            static_cast<std::size_t>(sourceX) * 4,
                        static_cast<std::size_t>(copyWidth) * 4);
                    continue;
                }

                for (std::uint32_t column = 0;
                     column < copyWidth;
                     ++column) {
                    const std::uint32_t logicalX = sourceX + column;
                    const std::uint32_t frameX = scaleMap.x[logicalX];
                    const auto* pixel = sourceRow +
                        static_cast<std::size_t>(frameX) * 4;
                    auto* output = destinationRow +
                        static_cast<std::size_t>(column) * bytesPerPixel;
                    if (reversed) {
                        output[0] = pixel[2];
                        output[1] = pixel[1];
                        output[2] = pixel[0];
                    } else {
                        output[0] = pixel[0];
                        output[1] = pixel[1];
                        output[2] = pixel[2];
                    }
                    if (bytesPerPixel == 4) {
                        output[3] = 255;
                    }
                }
            }
        }

        void BlendHelpOverlay(
            void* destination,
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
            if (!destination ||
                destinationPitch == 0 ||
                destinationPitch == std::numeric_limits<std::int32_t>::min() ||
                destinationHeight == 0 ||
                GetTickCount64() >=
                    helpVisibleUntil.load(std::memory_order_acquire)) {
                return;
            }

            const std::uint32_t surface = flags & kSurfaceMask;
            const bool reversed =
                surface == kSurface24Reversed ||
                surface == kSurface32Reversed ||
                surface == kSurface32ReversedAlpha;
            const std::uint32_t bytesPerPixel =
                surface == kSurface24 ||
                surface == kSurface24Reversed ? 3 :
                surface == kSurface32 ||
                surface == kSurface32Reversed ||
                surface == kSurface32Alpha ||
                surface == kSurface32ReversedAlpha ? 4 :
                0;
            if (bytesPerPixel == 0) {
                return;
            }

            const std::uint64_t absolutePitch =
                destinationPitch < 0 ?
                    static_cast<std::uint64_t>(-destinationPitch) :
                    static_cast<std::uint64_t>(destinationPitch);
            const std::uint32_t rowCapacity =
                static_cast<std::uint32_t>(
                    absolutePitch / bytesPerPixel);
            const std::uint32_t outputWidth =
                mainMenuWidth.load(std::memory_order_acquire);
            const std::uint32_t outputHeight =
                mainMenuHeight.load(std::memory_order_acquire);
            if (rowCapacity == 0 || outputWidth == 0 || outputHeight == 0) {
                return;
            }

            static std::mutex overlayMutex;
            static std::uint64_t cachedRevision = 0;
            static std::uint32_t cachedOutputWidth = 0;
            static VideoFrame overlay;
            std::scoped_lock lock(overlayMutex);
            const auto revision =
                helpRevision.load(std::memory_order_acquire);
            if (cachedRevision != revision ||
                cachedOutputWidth != outputWidth) {
                overlay = BuildHelpOverlay(outputWidth);
                cachedRevision = revision;
                cachedOutputWidth = outputWidth;
            }
            if (overlay.pixels.empty()) {
                return;
            }

            const std::uint32_t left =
                std::max(16U, outputWidth / 60U);
            const std::uint32_t top =
                std::max(16U, outputHeight / 34U);
            const std::uint32_t copyRight = std::min(
                sourceX + sourceWidth,
                left + overlay.width);
            const std::uint32_t copyBottom = std::min(
                sourceY + sourceHeight,
                top + overlay.height);
            const std::uint32_t copyLeft = std::max(sourceX, left);
            const std::uint32_t copyTop = std::max(sourceY, top);
            if (copyLeft >= copyRight || copyTop >= copyBottom) {
                return;
            }

            auto* destinationBytes =
                static_cast<std::uint8_t*>(destination);
            if (bytesPerPixel == 4 && !reversed) {
                for (std::uint32_t logicalY = copyTop;
                     logicalY < copyBottom;
                     ++logicalY) {
                    const std::uint32_t outputY =
                        destinationY + logicalY - sourceY;
                    if (outputY >= destinationHeight) {
                        break;
                    }
                    const std::uint32_t outputX =
                        destinationX + copyLeft - sourceX;
                    if (outputX >= rowCapacity) {
                        continue;
                    }
                    const std::uint32_t pixelsToCopy = std::min(
                        copyRight - copyLeft,
                        rowCapacity - outputX);
                    auto* destinationRow =
                        destinationBytes +
                        static_cast<std::ptrdiff_t>(outputY) *
                            destinationPitch +
                        static_cast<std::size_t>(outputX) * 4;
                    const auto* overlayRow =
                        overlay.pixels.data() +
                        static_cast<std::size_t>(logicalY - top) *
                            overlay.rowPitch +
                        static_cast<std::size_t>(copyLeft - left) * 4;
                    std::memcpy(
                        destinationRow,
                        overlayRow,
                        static_cast<std::size_t>(pixelsToCopy) * 4);
                }
                return;
            }

            for (std::uint32_t logicalY = copyTop;
                 logicalY < copyBottom;
                 ++logicalY) {
                const std::uint32_t outputY =
                    destinationY + logicalY - sourceY;
                if (outputY >= destinationHeight) {
                    break;
                }
                auto* destinationRow =
                    destinationBytes +
                    static_cast<std::ptrdiff_t>(outputY) *
                        destinationPitch;
                const auto* overlayRow =
                    overlay.pixels.data() +
                    static_cast<std::size_t>(logicalY - top) *
                        overlay.rowPitch;
                for (std::uint32_t logicalX = copyLeft;
                     logicalX < copyRight;
                     ++logicalX) {
                    const std::uint32_t outputX =
                        destinationX + logicalX - sourceX;
                    if (outputX >= rowCapacity) {
                        break;
                    }
                    const auto* sourcePixel =
                        overlayRow +
                        static_cast<std::size_t>(logicalX - left) * 4;
                    const std::uint32_t alpha = sourcePixel[3];
                    if (alpha == 0) {
                        continue;
                    }
                    auto* outputPixel =
                        destinationRow +
                        static_cast<std::size_t>(outputX) * bytesPerPixel;
                    const std::size_t blueIndex = reversed ? 2 : 0;
                    const std::size_t redIndex = reversed ? 0 : 2;
                    outputPixel[blueIndex] = static_cast<std::uint8_t>(
                        (sourcePixel[0] * alpha +
                         outputPixel[blueIndex] * (255 - alpha)) /
                        255);
                    outputPixel[1] = static_cast<std::uint8_t>(
                        (sourcePixel[1] * alpha +
                         outputPixel[1] * (255 - alpha)) /
                        255);
                    outputPixel[redIndex] = static_cast<std::uint8_t>(
                        (sourcePixel[2] * alpha +
                         outputPixel[redIndex] * (255 - alpha)) /
                        255);
                    if (bytesPerPixel == 4) {
                        outputPixel[3] = 255;
                    }
                }
            }
        }

        std::int32_t __stdcall HookedCopy(
            void* handle,
            void* destination,
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
            const std::int32_t result = selectedBink ?
                0 :
                originalCopy(
                    handle,
                    destination,
                    destinationPitch,
                    destinationHeight,
                    destinationX,
                    destinationY,
                    sourceX,
                    sourceY,
                    sourceWidth,
                    sourceHeight,
                    flags);

            if (!isMainMenu) {
                return result;
            }
            if (pendingSidecarStart.exchange(
                    false,
                    std::memory_order_acq_rel)) {
                std::optional<std::filesystem::path> sidecar;
                {
                    std::scoped_lock lock(selectionMutex);
                    sidecar = pendingSidecar;
                }
                if (sidecar) {
                    VideoPlayer::GetSingleton().StartSidecarAudio(*sidecar);
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
                        call + 1,
                        destinationPitch,
                        destinationHeight,
                        destinationX,
                        destinationY,
                        sourceX,
                        sourceY,
                        sourceWidth,
                        sourceHeight,
                        flags & kSurfaceMask,
                        flags);
                }

                const auto frame = selectedBink ?
                    activeBinkFrame.load(std::memory_order_acquire) :
                    VideoPlayer::GetSingleton().GetLatestFrame();
                static const VideoFrame blackFrame{
                    .pixels = { 0, 0, 0, 255 },
                    .width = 1,
                    .height = 1,
                    .rowPitch = 4,
                    .serial = 0
                };
                CopyVideoPixels(
                    frame && !mainMenuStopped.load(
                        std::memory_order_acquire) ?
                        *frame :
                        blackFrame,
                    destination,
                    destinationPitch,
                    destinationHeight,
                    destinationX,
                    destinationY,
                    sourceX,
                    sourceY,
                    sourceWidth,
                    sourceHeight,
                    flags);
            }

            BlendHelpOverlay(
                destination,
                destinationPitch,
                destinationHeight,
                destinationX,
                destinationY,
                sourceX,
                sourceY,
                sourceWidth,
                sourceHeight,
                flags);
            return result;
        }

        bool CreateHook(
            HMODULE module,
            const char* exportName,
            void* hook,
            void** original,
            void*& target)
        {
            target = reinterpret_cast<void*>(
                GetProcAddress(module, exportName));
            if (!target) {
                spdlog::error(
                    "Could not locate {} in bink2w64.dll",
                    exportName);
                return false;
            }
            const MH_STATUS result = MH_CreateHook(
                target,
                hook,
                original);
            if (result != MH_OK) {
                spdlog::error(
                    "MH_CreateHook({}) failed: {}",
                    exportName,
                    MH_StatusToString(result));
                return false;
            }
            return true;
        }
    }

    bool HandleWindowMessage(
        const UINT message,
        const WPARAM wParam,
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
                    spdlog::info(
                        "Main-menu next hotkey selected {}",
                        Utf8Path(*selected));
                } else {
                    spdlog::warn(
                        "The next-video hotkey could not activate {}",
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

        if (Config::MainMenuVolumeUpKey() != 0 &&
            key == Config::MainMenuVolumeUpKey()) {
            player.AdjustVolume(Config::MainMenuVolumeStep());
            if (Config::MainMenuHelpMilliseconds() != 0) {
                ShowHelp(2000);
            }
            return true;
        }

        if (Config::MainMenuVolumeDownKey() != 0 &&
            key == Config::MainMenuVolumeDownKey()) {
            player.AdjustVolume(-Config::MainMenuVolumeStep());
            if (Config::MainMenuHelpMilliseconds() != 0) {
                ShowHelp(2000);
            }
            return true;
        }

        return false;
    }

    bool Install()
    {
        const MH_STATUS initializeResult = MH_Initialize();
        if (initializeResult != MH_OK &&
            initializeResult != MH_ERROR_ALREADY_INITIALIZED) {
            spdlog::error(
                "MH_Initialize failed: {}",
                MH_StatusToString(initializeResult));
            return false;
        }

        HMODULE bink = GetModuleHandleW(L"bink2w64.dll");
        if (!bink) {
            bink = LoadLibraryW(L"bink2w64.dll");
        }
        if (!bink) {
            spdlog::error("Could not load bink2w64.dll");
            return false;
        }

        if (!CreateHook(
                bink,
                "BinkOpen",
                reinterpret_cast<void*>(&HookedOpen),
                reinterpret_cast<void**>(&originalOpen),
                hookTargets[0]) ||
            !CreateHook(
                bink,
                "BinkClose",
                reinterpret_cast<void*>(&HookedClose),
                reinterpret_cast<void**>(&originalClose),
                hookTargets[1]) ||
            !CreateHook(
                bink,
                "BinkCopyToBufferRect",
                reinterpret_cast<void*>(&HookedCopy),
                reinterpret_cast<void**>(&originalCopy),
                hookTargets[2]) ||
            !CreateHook(
                bink,
                "BinkPause",
                reinterpret_cast<void*>(&HookedPause),
                reinterpret_cast<void**>(&originalPause),
                hookTargets[3]) ||
            !CreateHook(
                bink,
                "BinkSetVolume",
                reinterpret_cast<void*>(&HookedSetVolume),
                reinterpret_cast<void**>(&originalSetVolume),
                hookTargets[4]) ||
            !CreateHook(
                bink,
                "BinkDoFrame",
                reinterpret_cast<void*>(&HookedDoFrame),
                reinterpret_cast<void**>(&originalDoFrame),
                hookTargets[5]) ||
            !CreateHook(
                bink,
                "BinkNextFrame",
                reinterpret_cast<void*>(&HookedNextFrame),
                reinterpret_cast<void**>(&originalNextFrame),
                hookTargets[6]) ||
            !CreateHook(
                bink,
                "BinkWait",
                reinterpret_cast<void*>(&HookedWait),
                reinterpret_cast<void**>(&originalWait),
                hookTargets[7]) ||
            !CreateHook(
                bink,
                "BinkShouldSkip",
                reinterpret_cast<void*>(&HookedShouldSkip),
                reinterpret_cast<void**>(&originalShouldSkip),
                hookTargets[8])) {
            return false;
        }

        for (void* target : hookTargets) {
            const MH_STATUS enableResult = MH_EnableHook(target);
            if (enableResult != MH_OK &&
                enableResult != MH_ERROR_ENABLED) {
                spdlog::error(
                    "MH_EnableHook({}) failed: {}",
                    target,
                    MH_StatusToString(enableResult));
                return false;
            }
        }

        spdlog::info(
            "Installed native Bink carrier-overlay hooks from {}",
            reinterpret_cast<void*>(bink));
        return true;
    }
}
