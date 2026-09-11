#include "PCH.h"

#include "Config.h"
#include "EngineSettings.h"
#include "PlaybackGate.h"

namespace PlaybackGate
{
    namespace
    {
        HWND FindGameWindow()
        {
            struct Search
            {
                DWORD processId;
                HWND window;
            } search{ GetCurrentProcessId(), nullptr };

            EnumWindows(
                [](const HWND window, const LPARAM parameter) -> BOOL {
                    auto& candidate = *reinterpret_cast<Search*>(parameter);
                    DWORD processId = 0;
                    GetWindowThreadProcessId(window, &processId);
                    if (processId == candidate.processId &&
                        IsWindowVisible(window) &&
                        GetWindow(window, GW_OWNER) == nullptr) {
                        RECT rect{};
                        RECT previous{};
                        GetWindowRect(window, &rect);
                        GetWindowRect(candidate.window, &previous);
                        const auto area =
                            static_cast<std::int64_t>(rect.right - rect.left) *
                            (rect.bottom - rect.top);
                        const auto previousArea =
                            static_cast<std::int64_t>(
                                previous.right - previous.left) *
                            (previous.bottom - previous.top);
                        if (area > previousArea) {
                            candidate.window = window;
                        }
                    }
                    return TRUE;
                },
                reinterpret_cast<LPARAM>(&search));
            return search.window;
        }

        HWND GameWindow()
        {
            static std::atomic<HWND> cachedWindow{ nullptr };
            HWND window = cachedWindow.load(std::memory_order_acquire);
            if (!window || !IsWindow(window)) {
                window = FindGameWindow();
                cachedWindow.store(window, std::memory_order_release);
            }
            return window;
        }

        bool IsBorderlessFullscreen(const HWND window)
        {
            if (!window || IsIconic(window)) {
                return false;
            }

            RECT windowRect{};
            MONITORINFO monitorInfo{};
            monitorInfo.cbSize = sizeof(MONITORINFO);
            const HMONITOR monitor =
                MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
            if (!GetWindowRect(window, &windowRect) ||
                !GetMonitorInfoW(monitor, &monitorInfo)) {
                return false;
            }

            constexpr LONG kEdgeTolerance{ 2 };
            return windowRect.left <=
                       monitorInfo.rcMonitor.left + kEdgeTolerance &&
                   windowRect.top <=
                       monitorInfo.rcMonitor.top + kEdgeTolerance &&
                   windowRect.right >=
                       monitorInfo.rcMonitor.right - kEdgeTolerance &&
                   windowRect.bottom >=
                       monitorInfo.rcMonitor.bottom - kEdgeTolerance;
        }
    } // namespace

    bool MayAdvance()
    {
        enum class State
        {
            kForeground,
            kBorderlessBackground,
            kPaused
        };
        static std::atomic<State> previousState{ State::kForeground };

        const HWND gameWindow = GameWindow();
        const bool falloutMinimized = gameWindow && IsIconic(gameWindow);
        const HWND foreground = GetForegroundWindow();
        if (foreground) {
            DWORD processId = 0;
            GetWindowThreadProcessId(foreground, &processId);
            if (processId == GetCurrentProcessId() && !falloutMinimized) {
                const State previous = previousState.exchange(
                    State::kForeground, std::memory_order_relaxed);
                if (previous != State::kForeground) {
                    spdlog::info("Fallout regained focus; playback active");
                }
                return true;
            }
        }

        const bool keepPlaying = Config::KeepPlayingWhenBorderless() &&
                                 !falloutMinimized &&
                                 (EngineSettings::IsBorderlessMode() ||
                                     IsBorderlessFullscreen(gameWindow));
        const State state =
            keepPlaying ? State::kBorderlessBackground : State::kPaused;
        const State previous =
            previousState.exchange(state, std::memory_order_relaxed);
        if (previous != state) {
            if (keepPlaying) {
                spdlog::info("Fallout lost focus in borderless mode; "
                             "playback remains active");
            } else if (falloutMinimized) {
                spdlog::info("Fallout is minimized; main-menu playback paused");
            } else {
                spdlog::info("Fallout lost focus outside borderless mode; "
                             "playback paused");
            }
        }
        return keepPlaying;
    }
} // namespace PlaybackGate
