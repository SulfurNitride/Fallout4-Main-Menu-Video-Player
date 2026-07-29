#include "PCH.h"

#include "Serialization.h"
#include "WorldPlayback.h"

namespace Serialization
{
    namespace
    {
        constexpr std::uint32_t FourCC(
            const char a,
            const char b,
            const char c,
            const char d) noexcept
        {
            return static_cast<std::uint32_t>(a) |
                   (static_cast<std::uint32_t>(b) << 8U) |
                   (static_cast<std::uint32_t>(c) << 16U) |
                   (static_cast<std::uint32_t>(d) << 24U);
        }

        constexpr std::uint32_t kUniqueId{
            FourCC('M', 'M', 'V', 'P')
        };
        constexpr std::uint32_t kRecordType{
            FourCC('S', 'T', 'A', 'T')
        };
        constexpr std::uint32_t kRecordVersion{ 1 };
        constexpr std::uint32_t kMaximumRecordSize{ 1024 * 1024 };
        constexpr std::uint32_t kPayloadMagic{
            FourCC('M', 'V', 'P', '1')
        };

        std::uint32_t pluginHandle{ 0 };

        template <class T>
        void Append(std::vector<std::uint8_t>& output, const T& value)
        {
            const auto* bytes =
                reinterpret_cast<const std::uint8_t*>(
                    std::addressof(value));
            output.insert(output.end(), bytes, bytes + sizeof(T));
        }

        void AppendString(
            std::vector<std::uint8_t>& output,
            const std::string_view value)
        {
            const auto size = static_cast<std::uint32_t>(
                std::min<std::size_t>(value.size(), 32768));
            Append(output, size);
            output.insert(
                output.end(),
                reinterpret_cast<const std::uint8_t*>(value.data()),
                reinterpret_cast<const std::uint8_t*>(value.data()) +
                    size);
        }

        template <class T>
        bool Read(
            const std::vector<std::uint8_t>& input,
            std::size_t& offset,
            T& value)
        {
            if (offset + sizeof(T) > input.size()) {
                return false;
            }
            std::memcpy(
                std::addressof(value),
                input.data() + offset,
                sizeof(T));
            offset += sizeof(T);
            return true;
        }

        bool ReadString(
            const std::vector<std::uint8_t>& input,
            std::size_t& offset,
            std::string& value)
        {
            std::uint32_t size = 0;
            if (!Read(input, offset, size) ||
                size > 32768 ||
                offset + size > input.size()) {
                return false;
            }
            value.assign(
                reinterpret_cast<const char*>(input.data() + offset),
                size);
            offset += size;
            return true;
        }

        void AppendSnapshot(
            std::vector<std::uint8_t>& output,
            const PlaybackChannel channel,
            const PlaybackSnapshot& snapshot)
        {
            const auto rawChannel =
                static_cast<std::uint8_t>(channel);
            const auto rawState =
                static_cast<std::uint8_t>(snapshot.state);
            const std::uint8_t loop = snapshot.loop ? 1 : 0;
            const std::uint8_t reserved = 0;
            Append(output, rawChannel);
            Append(output, rawState);
            Append(output, loop);
            Append(output, reserved);
            Append(output, snapshot.positionSeconds);
            AppendString(output, snapshot.mediaId);
        }

        bool ReadSnapshot(
            const std::vector<std::uint8_t>& input,
            std::size_t& offset)
        {
            std::uint8_t rawChannel = 0;
            std::uint8_t rawState = 0;
            std::uint8_t loop = 0;
            std::uint8_t reserved = 0;
            double positionSeconds = 0.0;
            std::string mediaId;
            if (!Read(input, offset, rawChannel) ||
                !Read(input, offset, rawState) ||
                !Read(input, offset, loop) ||
                !Read(input, offset, reserved) ||
                !Read(input, offset, positionSeconds) ||
                !ReadString(input, offset, mediaId)) {
                return false;
            }

            const auto state = static_cast<PlaybackState>(rawState);
            const bool paused = state == PlaybackState::kPaused;
            auto& playback = WorldPlayback::GetSingleton();
            WorldPlaybackSession* session = nullptr;
            if (rawChannel ==
                static_cast<std::uint8_t>(
                    PlaybackChannel::kTelevision)) {
                session = playback.Television();
            } else if (rawChannel ==
                       static_cast<std::uint8_t>(
                           PlaybackChannel::kProjector)) {
                session = playback.Projector();
            }
            if (!session || mediaId.empty()) {
                return true;
            }

            if (!session->Restore(
                    mediaId,
                    std::max(0.0, positionSeconds),
                    paused,
                    loop != 0)) {
                spdlog::warn(
                    "Saved {} media is no longer available: {}",
                    PlaybackChannelName(session->Channel()),
                    mediaId);
            }
            return true;
        }

        void DiscardRecord(
            const F4SEMinimal::SerializationInterface* interface,
            std::uint32_t length)
        {
            std::array<std::uint8_t, 4096> buffer{};
            while (length > 0) {
                const std::uint32_t requested = std::min<std::uint32_t>(
                    length,
                    static_cast<std::uint32_t>(buffer.size()));
                const std::uint32_t read = interface->ReadRecordData(
                    buffer.data(),
                    requested);
                if (read == 0) {
                    return;
                }
                length -= std::min(length, read);
            }
        }

        void __cdecl Save(
            const F4SEMinimal::SerializationInterface* interface)
        {
            auto& playback = WorldPlayback::GetSingleton();
            if (!playback.Initialized()) {
                return;
            }

            std::vector<std::uint8_t> payload;
            payload.reserve(512);
            Append(payload, kPayloadMagic);
            const std::uint32_t count = 2;
            Append(payload, count);
            AppendSnapshot(
                payload,
                PlaybackChannel::kTelevision,
                playback.Television()->Snapshot());
            AppendSnapshot(
                payload,
                PlaybackChannel::kProjector,
                playback.Projector()->Snapshot());

            if (!interface->WriteRecord(
                    kRecordType,
                    kRecordVersion,
                    payload.data(),
                    static_cast<std::uint32_t>(payload.size()))) {
                spdlog::error(
                    "F4SE could not save MMVP playback state");
            }
        }

        void __cdecl Load(
            const F4SEMinimal::SerializationInterface* interface)
        {
            std::uint32_t type = 0;
            std::uint32_t version = 0;
            std::uint32_t length = 0;
            while (interface->GetNextRecordInfo(
                &type,
                &version,
                &length)) {
                if (type != kRecordType ||
                    version != kRecordVersion ||
                    length < sizeof(std::uint32_t) * 2 ||
                    length > kMaximumRecordSize) {
                    DiscardRecord(interface, length);
                    continue;
                }

                std::vector<std::uint8_t> payload(length);
                if (interface->ReadRecordData(
                        payload.data(),
                        length) != length) {
                    spdlog::error(
                        "F4SE returned a truncated MMVP state record");
                    continue;
                }

                std::size_t offset = 0;
                std::uint32_t magic = 0;
                std::uint32_t count = 0;
                if (!Read(payload, offset, magic) ||
                    !Read(payload, offset, count) ||
                    magic != kPayloadMagic ||
                    count > 8) {
                    spdlog::warn(
                        "Ignored an invalid MMVP state record");
                    continue;
                }
                for (std::uint32_t index = 0;
                     index < count;
                     ++index) {
                    if (!ReadSnapshot(payload, offset)) {
                        spdlog::warn(
                            "MMVP state record ended unexpectedly");
                        break;
                    }
                }
            }
        }

        void __cdecl Revert(
            const F4SEMinimal::SerializationInterface*)
        {
            auto& playback = WorldPlayback::GetSingleton();
            if (auto* television = playback.Television()) {
                television->Stop();
                television->SetLoop(false);
                television->RefreshLibrary();
                television->Play();
            }
            if (auto* projector = playback.Projector()) {
                projector->Stop();
                projector->SetLoop(false);
                projector->RefreshLibrary();
            }
        }
    }

    bool Initialize(const F4SEMinimal::Interface* f4se)
    {
        if (!f4se || !f4se->QueryInterface || !f4se->GetPluginHandle) {
            return false;
        }
        auto* interface =
            static_cast<F4SEMinimal::SerializationInterface*>(
                f4se->QueryInterface(
                    F4SEMinimal::kInterfaceSerialization));
        if (!interface ||
            interface->interfaceVersion >
                F4SEMinimal::SerializationInterface::kVersion) {
            spdlog::warn(
                "F4SE serialization is unavailable; playback resume "
                "state will not be saved");
            return false;
        }

        pluginHandle = f4se->GetPluginHandle();
        interface->SetUniqueID(pluginHandle, kUniqueId);
        interface->SetSaveCallback(
            pluginHandle,
            reinterpret_cast<void*>(&Save));
        interface->SetLoadCallback(
            pluginHandle,
            reinterpret_cast<void*>(&Load));
        interface->SetRevertCallback(
            pluginHandle,
            reinterpret_cast<void*>(&Revert));
        spdlog::info("Registered F4SE playback-state serialization");
        return true;
    }
}
