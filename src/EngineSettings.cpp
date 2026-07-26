#include "PCH.h"

#include "Config.h"
#include "EngineSettings.h"

namespace
{
    struct Setting
    {
        void* virtualTable;
        union
        {
            bool boolean;
            std::uint64_t storage;
        } value;
        const char* key;
    };
    static_assert(offsetof(Setting, value) == 0x8);
    static_assert(offsetof(Setting, key) == 0x10);
    static_assert(sizeof(Setting) == 0x18);

    struct SettingNode
    {
        Setting* value;
        SettingNode* next;
    };
    static_assert(sizeof(SettingNode) == 0x10);

    constexpr std::size_t kSettingsListOffset{ 0x118 };
    constexpr std::size_t kMaximumSettings{ 8192 };

    std::uintptr_t iniSingletonOffset{ 0 };
    std::uintptr_t prefSingletonOffset{ 0 };
    std::atomic<bool> borderlessMode{ false };
    Setting* pauseOnAltTabSetting{ nullptr };
    Setting* alwaysActiveSetting{ nullptr };
    bool previousPauseOnAltTab{ true };
    bool previousAlwaysActive{ false };
    bool backgroundOverridesActive{ false };

    std::uintptr_t IniSingletonOffset(const std::uint32_t runtime)
    {
        switch (runtime) {
        case F4SEMinimal::kRuntimeOg:
            return 0x5EDB528;
        case F4SEMinimal::kRuntimeNg980:
            return 0x3194198;
        case F4SEMinimal::kRuntimeNg984:
            return 0x3195198;
        case F4SEMinimal::kRuntimeAe137:
            return 0x3424F38;
        case F4SEMinimal::kRuntimeAe159:
            return 0x3425EB8;
        case F4SEMinimal::kRuntimeAe169:
            return 0x342B138;
        case F4SEMinimal::kRuntimeAe191:
            return 0x343AFB8;
        case F4SEMinimal::kRuntimeAe221:
            return 0x343B038;
        default:
            return 0;
        }
    }

    std::uintptr_t PrefSingletonOffset(const std::uint32_t runtime)
    {
        switch (runtime) {
        case F4SEMinimal::kRuntimeOg:
            return 0x5B5BE58;
        case F4SEMinimal::kRuntimeNg980:
            return 0x30EE6D0;
        case F4SEMinimal::kRuntimeNg984:
            return 0x30EF6D0;
        case F4SEMinimal::kRuntimeAe137:
            return 0x337E960;
        case F4SEMinimal::kRuntimeAe159:
            return 0x337F8E0;
        case F4SEMinimal::kRuntimeAe169:
            return 0x3384B60;
        case F4SEMinimal::kRuntimeAe191:
            return 0x33949E0;
        case F4SEMinimal::kRuntimeAe221:
            return 0x3394A60;
        default:
            return 0;
        }
    }

    void* Collection(const std::uintptr_t offset)
    {
        if (offset == 0) {
            return nullptr;
        }
        const auto base = reinterpret_cast<std::uintptr_t>(
            GetModuleHandleW(nullptr));
        return *reinterpret_cast<void**>(base + offset);
    }

    Setting* FindSetting(void* collection, const std::string_view key)
    {
        if (!collection) {
            return nullptr;
        }

        auto* node = reinterpret_cast<SettingNode*>(
            static_cast<std::byte*>(collection) +
            kSettingsListOffset);
        for (std::size_t index = 0;
             node && index < kMaximumSettings;
             ++index) {
            Setting* setting = node->value;
            if (setting && setting->key &&
                std::string_view(setting->key) == key) {
                return setting;
            }
            node = node->next;
        }
        return nullptr;
    }

    bool SetBoolean(
        void* collection,
        const std::string_view key,
        const bool value)
    {
        Setting* setting = FindSetting(collection, key);
        if (!setting) {
            spdlog::warn("Could not locate live Fallout setting {}", key);
            return false;
        }

        const bool previous = setting->value.boolean;
        setting->value.boolean = value;
        spdlog::info(
            "Set live Fallout setting {} from {} to {}",
            key,
            previous,
            value);
        return true;
    }

    void OnF4SEMessage(F4SEMinimal::Message* message)
    {
        if (message &&
            message->type == F4SEMinimal::kMessageInputLoaded) {
            spdlog::info(
                "F4SE input loaded; applying live main-menu settings");
            EngineSettings::Apply();
        }
    }
}

bool EngineSettings::Initialize(
    const F4SEMinimal::Interface* f4se,
    const std::uint32_t runtimeVersion)
{
    iniSingletonOffset = IniSingletonOffset(runtimeVersion);
    prefSingletonOffset = PrefSingletonOffset(runtimeVersion);
    if (iniSingletonOffset == 0 || prefSingletonOffset == 0) {
        spdlog::error(
            "No live INI setting address is known for Fallout 4 {}",
            F4SEMinimal::VersionString(runtimeVersion));
        return false;
    }

    if (!f4se->QueryInterface || !f4se->GetPluginHandle) {
        spdlog::error("F4SE messaging functions are unavailable");
        return false;
    }
    auto* messaging = static_cast<F4SEMinimal::MessagingInterface*>(
        f4se->QueryInterface(F4SEMinimal::kInterfaceMessaging));
    if (!messaging ||
        messaging->interfaceVersion <
            F4SEMinimal::MessagingInterface::kVersion ||
        !messaging->RegisterListener) {
        spdlog::error("F4SE messaging interface is unavailable");
        return false;
    }

    const std::uint32_t handle = f4se->GetPluginHandle();
    if (!messaging->RegisterListener(
            handle,
            "F4SE",
            reinterpret_cast<void*>(&OnF4SEMessage))) {
        spdlog::error("Could not register for F4SE lifecycle messages");
        return false;
    }
    spdlog::info(
        "Registered live engine settings for Fallout 4 {}",
        F4SEMinimal::VersionString(runtimeVersion));
    if (!Apply()) {
        spdlog::info(
            "Live Fallout settings are not ready during plugin load; "
            "they will be retried when the main-menu Bink opens");
    }
    return true;
}

bool EngineSettings::Apply()
{
    if (iniSingletonOffset == 0) {
        return false;
    }

    void* collection = Collection(iniSingletonOffset);
    if (!collection) {
        return false;
    }

    bool success = true;
    if (Config::EnableNativeMainMenuBink()) {
        success = SetBoolean(
                      collection,
                      "bEnableMainMenuBink:General",
                      true) &&
                  success;
    }
    if (Config::MuteVanillaMenuMusic()) {
        success = SetBoolean(
                      collection,
                      "bPlayMainMenuMusic:General",
                      false) &&
                  success;
    }

    if (Setting* setting = FindSetting(
            Collection(prefSingletonOffset),
            "bBorderless:Display")) {
        borderlessMode.store(
            setting->value.boolean,
            std::memory_order_release);
        spdlog::info(
            "Detected live Fallout borderless mode: {}",
            setting->value.boolean);
    } else {
        spdlog::warn(
            "Could not locate live Fallout setting "
            "bBorderless:Display");
    }
    return success;
}

bool EngineSettings::IsBorderlessMode() noexcept
{
    return borderlessMode.load(std::memory_order_acquire);
}

void EngineSettings::BeginMainMenu()
{
    if (backgroundOverridesActive ||
        !Config::KeepPlayingWhenBorderless() ||
        !IsBorderlessMode()) {
        return;
    }

    void* collection = Collection(iniSingletonOffset);
    pauseOnAltTabSetting = FindSetting(
        collection,
        "bPauseOnAltTab:General");
    alwaysActiveSetting = FindSetting(
        collection,
        "bAlwaysActive:General");
    if (!pauseOnAltTabSetting || !alwaysActiveSetting) {
        spdlog::warn(
            "Could not locate Fallout background-render settings");
        pauseOnAltTabSetting = nullptr;
        alwaysActiveSetting = nullptr;
        return;
    }

    previousPauseOnAltTab =
        pauseOnAltTabSetting->value.boolean;
    previousAlwaysActive =
        alwaysActiveSetting->value.boolean;
    pauseOnAltTabSetting->value.boolean = false;
    alwaysActiveSetting->value.boolean = true;
    backgroundOverridesActive = true;
    spdlog::info(
        "Enabled background main-menu rendering: "
        "bPauseOnAltTab {} -> false, bAlwaysActive {} -> true",
        previousPauseOnAltTab,
        previousAlwaysActive);
}

void EngineSettings::EndMainMenu()
{
    if (!backgroundOverridesActive) {
        return;
    }

    pauseOnAltTabSetting->value.boolean =
        previousPauseOnAltTab;
    alwaysActiveSetting->value.boolean =
        previousAlwaysActive;
    backgroundOverridesActive = false;
    pauseOnAltTabSetting = nullptr;
    alwaysActiveSetting = nullptr;
    spdlog::info(
        "Restored Fallout background-render settings after main menu");
}
