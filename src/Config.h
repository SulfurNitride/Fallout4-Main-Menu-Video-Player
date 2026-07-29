#pragma once

namespace Config
{
    void Load(HMODULE module);
    [[nodiscard]] bool EnableNativeMainMenuBink() noexcept;
    [[nodiscard]] bool KeepPlayingWhenBorderless() noexcept;
    [[nodiscard]] bool MuteVanillaMenuMusic() noexcept;
    [[nodiscard]] bool RecursiveMediaScan() noexcept;
    [[nodiscard]] bool EnableWorldScreens() noexcept;
    [[nodiscard]] bool EnablePipBoyPlayer() noexcept;
    [[nodiscard]] std::filesystem::path MainMenuDirectory();
    [[nodiscard]] std::filesystem::path TelevisionDirectory();
    [[nodiscard]] std::filesystem::path MovieDirectory();
    [[nodiscard]] std::uint32_t TelevisionWidth() noexcept;
    [[nodiscard]] std::uint32_t TelevisionHeight() noexcept;
    [[nodiscard]] std::uint32_t ProjectorWidth() noexcept;
    [[nodiscard]] std::uint32_t ProjectorHeight() noexcept;
    [[nodiscard]] std::uint32_t PipBoyWidth() noexcept;
    [[nodiscard]] std::uint32_t PipBoyHeight() noexcept;
    [[nodiscard]] std::uint32_t PipBoyOverlayTimeoutMilliseconds() noexcept;
    [[nodiscard]] std::uint32_t MainMenuHelpMilliseconds() noexcept;
    [[nodiscard]] std::uint32_t MainMenuStopKey() noexcept;
    [[nodiscard]] std::uint32_t MainMenuNextKey() noexcept;
    [[nodiscard]] std::uint32_t MainMenuVolumeUpKey() noexcept;
    [[nodiscard]] std::uint32_t MainMenuVolumeDownKey() noexcept;
    [[nodiscard]] float MainMenuVolume() noexcept;
    [[nodiscard]] float MainMenuVolumeStep() noexcept;
    [[nodiscard]] float TelevisionVolume() noexcept;
    [[nodiscard]] float MovieVolume() noexcept;
}
