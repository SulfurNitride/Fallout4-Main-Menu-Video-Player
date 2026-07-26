#pragma once

namespace Config
{
    void Load(HMODULE module);
    [[nodiscard]] bool EnableNativeMainMenuBink() noexcept;
    [[nodiscard]] bool KeepPlayingWhenBorderless() noexcept;
    [[nodiscard]] bool MuteVanillaMenuMusic() noexcept;
}
