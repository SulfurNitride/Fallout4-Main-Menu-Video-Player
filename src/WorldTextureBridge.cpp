#include "PCH.h"

#include "Config.h"
#include "PipBoyPlayer.h"
#include "WorldPlayback.h"
#include "WorldTextureBridge.h"

#include <MinHook.h>
#include <d3d11.h>
#include <dxgi.h>

namespace WorldTextureBridge
{
    namespace
    {
        using CreateTexture2D = HRESULT (STDMETHODCALLTYPE*)(
            ID3D11Device*,
            const D3D11_TEXTURE2D_DESC*,
            const D3D11_SUBRESOURCE_DATA*,
            ID3D11Texture2D**);
        using Present = HRESULT (STDMETHODCALLTYPE*)(
            IDXGISwapChain*,
            UINT,
            UINT);
        using OMSetRenderTargets = void (STDMETHODCALLTYPE*)(
            ID3D11DeviceContext*,
            UINT,
            ID3D11RenderTargetView* const*,
            ID3D11DepthStencilView*);

        constexpr std::array<std::uint8_t, 16> kTelevisionMarker{
            'M', 'M', 'V', 'P', 'T', 'V', '0', '1',
            0x17, 0x42, 0xA5, 0xE1, 0x4D, 0x4D, 0x56, 0x50
        };
        constexpr std::array<std::uint8_t, 16> kProjectorMarker{
            'M', 'M', 'V', 'P', 'M', 'O', 'V', 'I',
            0x32, 0x88, 0xC4, 0x7F, 0x50, 0x56, 0x4D, 0x4D
        };

        struct CapturedTexture
        {
            ID3D11Texture2D* texture{ nullptr };
            std::uint64_t uploadedSerial{ 0 };
        };

        CreateTexture2D originalCreateTexture2D{ nullptr };
        Present originalPresent{ nullptr };
        OMSetRenderTargets originalOMSetRenderTargets{ nullptr };
        void* createTextureTarget{ nullptr };
        void* presentTarget{ nullptr };
        void* omSetRenderTargetsTarget{ nullptr };
        std::mutex textureMutex;
        std::mutex pipBoyMutex;
        CapturedTexture televisionTexture;
        CapturedTexture projectorTexture;
        ID3D11Texture2D* pipBoyTexture{ nullptr };
        bool pipBoyBound{ false };
        std::atomic<bool> televisionCaptured{ false };
        std::atomic<bool> projectorCaptured{ false };
        std::atomic<bool> installationRequested{ false };
        std::atomic<bool> installationComplete{ false };
        std::atomic<bool> pipBoyCaptureLogged{ false };

        bool MatchesMarker(
            const D3D11_TEXTURE2D_DESC* descriptor,
            const D3D11_SUBRESOURCE_DATA* data,
            const std::array<std::uint8_t, 16>& marker,
            const std::uint32_t width,
            const std::uint32_t height)
        {
            if (!descriptor || !data || !data[0].pSysMem ||
                descriptor->Width != width ||
                descriptor->Height != height ||
                descriptor->ArraySize != 1 ||
                descriptor->SampleDesc.Count != 1 ||
                data[0].SysMemPitch < marker.size()) {
                return false;
            }
            return std::memcmp(
                       data[0].pSysMem,
                       marker.data(),
                       marker.size()) == 0;
        }

        void ReplaceCapturedTexture(
            CapturedTexture& destination,
            ID3D11Texture2D* texture)
        {
            if (texture) {
                texture->AddRef();
            }
            if (destination.texture) {
                destination.texture->Release();
            }
            destination.texture = texture;
            destination.uploadedSerial = 0;
        }

        HRESULT STDMETHODCALLTYPE HookedCreateTexture2D(
            ID3D11Device* device,
            const D3D11_TEXTURE2D_DESC* descriptor,
            const D3D11_SUBRESOURCE_DATA* initialData,
            ID3D11Texture2D** texture)
        {
            const bool worldScreensEnabled =
                Config::EnableWorldScreens();
            const bool television = worldScreensEnabled && MatchesMarker(
                descriptor,
                initialData,
                kTelevisionMarker,
                Config::TelevisionWidth(),
                Config::TelevisionHeight());
            const bool projector = worldScreensEnabled && MatchesMarker(
                descriptor,
                initialData,
                kProjectorMarker,
                Config::ProjectorWidth(),
                Config::ProjectorHeight());

            D3D11_TEXTURE2D_DESC mutableDescriptor{};
            const D3D11_TEXTURE2D_DESC* effectiveDescriptor = descriptor;
            if (television || projector) {
                mutableDescriptor = *descriptor;
                mutableDescriptor.MipLevels = 1;
                mutableDescriptor.ArraySize = 1;
                mutableDescriptor.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                mutableDescriptor.SampleDesc.Count = 1;
                mutableDescriptor.SampleDesc.Quality = 0;
                mutableDescriptor.Usage = D3D11_USAGE_DEFAULT;
                mutableDescriptor.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
                mutableDescriptor.CPUAccessFlags = 0;
                mutableDescriptor.MiscFlags = 0;
                effectiveDescriptor = &mutableDescriptor;
            }

            const HRESULT result = originalCreateTexture2D(
                device,
                effectiveDescriptor,
                initialData,
                texture);
            if (FAILED(result) || !texture || !*texture) {
                return result;
            }

            if (television || projector) {
                std::scoped_lock lock(textureMutex);
                if (television) {
                    ReplaceCapturedTexture(televisionTexture, *texture);
                    televisionCaptured.store(true, std::memory_order_release);
                    if (auto* session =
                            WorldPlayback::GetSingleton().Television()) {
                        session->SetConsumers(1);
                    }
                    spdlog::info(
                        "Captured MMVP television texture {} ({}x{})",
                        static_cast<void*>(*texture),
                        effectiveDescriptor->Width,
                        effectiveDescriptor->Height);
                } else {
                    ReplaceCapturedTexture(projectorTexture, *texture);
                    projectorCaptured.store(true, std::memory_order_release);
                    if (auto* session =
                            WorldPlayback::GetSingleton().Projector()) {
                        session->SetConsumers(
                            1U + (PipBoyPlayer::Active() ? 1U : 0U));
                    }
                    spdlog::info(
                        "Captured MMVP projector texture {} ({}x{})",
                        static_cast<void*>(*texture),
                        effectiveDescriptor->Width,
                        effectiveDescriptor->Height);
                }
            }
            return result;
        }

        void ReplacePipBoyTexture(ID3D11Texture2D* texture)
        {
            if (texture) {
                texture->AddRef();
            }
            if (pipBoyTexture) {
                pipBoyTexture->Release();
            }
            pipBoyTexture = texture;
        }

        bool IsPipBoyTarget(
            ID3D11RenderTargetView* view,
            ID3D11Texture2D** texture)
        {
            if (!view || !texture) {
                return false;
            }
            *texture = nullptr;
            ID3D11Resource* resource = nullptr;
            view->GetResource(&resource);
            if (!resource) {
                return false;
            }
            const HRESULT query = resource->QueryInterface(
                __uuidof(ID3D11Texture2D),
                reinterpret_cast<void**>(texture));
            resource->Release();
            if (FAILED(query) || !*texture) {
                return false;
            }
            D3D11_TEXTURE2D_DESC descriptor{};
            (*texture)->GetDesc(&descriptor);
            return descriptor.Width == Config::PipBoyWidth() &&
                   descriptor.Height == Config::PipBoyHeight() &&
                   descriptor.MipLevels == 1 &&
                   (descriptor.BindFlags & D3D11_BIND_RENDER_TARGET) != 0;
        }

        void STDMETHODCALLTYPE HookedOMSetRenderTargets(
            ID3D11DeviceContext* context,
            const UINT viewCount,
            ID3D11RenderTargetView* const* views,
            ID3D11DepthStencilView* depthStencil)
        {
            const bool wasPipBoyBound = pipBoyBound;
            bool willBindPipBoy = false;
            ID3D11Texture2D* detectedTexture = nullptr;
            if ((PipBoyPlayer::Active() ||
                 PipBoyPlayer::CommandDetectionReady()) &&
                views) {
                for (UINT index = 0; index < viewCount; ++index) {
                    if (IsPipBoyTarget(views[index], &detectedTexture)) {
                        PipBoyPlayer::PollScaleformCommand();
                        willBindPipBoy = PipBoyPlayer::Active();
                        if (!willBindPipBoy) {
                            detectedTexture->Release();
                            detectedTexture = nullptr;
                        }
                        break;
                    }
                    if (detectedTexture) {
                        detectedTexture->Release();
                        detectedTexture = nullptr;
                    }
                }
            }

            if (detectedTexture) {
                std::scoped_lock lock(pipBoyMutex);
                if (detectedTexture != pipBoyTexture) {
                    ReplacePipBoyTexture(detectedTexture);
                    if (!pipBoyCaptureLogged.exchange(
                            true,
                            std::memory_order_relaxed)) {
                        spdlog::info(
                            "Captured the MMVP Pip-Boy render target {} "
                            "({}x{})",
                            static_cast<void*>(detectedTexture),
                            Config::PipBoyWidth(),
                            Config::PipBoyHeight());
                    }
                }
                detectedTexture->Release();
            }

            originalOMSetRenderTargets(
                context,
                viewCount,
                views,
                depthStencil);
            pipBoyBound = willBindPipBoy;

            if (wasPipBoyBound && !willBindPipBoy &&
                PipBoyPlayer::Active()) {
                ID3D11Texture2D* target = nullptr;
                {
                    std::scoped_lock lock(pipBoyMutex);
                    target = pipBoyTexture;
                    if (target) {
                        target->AddRef();
                    }
                }
                if (target) {
                    PipBoyPlayer::UploadFrame(context, target);
                    target->Release();
                }
            }
        }

        void UploadFrame(
            CapturedTexture& captured,
            const std::shared_ptr<const VideoFrame>& frame)
        {
            if (!captured.texture ||
                !frame ||
                frame->serial == captured.uploadedSerial ||
                frame->pixels.empty()) {
                return;
            }

            D3D11_TEXTURE2D_DESC descriptor{};
            captured.texture->GetDesc(&descriptor);
            if (descriptor.Width != frame->width ||
                descriptor.Height != frame->height ||
                descriptor.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
                spdlog::error(
                    "Dynamic texture dimensions or format no longer match "
                    "the decoded frame");
                return;
            }

            ID3D11Device* device = nullptr;
            captured.texture->GetDevice(&device);
            if (!device) {
                return;
            }
            ID3D11DeviceContext* context = nullptr;
            device->GetImmediateContext(&context);
            device->Release();
            if (!context) {
                return;
            }
            context->UpdateSubresource(
                captured.texture,
                0,
                nullptr,
                frame->pixels.data(),
                frame->rowPitch,
                0);
            context->Release();
            captured.uploadedSerial = frame->serial;
        }

        HRESULT STDMETHODCALLTYPE HookedPresent(
            IDXGISwapChain* swapChain,
            const UINT syncInterval,
            const UINT flags)
        {
            PipBoyPlayer::TickPointer();
            if (!PipBoyPlayer::Active()) {
                std::scoped_lock lock(pipBoyMutex);
                pipBoyBound = false;
                ReplacePipBoyTexture(nullptr);
                pipBoyCaptureLogged.store(
                    false,
                    std::memory_order_relaxed);
            }
            {
                std::scoped_lock lock(textureMutex);
                auto& playback = WorldPlayback::GetSingleton();
                UploadFrame(
                    televisionTexture,
                    playback.Frame(PlaybackChannel::kTelevision));
                UploadFrame(
                    projectorTexture,
                    playback.Frame(PlaybackChannel::kProjector));
            }
            return originalPresent(swapChain, syncInterval, flags);
        }

        LRESULT CALLBACK DummyWindowProcedure(
            const HWND window,
            const UINT message,
            const WPARAM wParam,
            const LPARAM lParam)
        {
            return DefWindowProcW(window, message, wParam, lParam);
        }

        bool ResolveHookTargets()
        {
            constexpr wchar_t kClassName[]{
                L"MMVP_D3D11_Discovery_Window"
            };
            WNDCLASSEXW windowClass{};
            windowClass.cbSize = sizeof(WNDCLASSEXW);
            windowClass.lpfnWndProc = DummyWindowProcedure;
            windowClass.hInstance = GetModuleHandleW(nullptr);
            windowClass.lpszClassName = kClassName;
            const ATOM classAtom = RegisterClassExW(&windowClass);
            if (!classAtom &&
                GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
                spdlog::error(
                    "Could not register the D3D11 discovery window");
                return false;
            }

            HWND window = CreateWindowExW(
                0,
                kClassName,
                L"",
                WS_OVERLAPPEDWINDOW,
                0,
                0,
                64,
                64,
                nullptr,
                nullptr,
                windowClass.hInstance,
                nullptr);
            if (!window) {
                spdlog::error(
                    "Could not create the D3D11 discovery window");
                return false;
            }

            DXGI_SWAP_CHAIN_DESC swapChainDescription{};
            swapChainDescription.BufferDesc.Width = 64;
            swapChainDescription.BufferDesc.Height = 64;
            swapChainDescription.BufferDesc.Format =
                DXGI_FORMAT_R8G8B8A8_UNORM;
            swapChainDescription.SampleDesc.Count = 1;
            swapChainDescription.BufferUsage =
                DXGI_USAGE_RENDER_TARGET_OUTPUT;
            swapChainDescription.BufferCount = 1;
            swapChainDescription.OutputWindow = window;
            swapChainDescription.Windowed = TRUE;
            swapChainDescription.SwapEffect =
                DXGI_SWAP_EFFECT_DISCARD;

            IDXGISwapChain* swapChain = nullptr;
            ID3D11Device* device = nullptr;
            ID3D11DeviceContext* context = nullptr;
            D3D_FEATURE_LEVEL featureLevel{};
            constexpr std::array featureLevels{
                D3D_FEATURE_LEVEL_11_0,
                D3D_FEATURE_LEVEL_10_1,
                D3D_FEATURE_LEVEL_10_0
            };
            HRESULT result = D3D11CreateDeviceAndSwapChain(
                nullptr,
                D3D_DRIVER_TYPE_HARDWARE,
                nullptr,
                0,
                featureLevels.data(),
                static_cast<UINT>(featureLevels.size()),
                D3D11_SDK_VERSION,
                &swapChainDescription,
                &swapChain,
                &device,
                &featureLevel,
                &context);
            if (FAILED(result)) {
                result = D3D11CreateDeviceAndSwapChain(
                    nullptr,
                    D3D_DRIVER_TYPE_WARP,
                    nullptr,
                    0,
                    featureLevels.data(),
                    static_cast<UINT>(featureLevels.size()),
                    D3D11_SDK_VERSION,
                    &swapChainDescription,
                    &swapChain,
                    &device,
                    &featureLevel,
                    &context);
            }

            if (SUCCEEDED(result) && device && swapChain) {
                void** deviceTable =
                    *reinterpret_cast<void***>(device);
                void** swapChainTable =
                    *reinterpret_cast<void***>(swapChain);
                void** contextTable =
                    *reinterpret_cast<void***>(context);
                createTextureTarget = deviceTable[5];
                presentTarget = swapChainTable[8];
                omSetRenderTargetsTarget = contextTable[33];
            }

            if (context) {
                context->Release();
            }
            if (device) {
                device->Release();
            }
            if (swapChain) {
                swapChain->Release();
            }
            DestroyWindow(window);
            UnregisterClassW(kClassName, windowClass.hInstance);

            if (!createTextureTarget ||
                !presentTarget ||
                !omSetRenderTargetsTarget) {
                spdlog::error(
                    "Could not resolve D3D11 texture/Present hook targets: "
                    "{:08X}",
                    static_cast<std::uint32_t>(result));
                return false;
            }
            return true;
        }

        bool CreateAndEnableHook(
            void* target,
            void* hook,
            void** original,
            const char* name)
        {
            const MH_STATUS create = MH_CreateHook(
                target,
                hook,
                original);
            if (create != MH_OK) {
                spdlog::error(
                    "MH_CreateHook({}) failed: {}",
                    name,
                    MH_StatusToString(create));
                return false;
            }
            const MH_STATUS enable = MH_EnableHook(target);
            if (enable != MH_OK && enable != MH_ERROR_ENABLED) {
                spdlog::error(
                    "MH_EnableHook({}) failed: {}",
                    name,
                    MH_StatusToString(enable));
                return false;
            }
            return true;
        }
    }

    bool InstallNow()
    {
        if (!Config::EnableWorldScreens() &&
            !Config::EnablePipBoyPlayer()) {
            return true;
        }
        const MH_STATUS initialize = MH_Initialize();
        if (initialize != MH_OK &&
            initialize != MH_ERROR_ALREADY_INITIALIZED) {
            spdlog::error(
                "MH_Initialize for world textures failed: {}",
                MH_StatusToString(initialize));
            return false;
        }
        if (!ResolveHookTargets()) {
            return false;
        }
        if (!CreateAndEnableHook(
                createTextureTarget,
                reinterpret_cast<void*>(&HookedCreateTexture2D),
                reinterpret_cast<void**>(&originalCreateTexture2D),
                "ID3D11Device::CreateTexture2D") ||
            !CreateAndEnableHook(
                presentTarget,
                reinterpret_cast<void*>(&HookedPresent),
                reinterpret_cast<void**>(&originalPresent),
                "IDXGISwapChain::Present") ||
            !CreateAndEnableHook(
                omSetRenderTargetsTarget,
                reinterpret_cast<void*>(&HookedOMSetRenderTargets),
                reinterpret_cast<void**>(&originalOMSetRenderTargets),
                "ID3D11DeviceContext::OMSetRenderTargets")) {
            return false;
        }
        spdlog::info(
            "Installed television/projector and Pip-Boy D3D11 hooks");
        return true;
    }

    void RequestInstall()
    {
        if ((!Config::EnableWorldScreens() &&
             !Config::EnablePipBoyPlayer()) ||
            installationComplete.load(std::memory_order_acquire)) {
            return;
        }

        bool expected = false;
        if (!installationRequested.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel)) {
            return;
        }

        std::thread([] {
            spdlog::info(
                "Renderer is active; discovering world texture hook targets");
            if (InstallNow()) {
                installationComplete.store(true, std::memory_order_release);
            } else {
                spdlog::error(
                    "World texture hooks are unavailable; main-menu "
                    "playback will remain active");
            }
        }).detach();
    }

    void Shutdown()
    {
        std::scoped_lock lock(textureMutex);
        ReplaceCapturedTexture(televisionTexture, nullptr);
        ReplaceCapturedTexture(projectorTexture, nullptr);
        televisionCaptured.store(false, std::memory_order_release);
        projectorCaptured.store(false, std::memory_order_release);
        {
            std::scoped_lock pipBoyLock(pipBoyMutex);
            pipBoyBound = false;
            ReplacePipBoyTexture(nullptr);
        }
    }

    bool TelevisionTextureCaptured() noexcept
    {
        return televisionCaptured.load(std::memory_order_acquire);
    }

    bool ProjectorTextureCaptured() noexcept
    {
        return projectorCaptured.load(std::memory_order_acquire);
    }
}
