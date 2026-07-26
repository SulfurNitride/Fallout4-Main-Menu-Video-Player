#include "PCH.h"

#include "BinkHook.h"
#include "EngineSettings.h"
#include "VideoPlayer.h"

#include <MinHook.h>

namespace BinkHook
{
    namespace
    {
        using BinkOpen = void* (__stdcall*)(const char*, std::uint32_t);
        using BinkClose = void (__stdcall*)(void*);
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
        BinkCopyToBufferRect originalCopy{ nullptr };
        std::array<void*, 3> hookTargets{};

        std::atomic<void*> mainMenuBink{ nullptr };
        std::atomic<std::uint32_t> mainMenuWidth{ 0 };
        std::atomic<std::uint32_t> mainMenuHeight{ 0 };
        std::atomic<std::uint32_t> loggedCopyCalls{ 0 };
        std::atomic<bool> loggedUnsupportedSurface{ false };

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

            mainMenuWidth.store(width, std::memory_order_release);
            mainMenuHeight.store(height, std::memory_order_release);
            mainMenuBink.store(handle, std::memory_order_release);
            loggedCopyCalls.store(0, std::memory_order_relaxed);
            loggedUnsupportedSurface.store(false, std::memory_order_relaxed);
            VideoPlayer::GetSingleton().OnNativeVideoOpened(width, height);
            spdlog::info(
                "Captured native main-menu Bink handle {} "
                "({}x{}, flags {:08X})",
                handle,
                width,
                height,
                flags);
            return handle;
        }

        void __stdcall HookedClose(void* handle)
        {
            void* expected = handle;
            if (mainMenuBink.compare_exchange_strong(
                    expected,
                    nullptr,
                    std::memory_order_acq_rel)) {
                mainMenuWidth.store(0, std::memory_order_release);
                mainMenuHeight.store(0, std::memory_order_release);
                VideoPlayer::GetSingleton().OnNativeVideoClosed();
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
            const std::int32_t result = originalCopy(
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

            if (handle != mainMenuBink.load(std::memory_order_acquire)) {
                return result;
            }

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

            if (const auto frame =
                    VideoPlayer::GetSingleton().GetLatestFrame()) {
                CopyVideoPixels(
                    *frame,
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
            } else {
                static const VideoFrame blackFrame{
                    .pixels = { 0, 0, 0, 255 },
                    .width = 1,
                    .height = 1,
                    .rowPitch = 4,
                    .serial = 0
                };
                CopyVideoPixels(
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
                hookTargets[2])) {
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
            "Installed native Bink open/close/copy hooks from {}",
            reinterpret_cast<void*>(bink));
        return true;
    }
}
