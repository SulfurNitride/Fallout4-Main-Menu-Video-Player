#pragma once

namespace WorldTextureBridge
{
    void RequestInstall();
    void Shutdown();
    [[nodiscard]] bool TelevisionTextureCaptured() noexcept;
    [[nodiscard]] bool ProjectorTextureCaptured() noexcept;
}
