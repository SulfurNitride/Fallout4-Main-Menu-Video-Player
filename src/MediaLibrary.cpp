#include "PCH.h"

#include "MediaLibrary.h"

namespace
{
    constexpr std::array kVideoExtensions{
        ".3g2"sv,  ".3gp"sv, ".asf"sv,  ".avi"sv, ".f4v"sv,
        ".flv"sv,  ".m4v"sv, ".mkv"sv,  ".mov"sv, ".mp4"sv,
        ".mpeg"sv, ".mpg"sv, ".ogv"sv,  ".qt"sv,  ".vob"sv,
        ".webm"sv, ".wmv"sv
    };

    constexpr std::array kAudioExtensions{
        ".aac"sv,  ".ac3"sv, ".aif"sv,  ".aiff"sv, ".flac"sv,
        ".m4a"sv,  ".mp2"sv, ".mp3"sv,  ".ogg"sv,  ".opus"sv,
        ".wav"sv,  ".wma"sv, ".xma"sv,  ".xwm"sv
    };

    std::string Lowercase(std::string value)
    {
        std::ranges::transform(value, value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    }

    std::string ComparablePath(const std::filesystem::path& path)
    {
        auto value = path.generic_u8string();
        std::string result(
            reinterpret_cast<const char*>(value.data()),
            value.size());
        return Lowercase(std::move(result));
    }
}

MediaLibrary::MediaLibrary(
    std::filesystem::path root,
    const bool recursive) :
    root_(std::move(root)),
    recursive_(recursive)
{}

void MediaLibrary::SetRoot(std::filesystem::path root)
{
    root_ = std::move(root);
}

std::vector<std::filesystem::path> MediaLibrary::Scan() const
{
    return ScanMatching(false);
}

std::vector<std::filesystem::path> MediaLibrary::ScanAudioSources() const
{
    return ScanMatching(true);
}

std::vector<std::filesystem::path> MediaLibrary::ScanMatching(
    const bool audioSources) const
{
    std::vector<std::filesystem::path> videos;
    std::error_code error;
    if (!std::filesystem::is_directory(root_, error)) {
        return videos;
    }

    const auto append = [&](const std::filesystem::directory_entry& entry) {
        std::error_code entryError;
        if (entry.is_regular_file(entryError) &&
            !entryError &&
            (audioSources ?
                 IsAudioSourceSupported(entry.path()) :
                 IsSupported(entry.path()))) {
            videos.push_back(entry.path().lexically_normal());
        }
    };

    if (recursive_) {
        for (std::filesystem::recursive_directory_iterator iterator(
                 root_,
                 std::filesystem::directory_options::
                     skip_permission_denied,
                 error),
             end;
             !error && iterator != end;
             iterator.increment(error)) {
            append(*iterator);
        }
    } else {
        for (std::filesystem::directory_iterator iterator(
                 root_,
                 std::filesystem::directory_options::
                     skip_permission_denied,
                 error),
             end;
             !error && iterator != end;
             iterator.increment(error)) {
            append(*iterator);
        }
    }

    std::ranges::sort(videos, {}, [](const auto& path) {
        return ComparablePath(path);
    });
    return videos;
}

bool MediaLibrary::IsSupported(const std::filesystem::path& path)
{
    const std::string extension =
        Lowercase(path.extension().string());
    return std::ranges::find(kVideoExtensions, extension) !=
           kVideoExtensions.end();
}

bool MediaLibrary::IsAudioSourceSupported(
    const std::filesystem::path& path)
{
    const std::string extension =
        Lowercase(path.extension().string());
    return extension == ".bk2" ||
           std::ranges::find(kAudioExtensions, extension) !=
               kAudioExtensions.end() ||
           std::ranges::find(kVideoExtensions, extension) !=
               kVideoExtensions.end();
}
