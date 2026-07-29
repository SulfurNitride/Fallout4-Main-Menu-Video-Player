#include "PCH.h"

#include "Config.h"
#include "InputRouter.h"
#include "PipBoyPlayer.h"
#include "WorldPlayback.h"
#include "WorldTextureBridge.h"

#include <d3d11.h>

namespace PipBoyPlayer
{
    namespace
    {
        constexpr std::uint32_t kScaleformTypeMask{ 0x8F };
        constexpr std::size_t kMovieRootOffset{ 0x18 };
        constexpr std::size_t kGetMovieDefVtableIndex{ 0x01 };
        constexpr std::size_t kGetFileUrlVtableIndex{ 0x0C };
        constexpr std::size_t kGetMouseStateVtableIndex{ 0x24 };
        constexpr std::size_t kSetVariableVtableIndex{ 0x31 };
        constexpr std::size_t kGetVariableVtableIndex{ 0x32 };
        constexpr std::string_view kCommandPath{
            "root.f4se.plugins.MainMenuVideoPlayer.command"
        };
        constexpr std::uint64_t kFadeMilliseconds{ 500 };
        constexpr std::uint32_t kScaleformDiagnosticLimit{ 64 };

        struct ScaleformValue
        {
            void* objectInterface{ nullptr };
            std::uint32_t type{ 0 };
            std::uint32_t padding{ 0 };
            std::uint64_t value{ 0 };
            void* unknown{ nullptr };
        };

        struct MenuCursorState
        {
            std::byte padding[0x24];
            std::int32_t cursorPosX;
            std::int32_t cursorPosY;
            std::int32_t minCursorX;
            std::int32_t minCursorY;
            std::int32_t maxCursorX;
            std::int32_t maxCursorY;
        };
        static_assert(offsetof(MenuCursorState, cursorPosX) == 0x24);

        struct UiState
        {
            float cursorX{ 438.0F };
            float cursorY{ 350.0F };
            std::uint32_t targetWidth{ 876 };
            std::uint32_t targetHeight{ 700 };
            std::uint64_t lastMotionTick{ 0 };
            std::uint64_t revision{ 1 };
            std::uint64_t lastClickTick{ 0 };
            PlaybackChannel channel{ PlaybackChannel::kProjector };
        };

        struct ComposedFrame
        {
            std::vector<std::uint8_t> pixels;
            std::uint32_t width{ 0 };
            std::uint32_t height{ 0 };
            DXGI_FORMAT format{ DXGI_FORMAT_UNKNOWN };
            std::uint64_t sourceSerial{ 0 };
            std::uint64_t uiRevision{ 0 };
            std::uint32_t overlayAlpha{ 0 };
            std::uint32_t progressBucket{ 0 };
            PlaybackState playbackState{ PlaybackState::kStopped };
            PlaybackChannel channel{ PlaybackChannel::kProjector };
        };

        std::atomic<bool> active{ false };
        std::atomic<void*> pipBoyMovieView{ nullptr };
        std::atomic<void*> cursorMovieView{ nullptr };
        std::atomic<void*> pipBoyMovieRoot{ nullptr };
        std::atomic<std::uint32_t> scaleformCallbackCount{ 0 };
        std::atomic<bool> loggedCursorSource{ false };
        std::mutex uiMutex;
        UiState ui;
        ComposedFrame composed;
        std::uintptr_t menuCursorSingletonOffset{ 0 };

        std::uintptr_t MenuCursorOffset(const std::uint32_t runtime)
        {
            switch (runtime) {
            case F4SEMinimal::kRuntimeOg:
                return 0x5A67210;
            case F4SEMinimal::kRuntimeAe137:
                return 0x325E1D8;
            case F4SEMinimal::kRuntimeAe159:
                return 0x325F158;
            case F4SEMinimal::kRuntimeAe169:
                return 0x3264358;
            case F4SEMinimal::kRuntimeAe191:
                return 0x326F458;
            case F4SEMinimal::kRuntimeAe221:
                return 0x326F4D8;
            default:
                return 0;
            }
        }
        WorldPlaybackSession* PlayerFor(const PlaybackChannel channel)
        {
            auto& playback = WorldPlayback::GetSingleton();
            return channel == PlaybackChannel::kTelevision ?
                playback.Television() :
                playback.Projector();
        }

        PlaybackChannel SelectedChannel()
        {
            std::scoped_lock lock(uiMutex);
            return ui.channel;
        }

        WorldPlaybackSession* SelectedPlayer()
        {
            return PlayerFor(SelectedChannel());
        }

        void StartPlayerIfNeeded(WorldPlaybackSession* player)
        {
            if (!player) {
                return;
            }
            const auto snapshot = player->Snapshot();
            if (!player->LatestFrame() &&
                snapshot.state != PlaybackState::kPaused) {
                player->Next();
            } else {
                player->Play();
            }
        }

        void SetFalloutCursorVisible(const bool visible)
        {
            void* movieView =
                cursorMovieView.load(std::memory_order_acquire);
            if (!movieView) {
                return;
            }
            void** vtable = *reinterpret_cast<void***>(movieView);
            constexpr std::size_t kSetVisibleVtableIndex{ 0x09 };
            if (!vtable || !vtable[kSetVisibleVtableIndex]) {
                return;
            }
            using SetVisible = void (*)(void*, bool);
            reinterpret_cast<SetVisible>(
                vtable[kSetVisibleVtableIndex])(
                movieView,
                visible);
        }

        std::uint32_t OverlayAlpha(const std::uint64_t now)
        {
            std::scoped_lock lock(uiMutex);
            const std::uint64_t elapsed =
                now >= ui.lastMotionTick ? now - ui.lastMotionTick : 0;
            const std::uint64_t timeout =
                Config::PipBoyOverlayTimeoutMilliseconds();
            if (elapsed >= timeout) {
                return 0;
            }
            if (elapsed + kFadeMilliseconds <= timeout) {
                return 220;
            }
            return static_cast<std::uint32_t>(
                220 * (timeout - elapsed) / kFadeMilliseconds);
        }

        void UpdateConsumers()
        {
            const bool pipBoyActive =
                active.load(std::memory_order_acquire);
            const auto selected = SelectedChannel();
            auto& playback = WorldPlayback::GetSingleton();
            if (auto* television = playback.Television()) {
                television->SetConsumers(
                    (WorldTextureBridge::TelevisionTextureCaptured() ?
                         1U :
                         0U) +
                    (pipBoyActive &&
                             selected == PlaybackChannel::kTelevision ?
                         1U :
                         0U));
            }
            if (auto* projector = playback.Projector()) {
                projector->SetConsumers(
                    (WorldTextureBridge::ProjectorTextureCaptured() ?
                         1U :
                         0U) +
                    (pipBoyActive &&
                             selected == PlaybackChannel::kProjector ?
                         1U :
                         0U));
            }
        }

        void ExecuteClick()
        {
            if (!active.load(std::memory_order_acquire)) {
                return;
            }

            float cursorX = 0.0F;
            float cursorY = 0.0F;
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            PlaybackChannel channel{ PlaybackChannel::kProjector };
            {
                std::scoped_lock lock(uiMutex);
                const auto now = GetTickCount64();
                if (now - ui.lastClickTick < 120) {
                    return;
                }
                ui.lastClickTick = now;
                ui.lastMotionTick = now;
                ++ui.revision;
                cursorX = ui.cursorX;
                cursorY = ui.cursorY;
                width = ui.targetWidth;
                height = ui.targetHeight;
                channel = ui.channel;
            }

            auto* player = PlayerFor(channel);
            if (!player) {
                return;
            }

            const float progressLeft = 48.0F;
            const float progressRight = static_cast<float>(width) - 48.0F;
            const float progressY = static_cast<float>(height) - 108.0F;
            if (cursorX >= progressLeft && cursorX <= progressRight &&
                std::abs(cursorY - progressY) <= 16.0F) {
                const auto snapshot = player->Snapshot();
                if (snapshot.durationSeconds > 0.0) {
                    const double fraction = std::clamp(
                        static_cast<double>(
                            (cursorX - progressLeft) /
                            (progressRight - progressLeft)),
                        0.0,
                        1.0);
                    player->SeekBy(
                        snapshot.durationSeconds * fraction -
                        snapshot.positionSeconds);
                }
                return;
            }

            constexpr float buttonHalf{ 28.0F };
            constexpr float spacing{ 72.0F };
            const float centerX = static_cast<float>(width) * 0.5F;
            const float buttonY = static_cast<float>(height) - 54.0F;
            if (std::abs(cursorY - buttonY) > buttonHalf) {
                return;
            }

            const auto hit = [&](const float x) {
                return std::abs(cursorX - x) <= buttonHalf;
            };
            if (hit(centerX - spacing * 1.5F)) {
                player->Previous();
            } else if (hit(centerX - spacing * 0.5F)) {
                const auto state = player->Snapshot().state;
                if (state == PlaybackState::kPlaying) {
                    player->Pause();
                } else {
                    player->Play();
                }
            } else if (hit(centerX + spacing * 0.5F)) {
                player->Next();
            } else if (hit(centerX + spacing * 1.5F)) {
                player->Stop();
            }
        }

        void ActivatePlayer()
        {
            if (!Config::EnablePipBoyPlayer()) {
                return;
            }
            const bool wasActive =
                active.exchange(true, std::memory_order_acq_rel);
            {
                std::scoped_lock lock(uiMutex);
                ui.targetWidth = Config::PipBoyWidth();
                ui.targetHeight = Config::PipBoyHeight();
                ui.cursorX = static_cast<float>(ui.targetWidth) * 0.5F;
                ui.cursorY = static_cast<float>(ui.targetHeight) * 0.5F;
                ui.lastMotionTick = GetTickCount64();
                ++ui.revision;
            }
            loggedCursorSource.store(false, std::memory_order_release);
            if (InputRouter::Install()) {
                spdlog::debug(
                    "Pip-Boy player is using the shared input router");
            }
            SetFalloutCursorVisible(false);
            UpdateConsumers();

            StartPlayerIfNeeded(SelectedPlayer());
            if (!wasActive) {
                spdlog::info("MMVP Pip-Boy video program activated");
            }
        }

        void ActivateRandom(const PlaybackChannel channel)
        {
            auto* player = PlayerFor(channel);
            if (!player) {
                spdlog::warn(
                    "Cannot play a random {} video because its playback "
                    "session is unavailable",
                    PlaybackChannelName(channel));
                return;
            }
            {
                std::scoped_lock lock(uiMutex);
                ui.channel = channel;
                ++ui.revision;
            }
            player->RefreshLibrary();
            player->Next();
            ActivatePlayer();
            spdlog::info(
                "Accepted Pip-Boy terminal request for a random {} video",
                PlaybackChannelName(channel));
        }

        bool EndsWithInsensitive(
            const std::string_view value,
            const std::string_view suffix)
        {
            if (value.size() < suffix.size()) {
                return false;
            }
            const auto tail = value.substr(value.size() - suffix.size());
            return std::ranges::equal(
                tail,
                suffix,
                [](const char left, const char right) {
                    return std::tolower(
                               static_cast<unsigned char>(left)) ==
                           std::tolower(
                               static_cast<unsigned char>(right));
                });
        }

        const char* MovieUrl(void* rawView)
        {
            void** viewVtable = *reinterpret_cast<void***>(rawView);
            if (!viewVtable ||
                !viewVtable[kGetMovieDefVtableIndex]) {
                return nullptr;
            }
            using GetMovieDef = void* (*)(void*);
            void* movieDef = reinterpret_cast<GetMovieDef>(
                viewVtable[kGetMovieDefVtableIndex])(rawView);
            if (!movieDef) {
                return nullptr;
            }
            void** definitionVtable = *reinterpret_cast<void***>(movieDef);
            if (!definitionVtable ||
                !definitionVtable[kGetFileUrlVtableIndex]) {
                return nullptr;
            }
            using GetFileUrl = const char* (*)(void*);
            return reinterpret_cast<GetFileUrl>(
                definitionVtable[kGetFileUrlVtableIndex])(movieDef);
        }

        bool ScaleformCallback(void* rawView, void*)
        {
            const auto callbackNumber =
                scaleformCallbackCount.fetch_add(
                    1,
                    std::memory_order_relaxed) + 1;
            if (!rawView || !Config::EnablePipBoyPlayer()) {
                if (callbackNumber <= kScaleformDiagnosticLimit) {
                    spdlog::info(
                        "Scaleform callback {} ignored: view={}, "
                        "Pip-Boy player enabled={}",
                        callbackNumber,
                        rawView != nullptr,
                        Config::EnablePipBoyPlayer());
                }
                return true;
            }
            const char* movieUrl = MovieUrl(rawView);
            if (callbackNumber <= kScaleformDiagnosticLimit) {
                spdlog::info(
                    "Scaleform callback {} movie='{}'",
                    callbackNumber,
                    movieUrl ? movieUrl : "<unknown>");
            }
            if (movieUrl &&
                EndsWithInsensitive(
                    movieUrl,
                    "PipboyMenu.swf")) {
                void* movieRoot = *reinterpret_cast<void**>(
                    static_cast<std::uint8_t*>(rawView) +
                    kMovieRootOffset);
                pipBoyMovieRoot.store(
                    movieRoot,
                    std::memory_order_release);
                pipBoyMovieView.store(
                    rawView,
                    std::memory_order_release);
                spdlog::info(
                    "Armed MMVP terminal command detection on '{}'",
                    movieUrl);
                return true;
            }
            if (movieUrl &&
                EndsWithInsensitive(
                    movieUrl,
                    "CursorMenu.swf")) {
                cursorMovieView.store(
                    rawView,
                    std::memory_order_release);
                if (active.load(std::memory_order_acquire)) {
                    SetFalloutCursorVisible(false);
                }
                spdlog::info(
                    "Captured Fallout cursor Scaleform movie '{}'",
                    movieUrl);
                return true;
            }
            return true;
        }

        void BlendPixel(
            std::uint8_t* pixel,
            const std::uint8_t blue,
            const std::uint8_t green,
            const std::uint8_t red,
            const std::uint8_t alpha)
        {
            const std::uint32_t inverse = 255U - alpha;
            pixel[0] = static_cast<std::uint8_t>(
                (pixel[0] * inverse + blue * alpha) / 255U);
            pixel[1] = static_cast<std::uint8_t>(
                (pixel[1] * inverse + green * alpha) / 255U);
            pixel[2] = static_cast<std::uint8_t>(
                (pixel[2] * inverse + red * alpha) / 255U);
            pixel[3] = 255;
        }

        void Rectangle(
            std::vector<std::uint8_t>& pixels,
            const std::uint32_t width,
            const std::uint32_t height,
            int left,
            int top,
            int right,
            int bottom,
            const std::uint8_t blue,
            const std::uint8_t green,
            const std::uint8_t red,
            const std::uint8_t alpha)
        {
            left = std::clamp(left, 0, static_cast<int>(width));
            right = std::clamp(right, 0, static_cast<int>(width));
            top = std::clamp(top, 0, static_cast<int>(height));
            bottom = std::clamp(bottom, 0, static_cast<int>(height));
            for (int y = top; y < bottom; ++y) {
                for (int x = left; x < right; ++x) {
                    BlendPixel(
                        pixels.data() +
                            (static_cast<std::size_t>(y) * width + x) * 4,
                        blue,
                        green,
                        red,
                        alpha);
                }
            }
        }

        void Line(
            std::vector<std::uint8_t>& pixels,
            const std::uint32_t width,
            const std::uint32_t height,
            int x0,
            int y0,
            const int x1,
            const int y1,
            const int thickness,
            const std::uint8_t alpha)
        {
            const int deltaX = std::abs(x1 - x0);
            const int stepX = x0 < x1 ? 1 : -1;
            const int deltaY = -std::abs(y1 - y0);
            const int stepY = y0 < y1 ? 1 : -1;
            int error = deltaX + deltaY;
            while (true) {
                Rectangle(
                    pixels,
                    width,
                    height,
                    x0 - thickness / 2,
                    y0 - thickness / 2,
                    x0 + (thickness + 1) / 2,
                    y0 + (thickness + 1) / 2,
                    210,
                    255,
                    210,
                    alpha);
                if (x0 == x1 && y0 == y1) {
                    break;
                }
                const int twiceError = 2 * error;
                if (twiceError >= deltaY) {
                    error += deltaY;
                    x0 += stepX;
                }
                if (twiceError <= deltaX) {
                    error += deltaX;
                    y0 += stepY;
                }
            }
        }

        void DrawTriangle(
            std::vector<std::uint8_t>& pixels,
            const std::uint32_t width,
            const std::uint32_t height,
            const int centerX,
            const int centerY,
            const int direction,
            const std::uint8_t alpha)
        {
            for (int offset = -14; offset <= 14; ++offset) {
                const int extent = 14 - std::abs(offset);
                const int x0 = direction < 0 ?
                    centerX + extent / 2 :
                    centerX - extent / 2;
                const int x1 = direction < 0 ?
                    centerX - extent :
                    centerX + extent;
                Line(
                    pixels,
                    width,
                    height,
                    x0,
                    centerY + offset,
                    x1,
                    centerY + offset,
                    2,
                    alpha);
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
            case ',': return { 0, 0, 0, 0, 0, 4, 8 };
            case '-': return { 0, 0, 0, 31, 0, 0, 0 };
            case '_': return { 0, 0, 0, 0, 0, 0, 31 };
            case '/': return { 1, 2, 2, 4, 8, 8, 16 };
            case '\\': return { 16, 8, 8, 4, 2, 2, 1 };
            case ':': return { 0, 12, 12, 0, 12, 12, 0 };
            case '<': return { 2, 4, 8, 16, 8, 4, 2 };
            case '>': return { 8, 4, 2, 1, 2, 4, 8 };
            case '(': return { 2, 4, 8, 8, 8, 4, 2 };
            case ')': return { 8, 4, 2, 2, 2, 4, 8 };
            case ' ': return {};
            default: return { 14, 17, 1, 2, 4, 0, 4 };
            }
        }

        void DrawText(
            std::vector<std::uint8_t>& pixels,
            const std::uint32_t width,
            const std::uint32_t height,
            const int left,
            const int top,
            const std::string_view text,
            const std::size_t maximumCharacters,
            const std::uint8_t alpha,
            const int scale = 2)
        {
            int x = left;
            const std::size_t count =
                std::min(maximumCharacters, text.size());
            for (std::size_t index = 0; index < count; ++index) {
                const auto glyph = Glyph(text[index]);
                for (int row = 0; row < 7; ++row) {
                    for (int column = 0; column < 5; ++column) {
                        if ((glyph[row] & (1U << (4 - column))) == 0) {
                            continue;
                        }
                        Rectangle(
                            pixels,
                            width,
                            height,
                            x + column * scale,
                            top + row * scale,
                            x + (column + 1) * scale,
                            top + (row + 1) * scale,
                            210,
                            255,
                            210,
                            alpha);
                    }
                }
                x += 6 * scale;
            }
        }

        void DrawScreenCursor(
            std::vector<std::uint8_t>& pixels,
            const std::uint32_t width,
            const std::uint32_t height,
            const int pointerX,
            const int pointerY)
        {
            constexpr std::array<std::uint16_t, 19> pointerMask{
                0x8000, 0xC000, 0xE000, 0xF000, 0xF800,
                0xFC00, 0xFE00, 0xFF00, 0xFF80, 0xFFC0,
                0xFFE0, 0xFFF0, 0xFF80, 0xE7C0, 0xC3E0,
                0x81F0, 0x00F8, 0x0078, 0x0030
            };
            const auto filled = [&](const int x, const int y) {
                return y >= 0 &&
                       y < static_cast<int>(pointerMask.size()) &&
                       x >= 0 &&
                       x < 16 &&
                       (pointerMask[static_cast<std::size_t>(y)] &
                        (0x8000U >> x)) != 0;
            };
            for (int y = -1;
                 y <= static_cast<int>(pointerMask.size());
                 ++y) {
                for (int x = -1; x <= 16; ++x) {
                    if (filled(x, y)) {
                        continue;
                    }
                    bool outline = false;
                    for (int offsetY = -1; offsetY <= 1; ++offsetY) {
                        for (int offsetX = -1; offsetX <= 1; ++offsetX) {
                            outline = outline ||
                                filled(x + offsetX, y + offsetY);
                        }
                    }
                    if (outline) {
                        Rectangle(
                            pixels,
                            width,
                            height,
                            pointerX + x,
                            pointerY + y,
                            pointerX + x + 1,
                            pointerY + y + 1,
                            0,
                            10,
                            0,
                            255);
                    }
                }
            }
            for (int y = 0;
                 y < static_cast<int>(pointerMask.size());
                 ++y) {
                for (int x = 0; x < 16; ++x) {
                    if (filled(x, y)) {
                        Rectangle(
                            pixels,
                            width,
                            height,
                            pointerX + x,
                            pointerY + y,
                            pointerX + x + 1,
                            pointerY + y + 1,
                            20,
                            255,
                            20,
                            255);
                    }
                }
            }
        }

        void DrawOverlay(
            std::vector<std::uint8_t>& pixels,
            const std::uint32_t width,
            const std::uint32_t height,
            const PlaybackSnapshot& snapshot,
            const PlaybackChannel channel,
            const std::uint32_t alpha,
            const float cursorX,
            const float cursorY)
        {
            if (alpha == 0 || width < 320 || height < 240) {
                return;
            }
            const auto opacity = static_cast<std::uint8_t>(alpha);
            Rectangle(
                pixels,
                width,
                height,
                0,
                0,
                static_cast<int>(width),
                52,
                0,
                0,
                0,
                static_cast<std::uint8_t>(alpha * 3 / 4));
            DrawText(
                pixels,
                width,
                height,
                18,
                18,
                channel == PlaybackChannel::kTelevision ?
                    "RANDOM TV" :
                    "RANDOM MOVIE",
                12,
                opacity,
                2);
            if (!snapshot.mediaId.empty()) {
                DrawText(
                    pixels,
                    width,
                    height,
                    190,
                    20,
                    snapshot.mediaId,
                    52,
                    static_cast<std::uint8_t>(opacity * 4 / 5),
                    1);
            }

            const int panelTop = static_cast<int>(height) - 145;
            Rectangle(
                pixels,
                width,
                height,
                0,
                panelTop,
                static_cast<int>(width),
                static_cast<int>(height),
                0,
                0,
                0,
                static_cast<std::uint8_t>(alpha * 3 / 4));

            const int progressLeft = 48;
            const int progressRight = static_cast<int>(width) - 48;
            const int progressY = static_cast<int>(height) - 108;
            Rectangle(
                pixels,
                width,
                height,
                progressLeft,
                progressY - 3,
                progressRight,
                progressY + 4,
                75,
                95,
                75,
                opacity);
            const double fraction = snapshot.durationSeconds > 0.0 ?
                std::clamp(
                    snapshot.positionSeconds / snapshot.durationSeconds,
                    0.0,
                    1.0) :
                0.0;
            const int progressX = progressLeft +
                static_cast<int>((progressRight - progressLeft) * fraction);
            Rectangle(
                pixels,
                width,
                height,
                progressLeft,
                progressY - 4,
                progressX,
                progressY + 5,
                150,
                255,
                170,
                opacity);
            Rectangle(
                pixels,
                width,
                height,
                progressX - 5,
                progressY - 8,
                progressX + 6,
                progressY + 9,
                210,
                255,
                210,
                opacity);

            constexpr int spacing = 72;
            const int centerX = static_cast<int>(width) / 2;
            const int buttonY = static_cast<int>(height) - 54;
            const int previousX = centerX - spacing * 3 / 2;
            const int playX = centerX - spacing / 2;
            const int nextX = centerX + spacing / 2;
            const int stopX = centerX + spacing * 3 / 2;

            Line(
                pixels, width, height,
                previousX - 14, buttonY - 15,
                previousX - 14, buttonY + 15, 4, opacity);
            DrawTriangle(
                pixels, width, height,
                previousX + 3, buttonY, -1, opacity);

            if (snapshot.state == PlaybackState::kPlaying) {
                Rectangle(
                    pixels, width, height,
                    playX - 11, buttonY - 15,
                    playX - 4, buttonY + 16,
                    210, 255, 210, opacity);
                Rectangle(
                    pixels, width, height,
                    playX + 4, buttonY - 15,
                    playX + 11, buttonY + 16,
                    210, 255, 210, opacity);
            } else {
                DrawTriangle(
                    pixels, width, height,
                    playX, buttonY, 1, opacity);
            }

            DrawTriangle(
                pixels, width, height,
                nextX - 3, buttonY, 1, opacity);
            Line(
                pixels, width, height,
                nextX + 14, buttonY - 15,
                nextX + 14, buttonY + 15, 4, opacity);
            Rectangle(
                pixels, width, height,
                stopX - 13, buttonY - 13,
                stopX + 14, buttonY + 14,
                210, 255, 210, opacity);
            DrawScreenCursor(
                pixels,
                width,
                height,
                static_cast<int>(cursorX),
                static_cast<int>(cursorY));
        }

        void Compose(
            const std::shared_ptr<const VideoFrame>& source,
            const PlaybackSnapshot& snapshot,
            const PlaybackChannel channel,
            const D3D11_TEXTURE2D_DESC& descriptor,
            const std::uint32_t overlayAlpha,
            const float cursorX,
            const float cursorY,
            const std::uint64_t revision)
        {
            composed.width = descriptor.Width;
            composed.height = descriptor.Height;
            composed.format = descriptor.Format;
            composed.sourceSerial = source ? source->serial : 0;
            composed.uiRevision = revision;
            composed.overlayAlpha = overlayAlpha;
            composed.playbackState = snapshot.state;
            composed.channel = channel;
            composed.progressBucket = snapshot.durationSeconds > 0.0 ?
                static_cast<std::uint32_t>(
                    std::clamp(
                        snapshot.positionSeconds /
                            snapshot.durationSeconds,
                        0.0,
                        1.0) * 1000.0) :
                0;
            composed.pixels.assign(
                static_cast<std::size_t>(descriptor.Width) *
                    descriptor.Height * 4,
                0);
            for (std::size_t index = 3;
                 index < composed.pixels.size();
                 index += 4) {
                composed.pixels[index] = 255;
            }

            if (source && source->width > 0 && source->height > 0 &&
                source->rowPitch >= source->width * 4 &&
                !source->pixels.empty()) {
                std::uint32_t outputWidth = descriptor.Width;
                std::uint32_t outputHeight = std::max(
                    1U,
                    static_cast<std::uint32_t>(
                        static_cast<std::uint64_t>(outputWidth) *
                        source->height / source->width));
                if (outputHeight > descriptor.Height) {
                    outputHeight = descriptor.Height;
                    outputWidth = std::max(
                        1U,
                        static_cast<std::uint32_t>(
                            static_cast<std::uint64_t>(outputHeight) *
                            source->width / source->height));
                }
                const std::uint32_t outputX =
                    (descriptor.Width - outputWidth) / 2;
                const std::uint32_t outputY =
                    (descriptor.Height - outputHeight) / 2;
                for (std::uint32_t y = 0; y < outputHeight; ++y) {
                    const std::uint32_t sourceY =
                        static_cast<std::uint32_t>(
                            static_cast<std::uint64_t>(y) *
                            source->height / outputHeight);
                    const std::uint8_t* sourceRow =
                        source->pixels.data() +
                        static_cast<std::size_t>(sourceY) *
                            source->rowPitch;
                    std::uint8_t* outputRow =
                        composed.pixels.data() +
                        (static_cast<std::size_t>(outputY + y) *
                         descriptor.Width + outputX) * 4;
                    for (std::uint32_t x = 0; x < outputWidth; ++x) {
                        const std::uint32_t sourceX =
                            static_cast<std::uint32_t>(
                                static_cast<std::uint64_t>(x) *
                                source->width / outputWidth);
                        std::memcpy(
                            outputRow + static_cast<std::size_t>(x) * 4,
                            sourceRow + static_cast<std::size_t>(sourceX) * 4,
                            4);
                    }
                }
            }

            DrawOverlay(
                composed.pixels,
                descriptor.Width,
                descriptor.Height,
                snapshot,
                channel,
                overlayAlpha,
                cursorX,
                cursorY);

            if (descriptor.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                descriptor.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
                for (std::size_t index = 0;
                     index + 3 < composed.pixels.size();
                     index += 4) {
                    std::swap(
                        composed.pixels[index],
                        composed.pixels[index + 2]);
                }
            }
        }
    }

    bool InitializeScaleform(const F4SEMinimal::Interface* f4se)
    {
        if (!Config::EnablePipBoyPlayer()) {
            return true;
        }
        const auto* scaleform =
            static_cast<const F4SEMinimal::ScaleformInterface*>(
                f4se->QueryInterface(F4SEMinimal::kInterfaceScaleform));
        if (!scaleform ||
            scaleform->interfaceVersion <
                F4SEMinimal::ScaleformInterface::kVersion) {
            spdlog::critical("F4SE Scaleform interface v1 is unavailable");
            return false;
        }
        menuCursorSingletonOffset =
            MenuCursorOffset(f4se->runtimeVersion);
        if (menuCursorSingletonOffset == 0) {
            spdlog::warn(
                "No direct MenuCursor address is registered for runtime {}; "
                "the Pip-Boy player will use CursorMenu Scaleform coordinates",
                F4SEMinimal::VersionString(f4se->runtimeVersion));
        }
        if (!scaleform->Register(
                "MainMenuVideoPlayer",
                &ScaleformCallback)) {
            spdlog::critical(
                "F4SE rejected the MMVP Scaleform registration callback");
            return false;
        }
        spdlog::info("Registered the MMVP Pip-Boy Scaleform detector");
        return true;
    }

    void Shutdown()
    {
        SetFalloutCursorVisible(true);
        active.store(false, std::memory_order_release);
        pipBoyMovieView.store(nullptr, std::memory_order_release);
        cursorMovieView.store(nullptr, std::memory_order_release);
        pipBoyMovieRoot.store(nullptr, std::memory_order_release);
        UpdateConsumers();
        std::scoped_lock lock(uiMutex);
        composed = {};
    }

    bool Active() noexcept
    {
        return active.load(std::memory_order_acquire);
    }

    bool CommandDetectionReady() noexcept
    {
        return pipBoyMovieRoot.load(std::memory_order_acquire) != nullptr;
    }

    void PollScaleformCommand()
    {
        void* movieRoot =
            pipBoyMovieRoot.load(std::memory_order_acquire);
        if (!movieRoot) {
            return;
        }
        void** vtable = *reinterpret_cast<void***>(movieRoot);
        if (!vtable ||
            !vtable[kGetVariableVtableIndex] ||
            !vtable[kSetVariableVtableIndex]) {
            return;
        }

        using GetVariable = bool (*)(
            void*,
            ScaleformValue*,
            const char*);
        const auto getVariable = reinterpret_cast<GetVariable>(
            vtable[kGetVariableVtableIndex]);
        ScaleformValue command{};
        if (!getVariable(
                movieRoot,
                &command,
                kCommandPath.data())) {
            return;
        }

        std::int32_t commandId = 0;
        switch (command.type & kScaleformTypeMask) {
        case 2:
            commandId = command.value != 0 ? 1 : 0;
            break;
        case 3:
            commandId = static_cast<std::int32_t>(
                static_cast<std::uint32_t>(command.value));
            break;
        case 4:
            commandId = static_cast<std::int32_t>(
                static_cast<std::uint32_t>(command.value));
            break;
        case 5:
            commandId = static_cast<std::int32_t>(
                std::bit_cast<double>(command.value));
            break;
        default:
            return;
        }
        if (commandId != 1 && commandId != 2) {
            return;
        }

        ScaleformValue reset{};
        reset.type = 3;
        using SetVariable = bool (*)(
            void*,
            const char*,
            const ScaleformValue*,
            std::uint32_t);
        const auto setVariable = reinterpret_cast<SetVariable>(
            vtable[kSetVariableVtableIndex]);
        if (!setVariable(
                movieRoot,
                kCommandPath.data(),
                &reset,
                0)) {
            spdlog::warn(
                "Could not clear Pip-Boy terminal command {}",
                commandId);
            return;
        }

        ActivateRandom(
            commandId == 1 ?
                PlaybackChannel::kProjector :
                PlaybackChannel::kTelevision);
    }

    void Activate()
    {
        ActivatePlayer();
    }

    void Deactivate()
    {
        if (!active.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        SetFalloutCursorVisible(true);
        UpdateConsumers();
        spdlog::info("MMVP Pip-Boy video program deactivated");
    }

    bool HandleWindowMessage(
        const UINT message,
        const WPARAM wParam,
        const LPARAM lParam)
    {
        if (!Active()) {
            return false;
        }

        if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) &&
            (wParam == VK_TAB || wParam == VK_ESCAPE)) {
            Deactivate();
            return false;
        }
        if (message == WM_INPUT) {
            UINT size = 0;
            GetRawInputData(
                reinterpret_cast<HRAWINPUT>(lParam),
                RID_INPUT,
                nullptr,
                &size,
                sizeof(RAWINPUTHEADER));
            if (size > 0 && size <= 256) {
                std::array<std::uint8_t, 256> data{};
                if (GetRawInputData(
                        reinterpret_cast<HRAWINPUT>(lParam),
                        RID_INPUT,
                        data.data(),
                        &size,
                        sizeof(RAWINPUTHEADER)) == size) {
                    const auto* raw =
                        reinterpret_cast<const RAWINPUT*>(data.data());
                    if (raw->header.dwType == RIM_TYPEMOUSE) {
                        if ((raw->data.mouse.usButtonFlags &
                             RI_MOUSE_LEFT_BUTTON_DOWN) != 0) {
                            ExecuteClick();
                            // Fallout consumes raw mouse buttons as Pip-Boy
                            // UI clicks. Keep player clicks out of that path.
                            return true;
                        }
                    }
                }
            }
            return false;
        }
        if (message == WM_LBUTTONDOWN) {
            ExecuteClick();
            // The video controls sit over the live Pip-Boy menu. Do not let
            // this click activate whichever vanilla control is underneath.
            return true;
        }
        return false;
    }

    void TickPointer()
    {
        if (!Active()) {
            return;
        }
        float movieX = 0.0F;
        float movieY = 0.0F;
        bool directCursor = false;
        if (menuCursorSingletonOffset != 0) {
            const auto module = reinterpret_cast<std::uintptr_t>(
                GetModuleHandleW(nullptr));
            if (module != 0) {
                auto* cursor = *reinterpret_cast<MenuCursorState**>(
                    module + menuCursorSingletonOffset);
                if (cursor &&
                    cursor->maxCursorX > cursor->minCursorX &&
                    cursor->maxCursorY > cursor->minCursorY) {
                    movieX = static_cast<float>(
                        cursor->cursorPosX - cursor->minCursorX) /
                        static_cast<float>(
                            cursor->maxCursorX - cursor->minCursorX);
                    movieY = static_cast<float>(
                        cursor->cursorPosY - cursor->minCursorY) /
                        static_cast<float>(
                            cursor->maxCursorY - cursor->minCursorY);
                    directCursor = true;
                    if (!loggedCursorSource.exchange(
                            true,
                            std::memory_order_acq_rel)) {
                        spdlog::info(
                            "Tracking MenuCursor ({}, {}) inside constraints "
                            "({}, {})-({}, {})",
                            cursor->cursorPosX,
                            cursor->cursorPosY,
                            cursor->minCursorX,
                            cursor->minCursorY,
                            cursor->maxCursorX,
                            cursor->maxCursorY);
                    }
                }
            }
        }
        if (!directCursor) {
            void* movieView =
                cursorMovieView.load(std::memory_order_acquire);
            if (!movieView) {
                movieView =
                    pipBoyMovieView.load(std::memory_order_acquire);
            }
            if (!movieView) {
                return;
            }
            void** vtable = *reinterpret_cast<void***>(movieView);
            if (!vtable || !vtable[kGetMouseStateVtableIndex]) {
                return;
            }

            std::uint32_t buttons = 0;
            using GetMouseState = void (*)(
                void*,
                std::uint32_t,
                float*,
                float*,
                std::uint32_t*);
            reinterpret_cast<GetMouseState>(
                vtable[kGetMouseStateVtableIndex])(
                movieView,
                0,
                &movieX,
                &movieY,
                &buttons);
        }
        if (!std::isfinite(movieX) || !std::isfinite(movieY)) {
            return;
        }

        std::scoped_lock lock(uiMutex);
        const float nextX = std::clamp(
            directCursor ?
                movieX * static_cast<float>(ui.targetWidth - 1) :
                movieX,
            0.0F,
            static_cast<float>(ui.targetWidth - 1));
        const float nextY = std::clamp(
            directCursor ?
                movieY * static_cast<float>(ui.targetHeight - 1) :
                movieY,
            0.0F,
            static_cast<float>(ui.targetHeight - 1));
        if (nextX == ui.cursorX && nextY == ui.cursorY) {
            return;
        }
        ui.cursorX = nextX;
        ui.cursorY = nextY;
        ui.lastMotionTick = GetTickCount64();
        ++ui.revision;
    }

    bool UploadFrame(
        ID3D11DeviceContext* context,
        ID3D11Texture2D* target)
    {
        if (!Active() || !context || !target) {
            return false;
        }
        D3D11_TEXTURE2D_DESC descriptor{};
        target->GetDesc(&descriptor);
        if (descriptor.Width != Config::PipBoyWidth() ||
            descriptor.Height != Config::PipBoyHeight() ||
            (descriptor.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
             descriptor.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB &&
             descriptor.Format != DXGI_FORMAT_B8G8R8X8_UNORM &&
             descriptor.Format != DXGI_FORMAT_B8G8R8X8_UNORM_SRGB &&
             descriptor.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
             descriptor.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)) {
            return false;
        }

        const auto now = GetTickCount64();
        const std::uint32_t alpha = OverlayAlpha(now);
        float cursorX = 0.0F;
        float cursorY = 0.0F;
        std::uint64_t revision = 0;
        PlaybackChannel channel{ PlaybackChannel::kProjector };
        {
            std::scoped_lock lock(uiMutex);
            ui.targetWidth = descriptor.Width;
            ui.targetHeight = descriptor.Height;
            cursorX = ui.cursorX;
            cursorY = ui.cursorY;
            revision = ui.revision;
            channel = ui.channel;
        }
        auto* player = PlayerFor(channel);
        const auto source = player ? player->LatestFrame() : nullptr;
        const auto snapshot = player ?
            player->Snapshot() :
            PlaybackSnapshot{};
        const std::uint32_t progressBucket =
            snapshot.durationSeconds > 0.0 ?
                static_cast<std::uint32_t>(
                    std::clamp(
                        snapshot.positionSeconds /
                            snapshot.durationSeconds,
                        0.0,
                        1.0) * 1000.0) :
                0;
        const bool needsCompose =
            composed.width != descriptor.Width ||
            composed.height != descriptor.Height ||
            composed.format != descriptor.Format ||
            composed.sourceSerial != (source ? source->serial : 0) ||
            composed.uiRevision != revision ||
            composed.overlayAlpha != alpha ||
            composed.progressBucket != progressBucket ||
            composed.playbackState != snapshot.state ||
            composed.channel != channel;
        if (needsCompose) {
            Compose(
                source,
                snapshot,
                channel,
                descriptor,
                alpha,
                cursorX,
                cursorY,
                revision);
        }
        if (composed.pixels.empty()) {
            return false;
        }
        context->UpdateSubresource(
            target,
            0,
            nullptr,
            composed.pixels.data(),
            descriptor.Width * 4,
            0);
        return true;
    }
}
