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

    // SettingCollectionList::data in the official F4SE layout.
    constexpr std::size_t kSettingsListOffset{ 0x120 };
    constexpr std::size_t kMaximumSettings{ 8192 };
    constexpr std::size_t kMaximumSettingKeyLength{ 256 };
    constexpr std::uintptr_t kMinimumUserAddress{ 0x10000 };
    // Fallout 4 is a 64-bit process; reject non-canonical/kernel addresses
    // before following pointers from a runtime-specific structure.
    constexpr std::uintptr_t kMaximumUserAddress{ 0x00007FFFFFFFFFFF };

    std::uintptr_t iniSingletonOffset{ 0 };
    std::uintptr_t prefSingletonOffset{ 0 };
    std::atomic<bool> borderlessMode{ false };
    Setting* pauseOnAltTabSetting{ nullptr };
    Setting* alwaysActiveSetting{ nullptr };
    bool previousPauseOnAltTab{ true };
    bool previousAlwaysActive{ false };
    bool backgroundOverridesActive{ false };

    bool IsSaneAddress(const std::uintptr_t address,
        const std::size_t size,
        const std::size_t alignment) noexcept
    {
        return address >= kMinimumUserAddress &&
               address <= kMaximumUserAddress &&
               size <= kMaximumUserAddress - address &&
               (alignment <= 1 || address % alignment == 0);
    }

    bool IsSanePointer(const void* pointer,
        const std::size_t size = 1,
        const std::size_t alignment = alignof(void*)) noexcept
    {
        return IsSaneAddress(
            reinterpret_cast<std::uintptr_t>(pointer), size, alignment);
    }

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
        case F4SEMinimal::kRuntimeAe240:
            return 0x344B4B8;
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
        case F4SEMinimal::kRuntimeAe240:
            return 0x339FAE0;
        default:
            return 0;
        }
    }

    void* Collection(const std::uintptr_t offset)
    {
        if (offset == 0) {
            return nullptr;
        }
        const auto base =
            reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        if (!IsSanePointer(reinterpret_cast<const void*>(base),
                sizeof(void*),
                alignof(void*)) ||
            offset > kMaximumUserAddress - base ||
            !IsSaneAddress(base + offset, sizeof(void*), alignof(void*))) {
            return nullptr;
        }

        void* collection = nullptr;
        __try {
            collection = *reinterpret_cast<void**>(base + offset);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
        return IsSanePointer(collection) ? collection : nullptr;
    }

    Setting* FindSetting(void* collection, const std::string_view key)
    {
        if (!IsSanePointer(collection) ||
            reinterpret_cast<std::uintptr_t>(collection) >
                kMaximumUserAddress - kSettingsListOffset ||
            !IsSaneAddress(reinterpret_cast<std::uintptr_t>(collection) +
                               kSettingsListOffset,
                sizeof(SettingNode*),
                alignof(SettingNode*))) {
            return nullptr;
        }

        Setting* result = nullptr;
        bool malformedList = false;
        __try {
            auto* node = *reinterpret_cast<SettingNode**>(
                static_cast<std::byte*>(collection) + kSettingsListOffset);
            for (std::size_t index = 0; node && index < kMaximumSettings;
                ++index) {
                if (!IsSanePointer(node, sizeof(SettingNode))) {
                    malformedList = true;
                    break;
                }

                Setting* setting = node->value;
                if (IsSanePointer(setting, sizeof(Setting)) &&
                    IsSanePointer(setting->key, 1, alignof(char))) {
                    std::size_t keyLength = 0;
                    while (keyLength < kMaximumSettingKeyLength &&
                           setting->key[keyLength] != '\0') {
                        ++keyLength;
                    }
                    if (keyLength == key.size() &&
                        keyLength < kMaximumSettingKeyLength &&
                        std::string_view(setting->key, keyLength) == key) {
                        result = setting;
                        break;
                    }
                }

                SettingNode* next = node->next;
                if (next == node) {
                    malformedList = true;
                    break;
                }
                node = next;
            }
            if (node && !result && !malformedList) {
                malformedList = true;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            malformedList = true;
        }
        if (malformedList) {
            spdlog::warn(
                "Stopped traversing live Fallout settings while looking "
                "for {} because the list was invalid",
                key);
        }
        return result;
    }

    bool
    SetBoolean(void* collection, const std::string_view key, const bool value)
    {
        Setting* setting = FindSetting(collection, key);
        if (!setting) {
            spdlog::warn("Could not locate live Fallout setting {}", key);
            return false;
        }

        const bool previous = setting->value.boolean;
        setting->value.boolean = value;
        spdlog::info(
            "Set live Fallout setting {} from {} to {}", key, previous, value);
        return true;
    }

    void OnF4SEMessage(F4SEMinimal::Message* message)
    {
        if (message && message->type == F4SEMinimal::kMessageInputLoaded) {
            spdlog::info("F4SE input loaded; applying live main-menu settings");
            EngineSettings::Apply();
        }
    }
} // namespace

bool EngineSettings::Initialize(const F4SEMinimal::Interface* f4se,
    const std::uint32_t runtimeVersion)
{
    iniSingletonOffset = IniSingletonOffset(runtimeVersion);
    prefSingletonOffset = PrefSingletonOffset(runtimeVersion);
    if (iniSingletonOffset == 0 || prefSingletonOffset == 0) {
        spdlog::error("No live INI setting address is known for Fallout 4 {}",
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
            handle, "F4SE", reinterpret_cast<void*>(&OnF4SEMessage))) {
        spdlog::error("Could not register for F4SE lifecycle messages");
        return false;
    }
    spdlog::info("Registered live engine settings for Fallout 4 {}",
        F4SEMinimal::VersionString(runtimeVersion));
    if (!Apply()) {
        spdlog::info("Live Fallout settings are not ready during plugin load; "
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
        success = SetBoolean(collection, "bEnableMainMenuBink:General", true) &&
                  success;
    }
    if (Config::MuteVanillaMenuMusic()) {
        success = SetBoolean(collection, "bPlayMainMenuMusic:General", false) &&
                  success;
    }

    if (Setting* setting = FindSetting(
            Collection(prefSingletonOffset), "bBorderless:Display")) {
        borderlessMode.store(setting->value.boolean, std::memory_order_release);
        spdlog::info("Detected live Fallout borderless mode: {}",
            setting->value.boolean);
    } else {
        spdlog::warn("Could not locate live Fallout setting "
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
    if (backgroundOverridesActive || !Config::KeepPlayingWhenBorderless() ||
        !IsBorderlessMode()) {
        return;
    }

    void* collection = Collection(iniSingletonOffset);
    pauseOnAltTabSetting = FindSetting(collection, "bPauseOnAltTab:General");
    alwaysActiveSetting = FindSetting(collection, "bAlwaysActive:General");
    if (!pauseOnAltTabSetting || !alwaysActiveSetting) {
        spdlog::warn("Could not locate Fallout background-render settings");
        pauseOnAltTabSetting = nullptr;
        alwaysActiveSetting = nullptr;
        return;
    }

    previousPauseOnAltTab = pauseOnAltTabSetting->value.boolean;
    previousAlwaysActive = alwaysActiveSetting->value.boolean;
    pauseOnAltTabSetting->value.boolean = false;
    alwaysActiveSetting->value.boolean = true;
    backgroundOverridesActive = true;
    spdlog::info("Enabled background main-menu rendering: "
                 "bPauseOnAltTab {} -> false, bAlwaysActive {} -> true",
        previousPauseOnAltTab,
        previousAlwaysActive);
}

void EngineSettings::EndMainMenu()
{
    if (!backgroundOverridesActive) {
        return;
    }

    pauseOnAltTabSetting->value.boolean = previousPauseOnAltTab;
    alwaysActiveSetting->value.boolean = previousAlwaysActive;
    backgroundOverridesActive = false;
    pauseOnAltTabSetting = nullptr;
    alwaysActiveSetting = nullptr;
    spdlog::info("Restored Fallout background-render settings after main menu");
}
