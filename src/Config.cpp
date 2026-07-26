#include "PCH.h"

#include "Config.h"

namespace
{
    bool enableNativeMainMenuBink{ true };
    bool keepPlayingWhenBorderless{ true };
    bool muteVanillaMenuMusic{ true };

    bool ReadBoolean(
        const std::filesystem::path& path,
        const wchar_t* key,
        const bool defaultValue)
    {
        return GetPrivateProfileIntW(
                   L"Playback",
                   key,
                   defaultValue ? 1 : 0,
                   path.c_str()) != 0;
    }
}

void Config::Load(const HMODULE module)
{
    std::array<wchar_t, 32768> modulePath{};
    const DWORD length = GetModuleFileNameW(
        module,
        modulePath.data(),
        static_cast<DWORD>(modulePath.size()));
    std::filesystem::path path{
        "Data/F4SE/Plugins/MainMenuVideoPlayer.ini"
    };
    if (length > 0 && length < modulePath.size()) {
        path = std::filesystem::path{
            std::wstring_view(modulePath.data(), length)
        }.parent_path() / L"MainMenuVideoPlayer.ini";
    }

    enableNativeMainMenuBink = ReadBoolean(
        path,
        L"EnableNativeMainMenuBink",
        true);
    keepPlayingWhenBorderless = ReadBoolean(
        path,
        L"KeepPlayingWhenBorderless",
        true);
    muteVanillaMenuMusic = ReadBoolean(
        path,
        L"MuteVanillaMenuMusic",
        true);

    spdlog::info(
        "Configuration: EnableNativeMainMenuBink={}, "
        "MuteVanillaMenuMusic={}, KeepPlayingWhenBorderless={}",
        enableNativeMainMenuBink,
        muteVanillaMenuMusic,
        keepPlayingWhenBorderless);
}

bool Config::EnableNativeMainMenuBink() noexcept
{
    return enableNativeMainMenuBink;
}

bool Config::KeepPlayingWhenBorderless() noexcept
{
    return keepPlayingWhenBorderless;
}

bool Config::MuteVanillaMenuMusic() noexcept
{
    return muteVanillaMenuMusic;
}
