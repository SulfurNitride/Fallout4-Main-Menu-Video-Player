#pragma once

class MediaLibrary
{
public:
    explicit MediaLibrary(
        std::filesystem::path root = {},
        bool recursive = true);

    void SetRoot(std::filesystem::path root);
    [[nodiscard]] std::vector<std::filesystem::path> Scan() const;
    [[nodiscard]] std::vector<std::filesystem::path>
        ScanAudioSources() const;
    [[nodiscard]] static bool IsSupported(
        const std::filesystem::path& path);
    [[nodiscard]] static bool IsAudioSourceSupported(
        const std::filesystem::path& path);

private:
    [[nodiscard]] std::vector<std::filesystem::path> ScanMatching(
        bool audioSources) const;
    std::filesystem::path root_;
    bool recursive_{ true };
};
