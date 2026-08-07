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
        constexpr std::uint32_t kRecordVersion1{ 1 };
        constexpr std::uint32_t kRecordVersion2{ 2 };
        constexpr std::uint32_t kCurrentRecordVersion{ kRecordVersion2 };
        constexpr std::uint32_t kMaximumRecordSize{ 1024 * 1024 };
        constexpr std::uint32_t kPayloadMagic1{
            FourCC('M', 'V', 'P', '1')
        };
        constexpr std::uint32_t kPayloadMagic2{
            FourCC('M', 'V', 'P', '2')
        };
        constexpr std::uint32_t kMaximumProgressEntries{ 4096 };

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

        void AppendProgress(
            std::vector<std::uint8_t>& output,
            const PlaybackChannel channel,
            const MediaProgress& progress)
        {
            const auto rawChannel = static_cast<std::uint8_t>(channel);
            const std::uint8_t completed = progress.completed ? 1 : 0;
            const std::uint16_t reserved = 0;
            Append(output, rawChannel);
            Append(output, completed);
            Append(output, reserved);
            Append(output, progress.positionSeconds);
            Append(output, progress.durationSeconds);
            Append(output, progress.lastPlayedMilliseconds);
            AppendString(output, progress.mediaId);
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

        bool ReadProgress(
            const std::vector<std::uint8_t>& input,
            std::size_t& offset,
            PlaybackChannel& channel,
            MediaProgress& progress)
        {
            std::uint8_t rawChannel = 0;
            std::uint8_t completed = 0;
            std::uint16_t reserved = 0;
            if (!Read(input, offset, rawChannel) ||
                !Read(input, offset, completed) ||
                !Read(input, offset, reserved) ||
                !Read(input, offset, progress.positionSeconds) ||
                !Read(input, offset, progress.durationSeconds) ||
                !Read(
                    input,
                    offset,
                    progress.lastPlayedMilliseconds) ||
                !ReadString(input, offset, progress.mediaId)) {
                return false;
            }

            if (rawChannel ==
                static_cast<std::uint8_t>(
                    PlaybackChannel::kTelevision)) {
                channel = PlaybackChannel::kTelevision;
            } else if (rawChannel ==
                       static_cast<std::uint8_t>(
                           PlaybackChannel::kProjector)) {
                channel = PlaybackChannel::kProjector;
            } else {
                progress.mediaId.clear();
            }
            progress.completed = completed != 0;
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
            payload.reserve(4096);
            Append(payload, kPayloadMagic2);
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

            auto televisionHistory =
                playback.Television()->ProgressHistory();
            auto projectorHistory =
                playback.Projector()->ProgressHistory();
            const auto historyCount = static_cast<std::uint32_t>(
                std::min<std::size_t>(
                    televisionHistory.size() + projectorHistory.size(),
                    kMaximumProgressEntries));
            Append(payload, historyCount);
            std::uint32_t written = 0;
            for (const auto& progress : televisionHistory) {
                if (written >= historyCount) {
                    break;
                }
                AppendProgress(
                    payload,
                    PlaybackChannel::kTelevision,
                    progress);
                ++written;
            }
            for (const auto& progress : projectorHistory) {
                if (written >= historyCount) {
                    break;
                }
                AppendProgress(
                    payload,
                    PlaybackChannel::kProjector,
                    progress);
                ++written;
            }

            if (!interface->WriteRecord(
                    kRecordType,
                    kCurrentRecordVersion,
                    payload.data(),
                    static_cast<std::uint32_t>(payload.size()))) {
                spdlog::error(
                    "F4SE could not save MMVP playback state");
            } else {
                spdlog::debug(
                    "Saved {} MMVP media progress entr{}",
                    historyCount,
                    historyCount == 1 ? "y" : "ies");
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
                    (version != kRecordVersion1 &&
                     version != kRecordVersion2) ||
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
                    ((version == kRecordVersion1 &&
                      magic != kPayloadMagic1) ||
                     (version == kRecordVersion2 &&
                      magic != kPayloadMagic2)) ||
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

                if (version == kRecordVersion2) {
                    std::uint32_t historyCount = 0;
                    if (!Read(payload, offset, historyCount) ||
                        historyCount > kMaximumProgressEntries) {
                        spdlog::warn(
                            "Ignored invalid MMVP progress history");
                        continue;
                    }

                    std::vector<MediaProgress> televisionHistory;
                    std::vector<MediaProgress> projectorHistory;
                    bool complete = true;
                    for (std::uint32_t index = 0;
                         index < historyCount;
                         ++index) {
                        PlaybackChannel channel{
                            PlaybackChannel::kTelevision
                        };
                        MediaProgress progress;
                        if (!ReadProgress(
                                payload,
                                offset,
                                channel,
                                progress)) {
                            complete = false;
                            break;
                        }
                        if (progress.mediaId.empty()) {
                            continue;
                        }
                        (channel == PlaybackChannel::kTelevision ?
                             televisionHistory :
                             projectorHistory)
                            .push_back(std::move(progress));
                    }
                    if (!complete) {
                        spdlog::warn(
                            "MMVP progress history ended unexpectedly");
                        continue;
                    }

                    auto& playback = WorldPlayback::GetSingleton();
                    if (auto* television = playback.Television()) {
                        television->RestoreProgressHistory(
                            std::move(televisionHistory));
                    }
                    if (auto* projector = playback.Projector()) {
                        projector->RestoreProgressHistory(
                            std::move(projectorHistory));
                    }
                    spdlog::info(
                        "Restored {} MMVP media progress entr{}",
                        historyCount,
                        historyCount == 1 ? "y" : "ies");
                }
            }
        }

        void __cdecl Revert(
            const F4SEMinimal::SerializationInterface*)
        {
            auto& playback = WorldPlayback::GetSingleton();
            if (auto* television = playback.Television()) {
                television->ClearProgressHistory();
                television->Stop();
                television->SetLoop(false);
                television->RefreshLibrary();
                television->Play();
            }
            if (auto* projector = playback.Projector()) {
                projector->ClearProgressHistory();
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
