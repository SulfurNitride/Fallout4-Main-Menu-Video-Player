#pragma once

#include <cstdint>

namespace InputRouter
{
    struct ClientSizeSnapshot
    {
        // available remains true for a successfully queried 0x0 minimized
        // client area; callers should check width and height before use.
        std::uint32_t width{ 0 };
        std::uint32_t height{ 0 };
        bool available{ false };
    };

    bool Install();
    [[nodiscard]] ClientSizeSnapshot GetClientSize() noexcept;
}
