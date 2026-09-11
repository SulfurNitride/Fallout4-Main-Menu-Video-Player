#pragma once

namespace MainMenuMedia
{
    [[nodiscard]] bool IsCarrierPath(const char* path);
    [[nodiscard]] bool IsBinkVideo(const std::filesystem::path& path);
    [[nodiscard]] std::string Utf8Path(const std::filesystem::path& path);
    [[nodiscard]] std::vector<std::filesystem::path> ScanVideos(
        const std::filesystem::path& directory,
        bool recursive,
        bool includeBink);
    [[nodiscard]] std::vector<std::filesystem::path>
    ScanAudioSources(const std::filesystem::path& directory, bool recursive);
    [[nodiscard]] std::optional<std::filesystem::path> FindXwmSidecar(
        const std::filesystem::path& video);
} // namespace MainMenuMedia
