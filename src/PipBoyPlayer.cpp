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
        constexpr std::size_t kGetMovieDefVtableIndex{ 0x01 };
        constexpr std::size_t kGetFileUrlVtableIndex{ 0x0C };
        constexpr std::size_t kMovieRootOffset{ 0x18 };
        constexpr std::size_t kCreateStringVtableIndex{ 0x2C };
        constexpr std::size_t kCreateFunctionVtableIndex{ 0x30 };
        constexpr std::uint32_t kScaleformTypeMask{ 0x8F };
        constexpr std::uint32_t kScaleformManagedFlag{ 1U << 6U };
        constexpr std::uint32_t kScaleformBoolType{ 2 };
        constexpr std::uint32_t kScaleformIntType{ 3 };
        constexpr std::uint32_t kScaleformUIntType{ 4 };
        constexpr std::uint32_t kScaleformStringType{ 6 };
        constexpr std::uint32_t kScaleformObjectType{ 8 };
        constexpr std::uint32_t kScaleformArrayType{ 9 };
        constexpr std::uint32_t kScaleformDisplayObjectType{ 10 };
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
        static_assert(sizeof(ScaleformValue) == 0x20);

        class ScaleformFunctionHandler
        {
        public:
            struct Parameters
            {
                ScaleformValue* result;
                void* movie;
                ScaleformValue* self;
                ScaleformValue* argumentsWithThis;
                ScaleformValue* arguments;
                std::uint32_t argumentCount;
                std::uint32_t padding;
                void* userData;
            };
            static_assert(sizeof(Parameters) == 0x38);

            virtual ~ScaleformFunctionHandler() = default;
            virtual void Call(const Parameters& parameters) = 0;

        protected:
            volatile std::int32_t referenceCount{ 1 };
            std::uint32_t padding{ 0 };
        };
        static_assert(sizeof(ScaleformFunctionHandler) == 0x10);

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

        struct ContinueItem
        {
            PlaybackChannel channel{ PlaybackChannel::kProjector };
            MediaLibrary::Item media;
            MediaProgress progress;
        };

        std::atomic<bool> active{ false };
        std::atomic<bool> interfaceActive{ false };
        std::atomic<std::int32_t> browserSelection{ 0 };
        std::atomic<std::int32_t> browserViewDepth{ 0 };
        std::atomic<std::uint64_t> browserSelectionReportTick{ 0 };
        std::atomic<std::uint64_t> browserLastClickTick{ 0 };
        std::atomic<std::int32_t> browserAcceptRequested{ -1 };
        std::atomic<std::int32_t> browserNavigationRequested{ 0 };
        std::atomic<std::uint64_t> suppressLegacyTabUntil{ 0 };
        std::atomic<std::uint32_t> scaleformCallbackCount{ 0 };
        std::atomic<bool> loggedCursorSource{ false };
        std::atomic<bool> scaleformCommandBridgeReady{ false };
        std::atomic<bool> browserBackRequested{ false };
        std::atomic<bool> browserProgressRefreshRequested{ false };
        std::mutex uiMutex;
        std::mutex composedMutex;
        std::mutex continueMutex;
        UiState ui;
        ComposedFrame composed;
        std::vector<ContinueItem> continueItems;
        std::uintptr_t menuCursorSingletonOffset{ 0 };
        std::uintptr_t scaleformSetMemberOffset{ 0 };
        std::uintptr_t scaleformObjectReleaseOffset{ 0 };

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

        struct ScaleformOffsets
        {
            std::uintptr_t setMember;
            std::uintptr_t objectRelease;
        };

        ScaleformOffsets RuntimeScaleformOffsets(
            const std::uint32_t runtime)
        {
            switch (runtime) {
            case F4SEMinimal::kRuntimeOg:
                return { 0x20D05E0, 0x20B9C80 };
            case F4SEMinimal::kRuntimeNg980:
                return { 0x19CB820, 0x19BB450 };
            case F4SEMinimal::kRuntimeNg984:
                return { 0x19CBBF0, 0x19BB820 };
            case F4SEMinimal::kRuntimeAe137:
                return { 0x1AE1170, 0x1AD0E20 };
            case F4SEMinimal::kRuntimeAe159:
                return { 0x1AE1860, 0x1AD1510 };
            case F4SEMinimal::kRuntimeAe169:
                return { 0x1AE21E0, 0x1AD1E90 };
            case F4SEMinimal::kRuntimeAe191:
                return { 0x1AE6920, 0x1AD65D0 };
            case F4SEMinimal::kRuntimeAe221:
                return { 0x1AE6A40, 0x1AD66F0 };
            default:
                return {};
            }
        }

        WorldPlaybackSession* PlayerFor(const PlaybackChannel channel)
        {
            auto& playback = WorldPlayback::GetSingleton();
            return channel == PlaybackChannel::kTelevision ?
                playback.Television() :
                playback.Projector();
        }

        std::int32_t RebuildContinueItems()
        {
            std::vector<ContinueItem> rebuilt;
            for (const auto channel : {
                     PlaybackChannel::kTelevision,
                     PlaybackChannel::kProjector }) {
                auto* player = PlayerFor(channel);
                if (!player) {
                    continue;
                }

                auto catalog = player->AvailableMedia();
                std::unordered_map<std::string, MediaLibrary::Item>
                    mediaById;
                mediaById.reserve(catalog.size());
                for (auto& media : catalog) {
                    mediaById.emplace(media.id, std::move(media));
                }

                for (auto& progress : player->ProgressHistory()) {
                    if (progress.completed ||
                        progress.positionSeconds <= 0.5) {
                        continue;
                    }
                    auto found = mediaById.find(progress.mediaId);
                    if (found == mediaById.end()) {
                        continue;
                    }
                    rebuilt.push_back(ContinueItem{
                        .channel = channel,
                        .media = found->second,
                        .progress = std::move(progress)
                    });
                }
            }

            std::ranges::sort(
                rebuilt,
                std::greater{},
                [](const ContinueItem& item) {
                    return item.progress.lastPlayedMilliseconds;
                });
            if (rebuilt.size() > 10000) {
                rebuilt.resize(10000);
            }
            const auto count =
                static_cast<std::int32_t>(rebuilt.size());
            {
                std::scoped_lock lock(continueMutex);
                continueItems = std::move(rebuilt);
            }
            return count;
        }

        std::optional<ContinueItem> ContinueItemAt(
            const std::int32_t index)
        {
            if (index < 0) {
                return std::nullopt;
            }
            std::scoped_lock lock(continueMutex);
            const auto converted = static_cast<std::size_t>(index);
            return converted < continueItems.size() ?
                std::optional<ContinueItem>(continueItems[converted]) :
                std::nullopt;
        }

        void SetBrowserActive(bool value, bool reset = true);

        std::optional<PlaybackChannel> BrowserChannel(
            const std::int32_t value)
        {
            switch (value) {
            case 1:
                return PlaybackChannel::kProjector;
            case 2:
                return PlaybackChannel::kTelevision;
            default:
                return std::nullopt;
            }
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

        void ActivatePlayer(const bool startIfNeeded = true)
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
                InputRouter::SetRawInputCapture(true);
                spdlog::debug(
                    "Pip-Boy player is using the shared input router");
            }
            UpdateConsumers();

            if (startIfNeeded) {
                StartPlayerIfNeeded(SelectedPlayer());
            }
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
            {
                std::scoped_lock lock(composedMutex);
                composed = {};
            }
            player->RefreshLibrary();
            player->Next();
            // Next() already queued the requested random item. Do not issue
            // another startup request while activating the native overlay.
            ActivatePlayer(false);
            spdlog::info(
                "Accepted Pip-Boy terminal request for a random {} video",
                PlaybackChannelName(channel));
        }

        bool ActivateMedia(
            const PlaybackChannel channel,
            const std::string_view mediaId)
        {
            auto* player = PlayerFor(channel);
            if (!player || mediaId.empty() || !player->Select(mediaId)) {
                return false;
            }
            {
                std::scoped_lock lock(uiMutex);
                ui.channel = channel;
                ++ui.revision;
            }
            {
                std::scoped_lock lock(composedMutex);
                composed = {};
            }
            // Select() already queued this exact item and its saved position.
            // Calling the generic startup helper here could race it with an
            // extra Next() and replace it with a random playlist item.
            ActivatePlayer(false);
            SetBrowserActive(false);
            spdlog::info(
                "Accepted exact Pip-Boy {} media id {}",
                PlaybackChannelName(channel),
                mediaId);
            return true;
        }

        void ActivateResume()
        {
            struct Candidate
            {
                PlaybackChannel channel;
                MediaProgress progress;
            };

            std::vector<Candidate> candidates;
            for (const auto channel : {
                     PlaybackChannel::kTelevision,
                     PlaybackChannel::kProjector }) {
                auto* player = PlayerFor(channel);
                if (!player) {
                    continue;
                }
                if (auto progress = player->MostRecentProgress()) {
                    candidates.push_back({
                        .channel = channel,
                        .progress = std::move(*progress)
                    });
                }
            }
            std::ranges::sort(
                candidates,
                std::greater{},
                [](const Candidate& candidate) {
                    return candidate.progress.lastPlayedMilliseconds;
                });

            for (const auto& candidate : candidates) {
                if (ActivateMedia(
                        candidate.channel,
                        candidate.progress.mediaId)) {
                    spdlog::info(
                        "Resumed most recent MMVP media at {:.1f} seconds",
                        candidate.progress.completed ?
                            0.0 :
                            candidate.progress.positionSeconds);
                    return;
                }
            }

            spdlog::info(
                "MMVP Resume has no saved media history; "
                "continuing the current player");
            ActivatePlayer();
        }

        void SetBrowserActive(
            const bool value,
            const bool reset)
        {
            const bool wasActive =
                interfaceActive.exchange(value, std::memory_order_acq_rel);
            browserBackRequested.store(false, std::memory_order_release);
            if (value && reset) {
                browserSelection.store(0, std::memory_order_release);
                browserViewDepth.store(0, std::memory_order_release);
                browserSelectionReportTick.store(
                    0,
                    std::memory_order_release);
                browserLastClickTick.store(0, std::memory_order_release);
                browserAcceptRequested.store(-1, std::memory_order_release);
                browserNavigationRequested.store(
                    0,
                    std::memory_order_release);
            }
            if (wasActive != value) {
                spdlog::info(
                    "MMVP holotape browser input lock {}",
                    value ? "activated" : "released");
            }
            InputRouter::SetRawInputCapture(value || Active());
        }

        void ActivateBrowserSelection(const std::int32_t selection)
        {
            switch (selection) {
            case 0:
                ActivateRandom(PlaybackChannel::kProjector);
                break;
            case 1:
                ActivateRandom(PlaybackChannel::kTelevision);
                break;
            case 2:
                ActivateRandom(
                    (GetTickCount64() & 1U) != 0 ?
                        PlaybackChannel::kProjector :
                        PlaybackChannel::kTelevision);
                break;
            case 3:
                ActivateResume();
                break;
            default:
                break;
            }
            SetBrowserActive(false);
        }

        void SetScaleformBoolean(
            ScaleformValue* result,
            const bool value)
        {
            if (!result) {
                return;
            }
            result->objectInterface = nullptr;
            result->type = kScaleformBoolType;
            result->value = value ? 1U : 0U;
            result->unknown = nullptr;
        }

        void SetScaleformInteger(
            ScaleformValue* result,
            const std::int32_t value)
        {
            if (!result) {
                return;
            }
            result->objectInterface = nullptr;
            result->type = kScaleformIntType;
            result->value = static_cast<std::uint32_t>(value);
            result->unknown = nullptr;
        }

        void SetScaleformString(
            const ScaleformFunctionHandler::Parameters& parameters,
            const std::string_view value)
        {
            if (!parameters.result || !parameters.movie) {
                return;
            }
            void* movieRoot = *reinterpret_cast<void**>(
                static_cast<std::byte*>(parameters.movie) +
                kMovieRootOffset);
            if (!movieRoot) {
                return;
            }
            void** vtable = *reinterpret_cast<void***>(movieRoot);
            if (!vtable || !vtable[kCreateStringVtableIndex]) {
                return;
            }
            const std::string copy(value);
            using CreateString = void (*)(
                void*,
                ScaleformValue*,
                const char*);
            reinterpret_cast<CreateString>(
                vtable[kCreateStringVtableIndex])(
                movieRoot,
                parameters.result,
                copy.c_str());
        }

        std::optional<std::int32_t> ScaleformInteger(
            const ScaleformValue& value)
        {
            switch (value.type & kScaleformTypeMask) {
            case 2:
            case 3:
            case 4:
                return static_cast<std::int32_t>(
                    static_cast<std::uint32_t>(value.value));
            case 5:
                return static_cast<std::int32_t>(
                    std::bit_cast<double>(value.value));
            default:
                return std::nullopt;
            }
        }

        std::optional<std::string_view> ScaleformString(
            const ScaleformValue& value)
        {
            if ((value.type & kScaleformTypeMask) !=
                kScaleformStringType) {
                return std::nullopt;
            }
            const char* text = nullptr;
            if ((value.type & kScaleformManagedFlag) != 0) {
                const char* const* managed =
                    reinterpret_cast<const char* const*>(value.value);
                text = managed ? *managed : nullptr;
            } else {
                text = reinterpret_cast<const char*>(value.value);
            }
            return text ? std::optional<std::string_view>(text) :
                          std::nullopt;
        }

        class GetApiVersionHandler final :
            public ScaleformFunctionHandler
        {
        public:
            void Call(const Parameters& parameters) override
            {
                if (!parameters.result) {
                    return;
                }
                parameters.result->objectInterface = nullptr;
                parameters.result->type = kScaleformUIntType;
                parameters.result->value = 4;
                parameters.result->unknown = nullptr;
            }
        };

        class PlayCommandHandler final :
            public ScaleformFunctionHandler
        {
        public:
            void Call(const Parameters& parameters) override
            {
                bool accepted = false;
                if (InterfaceActive() &&
                    parameters.arguments &&
                    parameters.argumentCount >= 1) {
                    const auto command =
                        ScaleformInteger(parameters.arguments[0]);
                    if (command) {
                        switch (*command) {
                        case 1:
                            ActivateBrowserSelection(0);
                            accepted = true;
                            break;
                        case 2:
                            ActivateBrowserSelection(1);
                            accepted = true;
                            break;
                        case 3:
                            ActivateBrowserSelection(3);
                            accepted = true;
                            break;
                        default:
                            break;
                        }
                        if (accepted) {
                            spdlog::info(
                                "Accepted MMVP Scaleform playback command {}",
                                *command);
                        }
                    }
                }
                SetScaleformBoolean(parameters.result, accepted);
            }
        };

        class SetSelectionHandler final :
            public ScaleformFunctionHandler
        {
        public:
            void Call(const Parameters& parameters) override
            {
                bool accepted = false;
                if (InterfaceActive() &&
                    parameters.arguments &&
                    parameters.argumentCount >= 1) {
                    const auto selection =
                        ScaleformInteger(parameters.arguments[0]);
                    if (selection && *selection >= 0 && *selection < 4) {
                        const auto previous = browserSelection.exchange(
                            *selection,
                            std::memory_order_acq_rel);
                        browserSelectionReportTick.store(
                            GetTickCount64(),
                            std::memory_order_release);
                        if (parameters.argumentCount >= 2) {
                            const auto depth =
                                ScaleformInteger(parameters.arguments[1]);
                            if (depth) {
                                browserViewDepth.store(
                                    std::clamp(*depth, 0, 1),
                                    std::memory_order_release);
                            }
                        }
                        if (previous != *selection) {
                            spdlog::info(
                                "MMVP Scaleform selected browser choice {}",
                                *selection);
                        }
                        accepted = true;
                    }
                }
                SetScaleformBoolean(parameters.result, accepted);
            }
        };

        class RefreshMediaHandler final :
            public ScaleformFunctionHandler
        {
        public:
            void Call(const Parameters& parameters) override
            {
                std::int32_t count = 0;
                if (InterfaceActive() &&
                    parameters.arguments &&
                    parameters.argumentCount >= 1) {
                    const auto rawChannel =
                        ScaleformInteger(parameters.arguments[0]);
                    const auto channel = rawChannel ?
                        BrowserChannel(*rawChannel) :
                        std::nullopt;
                    auto* player = channel ?
                        PlayerFor(*channel) :
                        nullptr;
                    if (player) {
                        // Browsing may rescan the catalog, but it must not
                        // interrupt or advance either playback session.
                        player->RefreshCatalog();
                        count = static_cast<std::int32_t>(
                            std::min<std::size_t>(
                                player->MediaCount(),
                                10000));
                    }
                }
                SetScaleformInteger(parameters.result, count);
            }
        };

        class GetMediaCountHandler final :
            public ScaleformFunctionHandler
        {
        public:
            void Call(const Parameters& parameters) override
            {
                std::int32_t count = 0;
                if (InterfaceActive() &&
                    parameters.arguments &&
                    parameters.argumentCount >= 1) {
                    const auto rawChannel =
                        ScaleformInteger(parameters.arguments[0]);
                    const auto channel = rawChannel ?
                        BrowserChannel(*rawChannel) :
                        std::nullopt;
                    auto* player = channel ?
                        PlayerFor(*channel) :
                        nullptr;
                    if (player) {
                        count = static_cast<std::int32_t>(
                            std::min<std::size_t>(
                                player->MediaCount(),
                                10000));
                    }
                }
                SetScaleformInteger(parameters.result, count);
            }
        };

        std::optional<MediaLibrary::Item> ScaleformMediaItem(
            const ScaleformFunctionHandler::Parameters& parameters)
        {
            if (!InterfaceActive() ||
                !parameters.arguments ||
                parameters.argumentCount < 2) {
                return std::nullopt;
            }
            const auto rawChannel =
                ScaleformInteger(parameters.arguments[0]);
            const auto rawIndex =
                ScaleformInteger(parameters.arguments[1]);
            const auto channel = rawChannel ?
                BrowserChannel(*rawChannel) :
                std::nullopt;
            auto* player = channel ? PlayerFor(*channel) : nullptr;
            if (!player || !rawIndex || *rawIndex < 0) {
                return std::nullopt;
            }
            const auto index = static_cast<std::size_t>(*rawIndex);
            return player->MediaAt(index);
        }

        class GetMediaIdHandler final :
            public ScaleformFunctionHandler
        {
        public:
            void Call(const Parameters& parameters) override
            {
                const auto item = ScaleformMediaItem(parameters);
                SetScaleformString(
                    parameters,
                    item ? item->id : std::string_view{});
            }
        };

        class GetMediaLabelHandler final :
            public ScaleformFunctionHandler
        {
        public:
            void Call(const Parameters& parameters) override
            {
                const auto item = ScaleformMediaItem(parameters);
                SetScaleformString(
                    parameters,
                    item ? item->displayName : std::string_view{});
            }
        };

        class GetMediaProgressHandler final :
            public ScaleformFunctionHandler
        {
        public:
            void Call(const Parameters& parameters) override
            {
                std::int32_t percentage = -1;
                if (InterfaceActive() &&
                    parameters.arguments &&
                    parameters.argumentCount >= 2) {
                    const auto rawChannel =
                        ScaleformInteger(parameters.arguments[0]);
                    const auto mediaId =
                        ScaleformString(parameters.arguments[1]);
                    const auto channel = rawChannel ?
                        BrowserChannel(*rawChannel) :
                        std::nullopt;
                    auto* player = channel ?
                        PlayerFor(*channel) :
                        nullptr;
                    if (player && mediaId) {
                        const auto progress = player->Progress(*mediaId);
                        if (progress) {
                            if (progress->completed) {
                                percentage = 100;
                            } else if (progress->durationSeconds > 0.0) {
                                percentage = std::clamp(
                                    static_cast<std::int32_t>(
                                        std::lround(
                                            progress->positionSeconds /
                                            progress->durationSeconds *
                                            100.0)),
                                    0,
                                    99);
                            } else {
                                percentage = 0;
                            }
                        }
                    }
                }
                SetScaleformInteger(parameters.result, percentage);
            }
        };

        class RefreshContinueHandler final :
            public ScaleformFunctionHandler
        {
        public:
            void Call(const Parameters& parameters) override
            {
                const auto count =
                    InterfaceActive() ? RebuildContinueItems() : 0;
                SetScaleformInteger(parameters.result, count);
            }
        };

        std::optional<ContinueItem> ScaleformContinueItem(
            const ScaleformFunctionHandler::Parameters& parameters)
        {
            if (!InterfaceActive() ||
                !parameters.arguments ||
                parameters.argumentCount < 1) {
                return std::nullopt;
            }
            const auto rawIndex =
                ScaleformInteger(parameters.arguments[0]);
            return rawIndex ? ContinueItemAt(*rawIndex) : std::nullopt;
        }

        class GetContinueChannelHandler final :
            public ScaleformFunctionHandler
        {
        public:
            void Call(const Parameters& parameters) override
            {
                const auto item = ScaleformContinueItem(parameters);
                const auto channel = item &&
                                     item->channel ==
                                         PlaybackChannel::kTelevision ?
                    2 :
                    item ? 1 : 0;
                SetScaleformInteger(parameters.result, channel);
            }
        };

        class GetContinueIdHandler final :
            public ScaleformFunctionHandler
        {
        public:
            void Call(const Parameters& parameters) override
            {
                const auto item = ScaleformContinueItem(parameters);
                SetScaleformString(
                    parameters,
                    item ? item->media.id : std::string_view{});
            }
        };

        class GetContinueLabelHandler final :
            public ScaleformFunctionHandler
        {
        public:
            void Call(const Parameters& parameters) override
            {
                const auto item = ScaleformContinueItem(parameters);
                SetScaleformString(
                    parameters,
                    item ? item->media.displayName :
                           std::string_view{});
            }
        };

        class PlayMediaHandler final :
            public ScaleformFunctionHandler
        {
        public:
            void Call(const Parameters& parameters) override
            {
                bool accepted = false;
                if (InterfaceActive() &&
                    parameters.arguments &&
                    parameters.argumentCount >= 2) {
                    const auto rawChannel =
                        ScaleformInteger(parameters.arguments[0]);
                    const auto mediaId =
                        ScaleformString(parameters.arguments[1]);
                    const auto channel = rawChannel ?
                        BrowserChannel(*rawChannel) :
                        std::nullopt;
                    if (channel && mediaId) {
                        accepted = ActivateMedia(*channel, *mediaId);
                    }
                }
                SetScaleformBoolean(parameters.result, accepted);
            }
        };

        class ConsumeAcceptRequestHandler final :
            public ScaleformFunctionHandler
        {
        public:
            void Call(const Parameters& parameters) override
            {
                const auto selection =
                    InterfaceActive() ?
                        browserAcceptRequested.exchange(
                            -1,
                            std::memory_order_acq_rel) :
                        -1;
                SetScaleformInteger(parameters.result, selection);
            }
        };

        class ConsumeNavigationRequestHandler final :
            public ScaleformFunctionHandler
        {
        public:
            void Call(const Parameters& parameters) override
            {
                const auto navigation =
                    InterfaceActive() ?
                        browserNavigationRequested.exchange(
                            0,
                            std::memory_order_acq_rel) :
                        0;
                SetScaleformInteger(parameters.result, navigation);
            }
        };

        class ConsumeBackRequestHandler final :
            public ScaleformFunctionHandler
        {
        public:
            void Call(const Parameters& parameters) override
            {
                const bool requested =
                    InterfaceActive() &&
                    browserBackRequested.exchange(
                        false,
                        std::memory_order_acq_rel);
                if (requested) {
                    spdlog::info(
                        "MMVP browser SWF consumed the holotape Back request");
                }
                SetScaleformBoolean(parameters.result, requested);
            }
        };

        class ConsumeProgressRefreshHandler final :
            public ScaleformFunctionHandler
        {
        public:
            void Call(const Parameters& parameters) override
            {
                const bool requested =
                    InterfaceActive() &&
                    browserProgressRefreshRequested.exchange(
                        false,
                        std::memory_order_acq_rel);
                SetScaleformBoolean(parameters.result, requested);
            }
        };

        bool RegisterScaleformFunction(
            void* rawView,
            void* rawRoot,
            const char* name,
            ScaleformFunctionHandler* handler)
        {
            if (!rawView || !rawRoot || !name || !handler ||
                scaleformSetMemberOffset == 0 ||
                scaleformObjectReleaseOffset == 0) {
                return false;
            }
            auto* root = static_cast<ScaleformValue*>(rawRoot);
            const auto rootType = root->type & kScaleformTypeMask;
            if (!root->objectInterface ||
                (rootType != kScaleformObjectType &&
                 rootType != kScaleformArrayType &&
                 rootType != kScaleformDisplayObjectType)) {
                return false;
            }

            void* movieRoot = *reinterpret_cast<void**>(
                static_cast<std::byte*>(rawView) + kMovieRootOffset);
            if (!movieRoot) {
                return false;
            }
            void** vtable = *reinterpret_cast<void***>(movieRoot);
            if (!vtable || !vtable[kCreateFunctionVtableIndex]) {
                return false;
            }

            ScaleformValue function{};
            using CreateFunction = void (*)(
                void*,
                ScaleformValue*,
                ScaleformFunctionHandler*,
                void*);
            reinterpret_cast<CreateFunction>(
                vtable[kCreateFunctionVtableIndex])(
                movieRoot,
                &function,
                handler,
                nullptr);

            const auto module = reinterpret_cast<std::uintptr_t>(
                GetModuleHandleW(nullptr));
            using SetMember = bool (*)(
                void*,
                void*,
                const char*,
                const ScaleformValue*,
                bool);
            const bool registered = reinterpret_cast<SetMember>(
                module + scaleformSetMemberOffset)(
                root->objectInterface,
                reinterpret_cast<void*>(root->value),
                name,
                &function,
                rootType == kScaleformDisplayObjectType);

            if ((function.type & kScaleformManagedFlag) != 0 &&
                function.objectInterface) {
                using ObjectRelease = void (*)(
                    void*,
                    ScaleformValue*,
                    void*);
                reinterpret_cast<ObjectRelease>(
                    module + scaleformObjectReleaseOffset)(
                    function.objectInterface,
                    &function,
                    reinterpret_cast<void*>(function.value));
            }
            return registered;
        }

        bool RegisterCommandBridge(void* rawView, void* rawRoot)
        {
            static auto* getApiVersion = new GetApiVersionHandler();
            static auto* playCommand = new PlayCommandHandler();
            static auto* setSelection = new SetSelectionHandler();
            static auto* consumeBackRequest =
                new ConsumeBackRequestHandler();
            static auto* consumeAcceptRequest =
                new ConsumeAcceptRequestHandler();
            static auto* consumeNavigationRequest =
                new ConsumeNavigationRequestHandler();
            static auto* consumeProgressRefresh =
                new ConsumeProgressRefreshHandler();
            static auto* refreshMedia = new RefreshMediaHandler();
            static auto* getMediaCount = new GetMediaCountHandler();
            static auto* getMediaId = new GetMediaIdHandler();
            static auto* getMediaLabel = new GetMediaLabelHandler();
            static auto* getMediaProgress =
                new GetMediaProgressHandler();
            static auto* refreshContinue =
                new RefreshContinueHandler();
            static auto* getContinueChannel =
                new GetContinueChannelHandler();
            static auto* getContinueId =
                new GetContinueIdHandler();
            static auto* getContinueLabel =
                new GetContinueLabelHandler();
            static auto* playMedia = new PlayMediaHandler();
            const bool apiRegistered = RegisterScaleformFunction(
                rawView,
                rawRoot,
                "getApiVersion",
                getApiVersion);
            const bool playRegistered = RegisterScaleformFunction(
                rawView,
                rawRoot,
                "playCommand",
                playCommand);
            const bool selectionRegistered = RegisterScaleformFunction(
                rawView,
                rawRoot,
                "setSelection",
                setSelection);
            const bool backRegistered = RegisterScaleformFunction(
                rawView,
                rawRoot,
                "consumeBackRequest",
                consumeBackRequest);
            const bool acceptRegistered = RegisterScaleformFunction(
                rawView,
                rawRoot,
                "consumeAcceptRequest",
                consumeAcceptRequest);
            const bool navigationRegistered = RegisterScaleformFunction(
                rawView,
                rawRoot,
                "consumeNavigationRequest",
                consumeNavigationRequest);
            const bool progressRefreshRegistered =
                RegisterScaleformFunction(
                    rawView,
                    rawRoot,
                    "consumeProgressRefresh",
                    consumeProgressRefresh);
            const bool refreshRegistered = RegisterScaleformFunction(
                rawView,
                rawRoot,
                "refreshMedia",
                refreshMedia);
            const bool countRegistered = RegisterScaleformFunction(
                rawView,
                rawRoot,
                "getMediaCount",
                getMediaCount);
            const bool idRegistered = RegisterScaleformFunction(
                rawView,
                rawRoot,
                "getMediaId",
                getMediaId);
            const bool labelRegistered = RegisterScaleformFunction(
                rawView,
                rawRoot,
                "getMediaLabel",
                getMediaLabel);
            const bool progressRegistered = RegisterScaleformFunction(
                rawView,
                rawRoot,
                "getMediaProgress",
                getMediaProgress);
            const bool continueRefreshRegistered =
                RegisterScaleformFunction(
                    rawView,
                    rawRoot,
                    "refreshContinue",
                    refreshContinue);
            const bool continueChannelRegistered =
                RegisterScaleformFunction(
                    rawView,
                    rawRoot,
                    "getContinueChannel",
                    getContinueChannel);
            const bool continueIdRegistered =
                RegisterScaleformFunction(
                    rawView,
                    rawRoot,
                    "getContinueId",
                    getContinueId);
            const bool continueLabelRegistered =
                RegisterScaleformFunction(
                    rawView,
                    rawRoot,
                    "getContinueLabel",
                    getContinueLabel);
            const bool exactPlayRegistered = RegisterScaleformFunction(
                rawView,
                rawRoot,
                "playMedia",
                playMedia);
            return apiRegistered &&
                   playRegistered &&
                   selectionRegistered &&
                   backRegistered &&
                   acceptRegistered &&
                   navigationRegistered &&
                   progressRefreshRegistered &&
                   refreshRegistered &&
                   countRegistered &&
                   idRegistered &&
                   labelRegistered &&
                   progressRegistered &&
                   continueRefreshRegistered &&
                   continueChannelRegistered &&
                   continueIdRegistered &&
                   continueLabelRegistered &&
                   exactPlayRegistered;
        }

        bool BrowserCursorPosition(
            float& stageX,
            float& stageY,
            float& normalizedY)
        {
            if (menuCursorSingletonOffset == 0) {
                return false;
            }
            const auto module = reinterpret_cast<std::uintptr_t>(
                GetModuleHandleW(nullptr));
            if (module == 0) {
                return false;
            }
            const auto* cursor = *reinterpret_cast<MenuCursorState**>(
                module + menuCursorSingletonOffset);
            if (!cursor ||
                cursor->maxCursorX <= cursor->minCursorX ||
                cursor->maxCursorY <= cursor->minCursorY) {
                return false;
            }

            constexpr float kBrowserWidth{ 826.0F };
            constexpr float kBrowserHeight{ 700.0F };
            stageX =
                static_cast<float>(
                    cursor->cursorPosX - cursor->minCursorX) /
                static_cast<float>(
                    cursor->maxCursorX - cursor->minCursorX) *
                kBrowserWidth;
            normalizedY =
                static_cast<float>(
                    cursor->cursorPosY - cursor->minCursorY) /
                static_cast<float>(
                    cursor->maxCursorY - cursor->minCursorY);

            // The 826x700 holotape stage is projected onto the curved Pip-Boy
            // screen rather than stretched across the full desktop cursor
            // range. The visible screen occupies approximately 13.5%-75.5%
            // of the vertical MenuCursor range in the vanilla presentation.
            // Convert that projected interval back into program-stage space;
            // the former full-window mapping compressed all four rows into
            // the first choice.
            constexpr float kProjectedTop{ 0.135F };
            constexpr float kProjectedHeight{ 0.620F };
            stageY =
                (normalizedY - kProjectedTop) /
                kProjectedHeight *
                kBrowserHeight;
            return std::isfinite(stageX) && std::isfinite(stageY);
        }

        std::optional<std::int32_t> BrowserCursorSelection()
        {
            if (!interfaceActive.load(std::memory_order_acquire)) {
                return std::nullopt;
            }

            float x = 0.0F;
            float y = 0.0F;
            float normalizedY = 0.0F;
            if (!BrowserCursorPosition(x, y, normalizedY)) {
                spdlog::warn(
                    "Could not resolve the native cursor for an MMVP "
                    "browser click");
                return std::nullopt;
            }
            spdlog::info(
                "MMVP browser click at stage ({:.1f}, {:.1f}), "
                "projected cursor Y {:.4f}",
                x,
                y,
                normalizedY);

            constexpr float kSafeX{ 37.17F };
            constexpr float kSafeWidth{ 751.66F };
            constexpr float kButtonX{ kSafeX + 20.0F };
            constexpr float kButtonWidth{ kSafeWidth - 40.0F };
            if (x < kButtonX || x > kButtonX + kButtonWidth) {
                return std::nullopt;
            }

            // The curved Pip-Boy screen applies a visibly non-linear
            // projection. Calibrate selection boundaries from the actual
            // MenuCursor positions observed over this SWF's four rows:
            // Movies ~= .202, TV ~= .274, Random ~= .380-.402, Resume ~=
            // .498. A single affine stage transform shifted each successive
            // row upward by one.
            constexpr float kBrowserTop{ 0.155F };
            constexpr float kMoviesTelevisionBoundary{ 0.238F };
            constexpr float kTelevisionRandomBoundary{ 0.327F };
            constexpr float kRandomResumeBoundary{ 0.450F };
            constexpr float kBrowserBottom{ 0.565F };
            if (normalizedY < kBrowserTop ||
                normalizedY > kBrowserBottom) {
                return std::nullopt;
            }
            const std::int32_t index =
                normalizedY < kMoviesTelevisionBoundary ? 0 :
                normalizedY < kTelevisionRandomBoundary ? 1 :
                normalizedY < kRandomResumeBoundary ? 2 :
                                                           3;
            spdlog::info(
                "Accepted native MMVP browser click for choice {}",
                index);
            return index;
        }

        void ExecuteBrowserInputClick()
        {
            const auto now = GetTickCount64();
            const auto previous = browserLastClickTick.exchange(
                now,
                std::memory_order_acq_rel);
            if (now - previous < 120) {
                return;
            }

            const auto reportTick =
                browserSelectionReportTick.load(std::memory_order_acquire);
            std::optional<std::int32_t> selection;
            if (scaleformCommandBridgeReady.load(
                    std::memory_order_acquire) &&
                reportTick != 0 &&
                now - reportTick <= 1000) {
                selection =
                    browserSelection.load(std::memory_order_acquire);
                spdlog::info(
                    "Accepted fresh MMVP Scaleform browser choice {}",
                    *selection);
            } else {
                selection = BrowserCursorSelection();
            }
            if (selection) {
                browserSelection.store(
                    *selection,
                    std::memory_order_release);
                browserAcceptRequested.store(
                    *selection,
                    std::memory_order_release);
                spdlog::info(
                    "Queued MMVP browser Accept for SWF choice {}",
                    *selection);
            }
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

        bool ScaleformCallback(void* rawView, void* rawRoot)
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
                spdlog::info(
                    "Observed MMVP Pip-Boy Scaleform movie '{}'",
                    movieUrl);
                return true;
            }
            if (movieUrl &&
                EndsWithInsensitive(
                    movieUrl,
                    "MMVPBrowser.swf")) {
                const bool commandBridgeRegistered =
                    RegisterCommandBridge(rawView, rawRoot);
                scaleformCommandBridgeReady.store(
                    commandBridgeRegistered,
                    std::memory_order_release);
                SetBrowserActive(true);
                spdlog::info(
                    "Detected active MMVP holotape browser '{}' "
                    "(command bridge={})",
                    movieUrl,
                    commandBridgeRegistered);
                return true;
            }
            if (movieUrl &&
                EndsWithInsensitive(
                    movieUrl,
                    "CursorMenu.swf")) {
                scaleformCommandBridgeReady.store(
                    false,
                    std::memory_order_release);
                if (interfaceActive.load(std::memory_order_acquire)) {
                    SetBrowserActive(false);
                }
                spdlog::info(
                    "Observed Fallout cursor Scaleform movie '{}'",
                    movieUrl);
                return true;
            }
            if (movieUrl &&
                (EndsWithInsensitive(movieUrl, "Console.swf") ||
                 EndsWithInsensitive(movieUrl, "HUDMenu.swf") ||
                 EndsWithInsensitive(movieUrl, "LoadingMenu.swf") ||
                 EndsWithInsensitive(movieUrl, "MainMenu.swf"))) {
                scaleformCommandBridgeReady.store(
                    false,
                    std::memory_order_release);
                if (Active()) {
                    Deactivate();
                }
                if (InterfaceActive()) {
                    SetBrowserActive(false);
                }
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
        const auto scaleformOffsets =
            RuntimeScaleformOffsets(f4se->runtimeVersion);
        scaleformSetMemberOffset = scaleformOffsets.setMember;
        scaleformObjectReleaseOffset = scaleformOffsets.objectRelease;
        if (scaleformSetMemberOffset == 0 ||
            scaleformObjectReleaseOffset == 0) {
            spdlog::critical(
                "No Scaleform bridge offsets are registered for runtime {}",
                F4SEMinimal::VersionString(f4se->runtimeVersion));
            return false;
        }
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
        active.store(false, std::memory_order_release);
        interfaceActive.store(false, std::memory_order_release);
        browserSelection.store(0, std::memory_order_release);
        browserViewDepth.store(0, std::memory_order_release);
        browserSelectionReportTick.store(0, std::memory_order_release);
        browserLastClickTick.store(0, std::memory_order_release);
        browserAcceptRequested.store(-1, std::memory_order_release);
        browserNavigationRequested.store(0, std::memory_order_release);
        browserBackRequested.store(false, std::memory_order_release);
        browserProgressRefreshRequested.store(
            false,
            std::memory_order_release);
        {
            std::scoped_lock lock(continueMutex);
            continueItems.clear();
        }
        suppressLegacyTabUntil.store(0, std::memory_order_release);
        scaleformCommandBridgeReady.store(false, std::memory_order_release);
        InputRouter::SetRawInputCapture(false);
        UpdateConsumers();
        std::scoped_lock lock(composedMutex);
        composed = {};
    }

    bool Active() noexcept
    {
        return active.load(std::memory_order_acquire);
    }

    bool InterfaceActive() noexcept
    {
        return interfaceActive.load(std::memory_order_acquire);
    }

    void Activate()
    {
        ActivatePlayer();
    }

    void Deactivate()
    {
        const bool wasActive =
            active.exchange(false, std::memory_order_acq_rel);
        InputRouter::SetRawInputCapture(InterfaceActive());
        if (!wasActive) {
            return;
        }
        UpdateConsumers();
        spdlog::info("MMVP Pip-Boy video program deactivated");
    }

    bool HandleWindowMessage(
        const UINT message,
        const WPARAM wParam,
        const LPARAM lParam)
    {
        if ((message == WM_KEYDOWN ||
             message == WM_SYSKEYDOWN ||
             message == WM_KEYUP ||
             message == WM_SYSKEYUP) &&
            wParam == VK_TAB) {
            const auto suppressUntil =
                suppressLegacyTabUntil.load(std::memory_order_acquire);
            if (suppressUntil != 0 &&
                GetTickCount64() <= suppressUntil) {
                if (message == WM_KEYUP || message == WM_SYSKEYUP) {
                    suppressLegacyTabUntil.store(
                        0,
                        std::memory_order_release);
                }
                return true;
            }
        }

        const bool playerActive = Active();
        const bool browserActive = InterfaceActive();
        if (!playerActive && !browserActive) {
            return false;
        }

        const auto isBrowserNavigationKey = [](const USHORT key) {
            return key == VK_UP ||
                   key == VK_DOWN ||
                   key == VK_LEFT ||
                   key == VK_RIGHT ||
                   key == VK_RETURN ||
                   key == VK_SPACE;
        };

        if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) {
            if (wParam == VK_TAB) {
                if (playerActive) {
                    // The native player is an interim overlay over the still
                    // loaded browser SWF. First Tab removes only that overlay
                    // and returns input ownership to the browser. A later Tab
                    // reaches Fallout and closes the holotape program.
                    suppressLegacyTabUntil.store(
                        GetTickCount64() + 250,
                        std::memory_order_release);
                    Deactivate();
                    browserProgressRefreshRequested.store(
                        true,
                        std::memory_order_release);
                    SetBrowserActive(true, false);
                    return true;
                }
                if (browserViewDepth.load(
                        std::memory_order_acquire) > 0 &&
                    scaleformCommandBridgeReady.load(
                        std::memory_order_acquire)) {
                    suppressLegacyTabUntil.store(
                        GetTickCount64() + 250,
                        std::memory_order_release);
                    browserBackRequested.store(
                        true,
                        std::memory_order_release);
                    return true;
                }
                SetBrowserActive(false);
                // Match Holo-Wind's proven eject path: after releasing MMVP's
                // raw-input ownership, let Fallout receive both the normal
                // Tab message and its raw event. PipboyHolotapeMenu then runs
                // its native ProcessCancel/eject behavior.
                return false;
            }
            if (browserActive) {
                if (isBrowserNavigationKey(
                        static_cast<USHORT>(wParam))) {
                    return true;
                }
            }
            return true;
        }
        if (message == WM_KEYUP || message == WM_SYSKEYUP) {
            if (wParam == VK_TAB) {
                return false;
            }
            return true;
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
                    if (raw->header.dwType == RIM_TYPEKEYBOARD) {
                        const auto& keyboard = raw->data.keyboard;
                        if ((keyboard.Flags & RI_KEY_BREAK) != 0) {
                            return true;
                        }
                        const USHORT key = keyboard.VKey;
                        if (key == VK_TAB) {
                            const auto now = GetTickCount64();
                            const auto suppressUntil =
                                suppressLegacyTabUntil.load(
                                    std::memory_order_acquire);
                            if (suppressUntil != 0 &&
                                now <= suppressUntil) {
                                return true;
                            }
                            if (playerActive) {
                                suppressLegacyTabUntil.store(
                                    now + 250,
                                    std::memory_order_release);
                                Deactivate();
                                browserProgressRefreshRequested.store(
                                    true,
                                    std::memory_order_release);
                                SetBrowserActive(true, false);
                                return true;
                            }
                            if (browserViewDepth.load(
                                    std::memory_order_acquire) > 0 &&
                                scaleformCommandBridgeReady.load(
                                    std::memory_order_acquire)) {
                                suppressLegacyTabUntil.store(
                                    now + 250,
                                    std::memory_order_release);
                                browserBackRequested.store(
                                    true,
                                    std::memory_order_release);
                                return true;
                            }
                            SetBrowserActive(false);
                            // Holo-Wind passes Tab through in both raw and
                            // legacy form so Fallout's holotape menu performs
                            // its normal eject. Do the same after cleanup.
                            return false;
                        }
                        if (browserActive &&
                            (key == VK_UP || key == 'W')) {
                            browserNavigationRequested.fetch_sub(
                                1,
                                std::memory_order_release);
                            return true;
                        }
                        if (browserActive &&
                            (key == VK_DOWN || key == 'S')) {
                            browserNavigationRequested.fetch_add(
                                1,
                                std::memory_order_release);
                            return true;
                        }
                        if (browserActive &&
                            (key == VK_LEFT || key == 'A')) {
                            browserNavigationRequested.fetch_sub(
                                10,
                                std::memory_order_release);
                            return true;
                        }
                        if (browserActive &&
                            (key == VK_RIGHT || key == 'D')) {
                            browserNavigationRequested.fetch_add(
                                10,
                                std::memory_order_release);
                            return true;
                        }
                        if (browserActive &&
                            (key == VK_RETURN || key == VK_SPACE)) {
                            browserAcceptRequested.store(
                                browserSelection.load(
                                    std::memory_order_acquire),
                                std::memory_order_release);
                            return true;
                        }
                        // Fallout's raw event is always consumed while MMVP
                        // owns input. Selected legacy WM_KEYDOWN messages are
                        // independently allowed to reach Scaleform above.
                        // Forwarding both paths made every arrow move twice
                        // and let raw Tab close the entire Pip-Boy.
                        return true;
                    }
                    if (raw->header.dwType == RIM_TYPEMOUSE) {
                        if (browserActive &&
                            (raw->data.mouse.usButtonFlags &
                             RI_MOUSE_WHEEL) != 0) {
                            const auto wheelDelta = static_cast<SHORT>(
                                raw->data.mouse.usButtonData);
                            browserNavigationRequested.fetch_add(
                                wheelDelta > 0 ? -1 : 1,
                                std::memory_order_release);
                            return true;
                        }
                        if (browserActive &&
                            (raw->data.mouse.usButtonFlags &
                             RI_MOUSE_LEFT_BUTTON_DOWN) != 0) {
                            // Fallout/Proton does not deliver either the
                            // browser's complete Flash CLICK sequence while
                            // raw input is owned. Prefer the SWF's freshly
                            // reported exact hover selection; resolve the live
                            // MenuCursor only when no fresh report exists.
                            ExecuteBrowserInputClick();
                            return true;
                        }
                        if (playerActive &&
                            (raw->data.mouse.usButtonFlags &
                             RI_MOUSE_LEFT_BUTTON_DOWN) != 0) {
                            if (GetTickCount64() -
                                    browserLastClickTick.load(
                                        std::memory_order_acquire) <
                                200) {
                                return true;
                            }
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
        if (browserActive && message == WM_MOUSEWHEEL) {
            // Raw mouse-wheel input above owns navigation; suppress its
            // legacy duplicate so one detent moves exactly one row.
            return true;
        }
        if (browserActive && message == WM_LBUTTONDOWN) {
            ExecuteBrowserInputClick();
            return true;
        }
        if (playerActive && message == WM_LBUTTONDOWN) {
            if (GetTickCount64() -
                    browserLastClickTick.load(std::memory_order_acquire) <
                200) {
                return true;
            }
            ExecuteClick();
            // The video controls sit over the live Pip-Boy menu. Do not let
            // this click activate whichever vanilla control is underneath.
            return true;
        }
        if (message == WM_CHAR || message == WM_SYSCHAR) {
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
            return;
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
        auto source = player ? player->LatestFrame() : nullptr;
        if (source && source->channel != channel) {
            spdlog::error(
                "Rejected a {} frame while composing the {} Pip-Boy channel",
                PlaybackChannelName(source->channel),
                PlaybackChannelName(channel));
            source.reset();
        }
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
        std::scoped_lock composedLock(composedMutex);
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
