#pragma once

namespace F4SEMinimal
{
    constexpr std::uint32_t PackVersion(
        const std::uint32_t major,
        const std::uint32_t minor,
        const std::uint32_t patch,
        const std::uint32_t build = 0) noexcept
    {
        return ((major & 0xFFU) << 24U) |
               ((minor & 0xFFU) << 16U) |
               ((patch & 0xFFFU) << 4U) |
               (build & 0xFU);
    }

    inline std::string VersionString(const std::uint32_t version)
    {
        return std::format(
            "{}.{}.{}",
            (version >> 24U) & 0xFFU,
            (version >> 16U) & 0xFFU,
            (version >> 4U) & 0xFFFU);
    }

    inline constexpr std::uint32_t kRuntimeOg =
        PackVersion(1, 10, 163);
    inline constexpr std::uint32_t kRuntimeNg980 =
        PackVersion(1, 10, 980);
    inline constexpr std::uint32_t kRuntimeNg984 =
        PackVersion(1, 10, 984);
    inline constexpr std::uint32_t kRuntimeAe137 =
        PackVersion(1, 11, 137);
    inline constexpr std::uint32_t kRuntimeAe159 =
        PackVersion(1, 11, 159);
    inline constexpr std::uint32_t kRuntimeAe169 =
        PackVersion(1, 11, 169);
    inline constexpr std::uint32_t kRuntimeAe191 =
        PackVersion(1, 11, 191);
    inline constexpr std::uint32_t kRuntimeAe221 =
        PackVersion(1, 11, 221);

    struct Interface
    {
        std::uint32_t f4seVersion;
        std::uint32_t runtimeVersion;
        std::uint32_t editorVersion;
        std::uint32_t isEditor;
        void* (*QueryInterface)(std::uint32_t);
        std::uint32_t (*GetPluginHandle)();
        std::uint32_t (*GetReleaseIndex)();
        const void* (*GetPluginInfo)(const char*);
    };

    inline constexpr std::uint32_t kInterfaceMessaging{ 1 };
    inline constexpr std::uint32_t kInterfaceScaleform{ 2 };
    inline constexpr std::uint32_t kInterfacePapyrus{ 3 };
    inline constexpr std::uint32_t kInterfaceSerialization{ 4 };
    inline constexpr std::uint32_t kMessageInputLoaded{ 7 };

    struct Message
    {
        const char* sender;
        std::uint32_t type;
        std::uint32_t dataLength;
        void* data;
    };

    struct MessagingInterface
    {
        static constexpr std::uint32_t kVersion{ 1 };

        std::uint32_t interfaceVersion;
        bool (*RegisterListener)(
            std::uint32_t,
            const char*,
            void*);
        bool (*Dispatch)(
            std::uint32_t,
            std::uint32_t,
            void*,
            std::uint32_t,
            const char*);
        void* (*GetEventDispatcher)(std::uint32_t);
    };

    struct PapyrusInterface
    {
        static constexpr std::uint32_t kVersion{ 2 };

        std::uint32_t interfaceVersion;
        bool (*Register)(void*);
        void (*GetExternalEventRegistrations)(
            const char*,
            void*,
            void*);
    };

    struct ScaleformInterface
    {
        static constexpr std::uint32_t kVersion{ 1 };

        using RegisterCallback = bool (*)(void*, void*);

        std::uint32_t interfaceVersion;
        bool (*Register)(const char*, RegisterCallback);
    };

    struct SerializationInterface
    {
        static constexpr std::uint32_t kVersion{ 1 };

        std::uint32_t interfaceVersion;
        void (*SetUniqueID)(std::uint32_t, std::uint32_t);
        void (*SetRevertCallback)(std::uint32_t, void*);
        void (*SetSaveCallback)(std::uint32_t, void*);
        void (*SetLoadCallback)(std::uint32_t, void*);
        void (*SetFormDeleteCallback)(std::uint32_t, void*);
        bool (*WriteRecord)(
            std::uint32_t,
            std::uint32_t,
            const void*,
            std::uint32_t);
        bool (*OpenRecord)(std::uint32_t, std::uint32_t);
        bool (*WriteRecordData)(const void*, std::uint32_t);
        bool (*GetNextRecordInfo)(
            std::uint32_t*,
            std::uint32_t*,
            std::uint32_t*);
        std::uint32_t (*ReadRecordData)(void*, std::uint32_t);
        bool (*ResolveHandle)(std::uint64_t, std::uint64_t*);
        bool (*ResolveFormID)(std::uint32_t, std::uint32_t*);
    };

    struct PluginInfo
    {
        static constexpr std::uint32_t kVersion{ 1 };

        std::uint32_t infoVersion;
        const char* name;
        std::uint32_t version;
    };

    struct PluginVersionData
    {
        static constexpr std::uint32_t kVersion{ 1 };
        static constexpr std::uint32_t kNoStructs{ 1U << 0U };

        constexpr void SetPluginName(const std::string_view value) noexcept
        {
            SetText(pluginName, value);
        }

        constexpr void SetAuthor(const std::string_view value) noexcept
        {
            SetText(author, value);
        }

        constexpr void AddCompatibleVersion(
            const std::uint32_t value) noexcept
        {
            for (auto& version : compatibleVersions) {
                if (version == 0) {
                    version = value;
                    return;
                }
            }
        }

        std::uint32_t dataVersion{ kVersion };
        std::uint32_t pluginVersion{ 0 };
        char pluginName[256]{};
        char author[256]{};
        std::uint32_t addressIndependence{ 0 };
        std::uint32_t structureIndependence{ kNoStructs };
        std::uint32_t compatibleVersions[16]{};
        std::uint32_t f4seMinimum{ 0 };
        std::uint32_t reservedNonBreaking{ 0 };
        std::uint32_t reservedBreaking{ 0 };
        std::uint8_t reserved[512]{};

    private:
        static constexpr void SetText(
            char (&destination)[256],
            const std::string_view value) noexcept
        {
            const std::size_t count =
                value.size() < 255 ? value.size() : 255;
            for (std::size_t index = 0; index < count; ++index) {
                destination[index] = value[index];
            }
            destination[count] = '\0';
        }
    };

    static_assert(sizeof(PluginVersionData) == 0x45C);
}
