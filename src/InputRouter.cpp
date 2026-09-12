#include "PCH.h"

#include "BinkHook.h"
#include "InputRouter.h"

namespace InputRouter
{
    namespace
    {
        std::mutex hookMutex;
        std::mutex clientSizeMutex;
        HWND falloutWindow{ nullptr };
        std::atomic<WNDPROC> originalWindowProcedure{ nullptr };
        constexpr std::uint64_t kClientSizeAvailable{ 1ULL << 63U };
        std::atomic<std::uint64_t> clientSizeState{ 0 };
        std::atomic<bool> clientSizeQueryFailureLogged{ false };

        std::uint64_t EncodeClientSize(
            const std::uint32_t width,
            const std::uint32_t height) noexcept
        {
            // RECT coordinates are signed LONGs, so a valid client dimension
            // cannot use the high bit. Reserve that bit to distinguish an
            // unavailable/stale snapshot from a successfully observed 0x0
            // minimized client area.
            return kClientSizeAvailable |
                   (static_cast<std::uint64_t>(width) << 32U) | height;
        }

        ClientSizeSnapshot DecodeClientSize(
            const std::uint64_t state) noexcept
        {
            return { static_cast<std::uint32_t>((state >> 32U) & 0x7FFFFFFFU),
                static_cast<std::uint32_t>(state & 0xFFFFFFFFU),
                (state & kClientSizeAvailable) != 0 };
        }

        void ObserveClientSize(const HWND window, const std::string_view reason)
        {
            std::scoped_lock lock(clientSizeMutex);
            RECT clientRect{};
            if (!GetClientRect(window, &clientRect) ||
                clientRect.left > clientRect.right ||
                clientRect.top > clientRect.bottom) {
                clientSizeState.store(0, std::memory_order_release);
                if (!clientSizeQueryFailureLogged.exchange(
                        true, std::memory_order_acq_rel)) {
                    spdlog::warn(
                        "Could not query the Fallout window client size ({})",
                        reason);
                }
                return;
            }

            const auto width = static_cast<std::uint32_t>(
                clientRect.right - clientRect.left);
            const auto height = static_cast<std::uint32_t>(
                clientRect.bottom - clientRect.top);
            clientSizeQueryFailureLogged.store(
                false, std::memory_order_release);
            const auto previous = DecodeClientSize(
                clientSizeState.load(std::memory_order_acquire));
            if (!previous.available) {
                clientSizeState.store(
                    EncodeClientSize(width, height), std::memory_order_release);
                spdlog::info(
                    "Observed Fallout window client size: {}x{} ({})",
                    width,
                    height,
                    reason);
                return;
            }

            if (previous.width == width && previous.height == height) {
                return;
            }

            spdlog::info(
                "Fallout window client size changed: {}x{} -> {}x{} ({})",
                previous.width,
                previous.height,
                width,
                height,
                reason);
            clientSizeState.store(
                EncodeClientSize(width, height), std::memory_order_release);
        }

        bool IsCurrentProcessWindow(const HWND window)
        {
            if (!window || !IsWindow(window)) {
                return false;
            }

            DWORD processId = 0;
            return GetWindowThreadProcessId(window, &processId) != 0 &&
                   processId == GetCurrentProcessId();
        }

        BOOL CALLBACK FindProcessWindow(const HWND window,
            const LPARAM parameter)
        {
            DWORD processId = 0;
            GetWindowThreadProcessId(window, &processId);
            if (processId != GetCurrentProcessId() ||
                !IsWindowVisible(window) ||
                GetWindow(window, GW_OWNER) != nullptr) {
                return TRUE;
            }

            auto& best = *reinterpret_cast<HWND*>(parameter);
            RECT candidateRect{};
            RECT bestRect{};
            GetWindowRect(window, &candidateRect);
            if (best) {
                GetWindowRect(best, &bestRect);
            }
            const auto candidateArea =
                static_cast<std::int64_t>(
                    candidateRect.right - candidateRect.left) *
                (candidateRect.bottom - candidateRect.top);
            const auto bestArea =
                static_cast<std::int64_t>(bestRect.right - bestRect.left) *
                (bestRect.bottom - bestRect.top);
            if (candidateArea > bestArea) {
                best = window;
            }
            return TRUE;
        }

        LRESULT CALLBACK RoutedWindowProcedure(const HWND window,
            const UINT message,
            const WPARAM wParam,
            const LPARAM lParam)
        {
            if (BinkHook::HandleWindowMessage(message, wParam, lParam)) {
                return 0;
            }

            const WNDPROC original =
                originalWindowProcedure.load(std::memory_order_acquire);
            const LRESULT result =
                original
                    ? CallWindowProcW(original,
                          window,
                          message,
                          wParam,
                          lParam)
                    : DefWindowProcW(window, message, wParam, lParam);
            if (message == WM_SIZE) {
                ObserveClientSize(window, "WM_SIZE");
            } else if (message == WM_DISPLAYCHANGE) {
                ObserveClientSize(window, "WM_DISPLAYCHANGE");
            } else if (message == WM_NCDESTROY) {
                std::scoped_lock lock(clientSizeMutex);
                clientSizeState.store(0, std::memory_order_release);
                clientSizeQueryFailureLogged.store(
                    false, std::memory_order_release);
            }
            return result;
        }
    } // namespace

    bool Install()
    {
        std::scoped_lock lock(hookMutex);
        if (originalWindowProcedure.load(std::memory_order_acquire) &&
            IsCurrentProcessWindow(falloutWindow)) {
            return true;
        }
        if (originalWindowProcedure.load(std::memory_order_acquire)) {
            // Do not reuse a stale HWND if Windows has recycled it for a
            // window owned by another process.
            falloutWindow = nullptr;
            originalWindowProcedure.store(nullptr, std::memory_order_release);
            {
                std::scoped_lock clientSizeLock(clientSizeMutex);
                clientSizeState.store(0, std::memory_order_release);
                clientSizeQueryFailureLogged.store(
                    false, std::memory_order_release);
            }
        }

        falloutWindow = FindWindowW(L"Fallout4", nullptr);
        if (!IsCurrentProcessWindow(falloutWindow)) {
            falloutWindow = nullptr;
            EnumWindows(
                FindProcessWindow, reinterpret_cast<LPARAM>(&falloutWindow));
        }
        if (!IsCurrentProcessWindow(falloutWindow)) {
            spdlog::warn("Could not find the Fallout 4 window for MMVP input");
            falloutWindow = nullptr;
            return false;
        }

        SetLastError(ERROR_SUCCESS);
        const WNDPROC previousWindowProcedure =
            reinterpret_cast<WNDPROC>(SetWindowLongPtrW(falloutWindow,
                GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(&RoutedWindowProcedure)));
        if (!previousWindowProcedure && GetLastError() != ERROR_SUCCESS) {
            spdlog::warn(
                "Could not subclass the Fallout 4 window for MMVP input: {}",
                GetLastError());
            falloutWindow = nullptr;
            return false;
        }
        originalWindowProcedure.store(
            previousWindowProcedure, std::memory_order_release);

        ObserveClientSize(falloutWindow, "initial");
        spdlog::info("Installed main-menu input router");
        return true;
    }

    ClientSizeSnapshot GetClientSize() noexcept
    {
        return DecodeClientSize(
            clientSizeState.load(std::memory_order_acquire));
    }
} // namespace InputRouter
