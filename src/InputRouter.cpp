#include "PCH.h"

#include "BinkHook.h"
#include "InputRouter.h"
#include "PipBoyPlayer.h"

namespace InputRouter
{
    namespace
    {
        std::mutex hookMutex;
        HWND falloutWindow{ nullptr };
        WNDPROC originalWindowProcedure{ nullptr };
        bool rawInputCaptured{ false };

        BOOL CALLBACK FindProcessWindow(
            const HWND window,
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
            const auto candidateArea = static_cast<std::int64_t>(
                candidateRect.right - candidateRect.left) *
                (candidateRect.bottom - candidateRect.top);
            const auto bestArea = static_cast<std::int64_t>(
                bestRect.right - bestRect.left) *
                (bestRect.bottom - bestRect.top);
            if (candidateArea > bestArea) {
                best = window;
            }
            return TRUE;
        }

        LRESULT CALLBACK RoutedWindowProcedure(
            const HWND window,
            const UINT message,
            const WPARAM wParam,
            const LPARAM lParam)
        {
            if (PipBoyPlayer::HandleWindowMessage(
                    message,
                    wParam,
                    lParam)) {
                if (message == WM_INPUT) {
                    DefWindowProcW(window, message, wParam, lParam);
                }
                return 0;
            }
            if (BinkHook::HandleWindowMessage(
                    message,
                    wParam,
                    lParam)) {
                return 0;
            }

            return originalWindowProcedure ?
                CallWindowProcW(
                    originalWindowProcedure,
                    window,
                    message,
                    wParam,
                    lParam) :
                DefWindowProcW(window, message, wParam, lParam);
        }
    }

    bool Install()
    {
        std::scoped_lock lock(hookMutex);
        if (originalWindowProcedure && IsWindow(falloutWindow)) {
            return true;
        }

        falloutWindow = FindWindowW(L"Fallout4", nullptr);
        if (!falloutWindow) {
            EnumWindows(
                FindProcessWindow,
                reinterpret_cast<LPARAM>(&falloutWindow));
        }
        if (!falloutWindow) {
            spdlog::warn(
                "Could not find the Fallout 4 window for MMVP input");
            return false;
        }

        SetLastError(ERROR_SUCCESS);
        originalWindowProcedure = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(
                falloutWindow,
                GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(&RoutedWindowProcedure)));
        if (!originalWindowProcedure && GetLastError() != ERROR_SUCCESS) {
            spdlog::warn(
                "Could not subclass the Fallout 4 window for MMVP input: {}",
                GetLastError());
            falloutWindow = nullptr;
            return false;
        }

        spdlog::info("Installed shared main-menu/Pip-Boy input router");
        rawInputCaptured = false;
        return true;
    }

    bool SetRawInputCapture(const bool enabled)
    {
        if (enabled && !Install()) {
            return false;
        }

        std::scoped_lock lock(hookMutex);
        if (!IsWindow(falloutWindow)) {
            rawInputCaptured = false;
            return !enabled;
        }
        if (rawInputCaptured == enabled) {
            return true;
        }

        RAWINPUTDEVICE devices[2]{};
        devices[0].usUsagePage = 0x01;
        devices[0].usUsage = 0x06;
        devices[0].dwFlags = enabled ? RIDEV_INPUTSINK : 0;
        devices[0].hwndTarget = falloutWindow;
        devices[1].usUsagePage = 0x01;
        devices[1].usUsage = 0x02;
        devices[1].dwFlags = enabled ? RIDEV_INPUTSINK : 0;
        devices[1].hwndTarget = falloutWindow;

        if (!RegisterRawInputDevices(
                devices,
                static_cast<UINT>(std::size(devices)),
                sizeof(RAWINPUTDEVICE))) {
            spdlog::error(
                "Could not {} MMVP raw keyboard/mouse capture: {}",
                enabled ? "register" : "restore",
                GetLastError());
            return false;
        }

        rawInputCaptured = enabled;
        if (enabled) {
            spdlog::info(
                "Registered Holo-Wind-style raw keyboard/mouse input sink");
        } else {
            spdlog::info(
                "Restored Fallout raw keyboard/mouse input registration");
        }
        return true;
    }

    HWND Window() noexcept
    {
        return falloutWindow;
    }
}
