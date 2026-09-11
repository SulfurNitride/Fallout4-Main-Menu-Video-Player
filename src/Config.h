#pragma once

namespace Config
{
    void Load(HMODULE module);
    [[nodiscard]] bool EnableNativeMainMenuBink() noexcept;
    [[nodiscard]] bool KeepPlayingWhenBorderless() noexcept;
    [[nodiscard]] bool MuteVanillaMenuMusic() noexcept;
    [[nodiscard]] bool RecursiveMediaScan() noexcept;
    [[nodiscard]] std::filesystem::path MainMenuDirectory();
    [[nodiscard]] std::filesystem::path MainMenuAudioDirectory();
    [[nodiscard]] std::uint32_t MainMenuHelpMilliseconds() noexcept;
    [[nodiscard]] std::uint32_t MainMenuStopKey() noexcept;
    [[nodiscard]] std::uint32_t MainMenuNextKey() noexcept;
    [[nodiscard]] std::uint32_t MainMenuVolumeUpKey() noexcept;
    [[nodiscard]] std::uint32_t MainMenuVolumeDownKey() noexcept;
    [[nodiscard]] std::uint32_t MainMenuNextAudioKey() noexcept;
    [[nodiscard]] std::uint32_t MainMenuToggleOriginalAudioKey() noexcept;
    [[nodiscard]] float MainMenuVolume() noexcept;
    [[nodiscard]] float MainMenuVolumeStep() noexcept;
}
