#pragma once

namespace InputRouter
{
    bool Install();
    bool SetRawInputCapture(bool enabled);
    [[nodiscard]] HWND Window() noexcept;
}
