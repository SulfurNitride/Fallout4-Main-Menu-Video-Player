#include "PCH.h"

#include "BinkHook.h"
#include "Config.h"
#include "EngineSettings.h"
#include "F4SEMinimal.h"
#include "Log.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace
{
    constexpr std::string_view kPluginName{ "MainMenuVideoPlayer" };
    constexpr std::uint32_t kPluginVersion =
        F4SEMinimal::PackVersion(
            MMVP_VERSION_MAJOR,
            MMVP_VERSION_MINOR,
            MMVP_VERSION_PATCH);

    constexpr bool IsSupportedRuntime(const std::uint32_t runtime)
    {
#if defined(MMVP_RUNTIME_OG)
        return runtime == F4SEMinimal::kRuntimeOg;
#elif defined(MMVP_RUNTIME_NG)
        return runtime == F4SEMinimal::kRuntimeNg980 ||
               runtime == F4SEMinimal::kRuntimeNg984;
#elif defined(MMVP_RUNTIME_AE)
        return runtime == F4SEMinimal::kRuntimeAe137 ||
               runtime == F4SEMinimal::kRuntimeAe159 ||
               runtime == F4SEMinimal::kRuntimeAe169 ||
               runtime == F4SEMinimal::kRuntimeAe191 ||
               runtime == F4SEMinimal::kRuntimeAe221;
#else
#error "A Main Menu Video Player runtime variant must be selected"
#endif
    }

    consteval F4SEMinimal::PluginVersionData MakeVersionData()
    {
        F4SEMinimal::PluginVersionData data{};
        data.pluginVersion = kPluginVersion;
        data.SetPluginName(kPluginName);
        data.SetAuthor("Main Menu Video Player contributors");
        data.structureIndependence = 0;
#if defined(MMVP_RUNTIME_OG)
        data.AddCompatibleVersion(F4SEMinimal::kRuntimeOg);
#elif defined(MMVP_RUNTIME_NG)
        data.AddCompatibleVersion(F4SEMinimal::kRuntimeNg980);
        data.AddCompatibleVersion(F4SEMinimal::kRuntimeNg984);
#elif defined(MMVP_RUNTIME_AE)
        data.AddCompatibleVersion(F4SEMinimal::kRuntimeAe137);
        data.AddCompatibleVersion(F4SEMinimal::kRuntimeAe159);
        data.AddCompatibleVersion(F4SEMinimal::kRuntimeAe169);
        data.AddCompatibleVersion(F4SEMinimal::kRuntimeAe191);
        data.AddCompatibleVersion(F4SEMinimal::kRuntimeAe221);
#endif
        return data;
    }
}

extern "C" __declspec(dllexport) bool F4SEPlugin_Query(
    const F4SEMinimal::Interface* f4se,
    F4SEMinimal::PluginInfo* info)
{
    info->infoVersion = F4SEMinimal::PluginInfo::kVersion;
    info->name = kPluginName.data();
    info->version = kPluginVersion;

    if (f4se->isEditor != 0) {
        return false;
    }
    return IsSupportedRuntime(f4se->runtimeVersion);
}

extern "C" __declspec(dllexport) constinit
    F4SEMinimal::PluginVersionData F4SEPlugin_Version =
        MakeVersionData();

extern "C" __declspec(dllexport) bool F4SEPlugin_Load(
    const F4SEMinimal::Interface* f4se)
{
    Log::Initialize(reinterpret_cast<HMODULE>(&__ImageBase));
    Config::Load(reinterpret_cast<HMODULE>(&__ImageBase));
    spdlog::info(
        "{} {} loading for Fallout 4 {}",
        kPluginName,
        MMVP_VERSION,
        F4SEMinimal::VersionString(f4se->runtimeVersion));

    if (!EngineSettings::Initialize(f4se, f4se->runtimeVersion)) {
        spdlog::critical("Failed to initialize live Fallout settings");
        return false;
    }

    spdlog::info("Installing native Bink substitution hooks");
    if (!BinkHook::Install()) {
        spdlog::critical("Failed to install native Bink substitution hooks");
        return false;
    }
    spdlog::info("Native Bink substitution hooks installed successfully");
    return true;
}
