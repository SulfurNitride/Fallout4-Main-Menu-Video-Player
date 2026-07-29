#include "PCH.h"

#include "Config.h"

namespace
{
    bool enableNativeMainMenuBink{ true };
    bool keepPlayingWhenBorderless{ true };
    bool muteVanillaMenuMusic{ true };
    bool recursiveMediaScan{ true };
    bool enableWorldScreens{ false };
    bool enablePipBoyPlayer{ true };
    std::filesystem::path mainMenuDirectory{
        "Data/MainMenuVideos"
    };
    std::filesystem::path televisionDirectory{
        "Data/TVVideos"
    };
    std::filesystem::path movieDirectory{
        "Data/MovieVideos"
    };
    std::uint32_t televisionWidth{ 1024 };
    std::uint32_t televisionHeight{ 576 };
    std::uint32_t projectorWidth{ 1920 };
    std::uint32_t projectorHeight{ 1080 };
    std::uint32_t pipBoyWidth{ 876 };
    std::uint32_t pipBoyHeight{ 700 };
    std::uint32_t pipBoyOverlayTimeoutMilliseconds{ 3000 };
    std::uint32_t mainMenuHelpMilliseconds{ 5000 };
    std::uint32_t mainMenuStopKey{ VK_BACK };
    std::uint32_t mainMenuNextKey{ VK_TAB };
    std::uint32_t mainMenuVolumeUpKey{ VK_PRIOR };
    std::uint32_t mainMenuVolumeDownKey{ VK_NEXT };
    float mainMenuVolume{ 1.0F };
    float mainMenuVolumeStep{ 0.1F };
    float televisionVolume{ 1.0F };
    float movieVolume{ 1.0F };

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

    std::uint32_t ReadUnsigned(
        const std::filesystem::path& path,
        const wchar_t* section,
        const wchar_t* key,
        const std::uint32_t defaultValue,
        const std::uint32_t minimum,
        const std::uint32_t maximum)
    {
        const auto value = GetPrivateProfileIntW(
            section,
            key,
            static_cast<int>(defaultValue),
            path.c_str());
        return std::clamp(
            value < 0 ? minimum : static_cast<std::uint32_t>(value),
            minimum,
            maximum);
    }

    float ReadFloat(
        const std::filesystem::path& path,
        const wchar_t* section,
        const wchar_t* key,
        const float defaultValue,
        const float minimum,
        const float maximum)
    {
        std::array<wchar_t, 64> buffer{};
        const std::wstring fallback = std::format(L"{:.3f}", defaultValue);
        GetPrivateProfileStringW(
            section,
            key,
            fallback.c_str(),
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            path.c_str());
        wchar_t* end = nullptr;
        const float value = std::wcstof(buffer.data(), &end);
        return end == buffer.data() ?
            defaultValue :
            std::clamp(value, minimum, maximum);
    }

    std::filesystem::path ReadPath(
        const std::filesystem::path& path,
        const wchar_t* key,
        const wchar_t* fallback)
    {
        std::array<wchar_t, 32768> buffer{};
        GetPrivateProfileStringW(
            L"Media",
            key,
            fallback,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            path.c_str());
        return std::filesystem::path(buffer.data()).lexically_normal();
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
    recursiveMediaScan = ReadBoolean(
        path,
        L"RecursiveMediaScan",
        true);
    enableWorldScreens = ReadBoolean(
        path,
        L"EnableWorldScreens",
        false);
    enablePipBoyPlayer = ReadBoolean(
        path,
        L"EnablePipBoyPlayer",
        true);
    mainMenuDirectory = ReadPath(
        path,
        L"MainMenuDirectory",
        L"Data\\MainMenuVideos");
    televisionDirectory = ReadPath(
        path,
        L"TelevisionDirectory",
        L"Data\\TVVideos");
    movieDirectory = ReadPath(
        path,
        L"MovieDirectory",
        L"Data\\MovieVideos");
    televisionWidth = ReadUnsigned(
        path, L"Video", L"TelevisionWidth", 1024, 320, 3840);
    televisionHeight = ReadUnsigned(
        path, L"Video", L"TelevisionHeight", 576, 180, 2160);
    projectorWidth = ReadUnsigned(
        path, L"Video", L"ProjectorWidth", 1920, 320, 3840);
    projectorHeight = ReadUnsigned(
        path, L"Video", L"ProjectorHeight", 1080, 180, 2160);
    pipBoyWidth = ReadUnsigned(
        path, L"PipBoy", L"TargetWidth", 876, 320, 4096);
    pipBoyHeight = ReadUnsigned(
        path, L"PipBoy", L"TargetHeight", 700, 240, 4096);
    pipBoyOverlayTimeoutMilliseconds = ReadUnsigned(
        path, L"PipBoy", L"OverlayTimeoutMilliseconds", 3000, 500, 30000);
    mainMenuHelpMilliseconds = ReadUnsigned(
        path, L"MainMenuControls", L"HelpMilliseconds", 5000, 0, 30000);
    mainMenuStopKey = ReadUnsigned(
        path, L"MainMenuControls", L"StopKey", VK_BACK, 0, 255);
    mainMenuNextKey = ReadUnsigned(
        path, L"MainMenuControls", L"NextKey", VK_TAB, 0, 255);
    mainMenuVolumeUpKey = ReadUnsigned(
        path, L"MainMenuControls", L"VolumeUpKey", VK_PRIOR, 0, 255);
    mainMenuVolumeDownKey = ReadUnsigned(
        path, L"MainMenuControls", L"VolumeDownKey", VK_NEXT, 0, 255);
    mainMenuVolume = ReadFloat(
        path, L"Audio", L"MainMenuVolume", 1.0F, 0.0F, 2.0F);
    mainMenuVolumeStep = ReadFloat(
        path, L"MainMenuControls", L"VolumeStep", 0.1F, 0.01F, 1.0F);
    televisionVolume = ReadFloat(
        path, L"Audio", L"TelevisionVolume", 1.0F, 0.0F, 2.0F);
    movieVolume = ReadFloat(
        path, L"Audio", L"MovieVolume", 1.0F, 0.0F, 2.0F);

    spdlog::info(
        "Configuration: EnableNativeMainMenuBink={}, "
        "MuteVanillaMenuMusic={}, KeepPlayingWhenBorderless={}, "
        "EnableWorldScreens={}, EnablePipBoyPlayer={}, RecursiveMediaScan={}",
        enableNativeMainMenuBink,
        muteVanillaMenuMusic,
        keepPlayingWhenBorderless,
        enableWorldScreens,
        enablePipBoyPlayer,
        recursiveMediaScan);
    spdlog::info(
        "Media directories: main menu='{}', TV='{}', movies='{}'",
        mainMenuDirectory.string(),
        televisionDirectory.string(),
        movieDirectory.string());
    spdlog::info(
        "Main-menu controls: stop={}, next={}, volume up={}, "
        "volume down={}, step={:.2f}, help={} ms",
        mainMenuStopKey,
        mainMenuNextKey,
        mainMenuVolumeUpKey,
        mainMenuVolumeDownKey,
        mainMenuVolumeStep,
        mainMenuHelpMilliseconds);
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

bool Config::RecursiveMediaScan() noexcept
{
    return recursiveMediaScan;
}

bool Config::EnableWorldScreens() noexcept
{
    return enableWorldScreens;
}

bool Config::EnablePipBoyPlayer() noexcept
{
    return enablePipBoyPlayer;
}

std::filesystem::path Config::MainMenuDirectory()
{
    return mainMenuDirectory;
}

std::filesystem::path Config::TelevisionDirectory()
{
    return televisionDirectory;
}

std::filesystem::path Config::MovieDirectory()
{
    return movieDirectory;
}

std::uint32_t Config::TelevisionWidth() noexcept
{
    return televisionWidth;
}

std::uint32_t Config::TelevisionHeight() noexcept
{
    return televisionHeight;
}

std::uint32_t Config::ProjectorWidth() noexcept
{
    return projectorWidth;
}

std::uint32_t Config::ProjectorHeight() noexcept
{
    return projectorHeight;
}

std::uint32_t Config::PipBoyWidth() noexcept
{
    return pipBoyWidth;
}

std::uint32_t Config::PipBoyHeight() noexcept
{
    return pipBoyHeight;
}

std::uint32_t Config::PipBoyOverlayTimeoutMilliseconds() noexcept
{
    return pipBoyOverlayTimeoutMilliseconds;
}

std::uint32_t Config::MainMenuHelpMilliseconds() noexcept
{
    return mainMenuHelpMilliseconds;
}

std::uint32_t Config::MainMenuStopKey() noexcept
{
    return mainMenuStopKey;
}

std::uint32_t Config::MainMenuNextKey() noexcept
{
    return mainMenuNextKey;
}

std::uint32_t Config::MainMenuVolumeUpKey() noexcept
{
    return mainMenuVolumeUpKey;
}

std::uint32_t Config::MainMenuVolumeDownKey() noexcept
{
    return mainMenuVolumeDownKey;
}

float Config::MainMenuVolume() noexcept
{
    return mainMenuVolume;
}

float Config::MainMenuVolumeStep() noexcept
{
    return mainMenuVolumeStep;
}

float Config::TelevisionVolume() noexcept
{
    return televisionVolume;
}

float Config::MovieVolume() noexcept
{
    return movieVolume;
}
