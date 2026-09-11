#include "PCH.h"

#include "MainMenuMedia.h"
#include "MediaLibrary.h"

namespace MainMenuMedia
{
    namespace
    {
        struct CachedCatalog
        {
            std::filesystem::path directory;
            bool recursive{ true };
            bool includeBink{ true };
            std::vector<std::filesystem::path> videos;
            std::chrono::steady_clock::time_point lastScan;
        };

        std::mutex catalogMutex;
        std::vector<CachedCatalog> catalogs;
        std::optional<CachedCatalog> audioCatalog;

        std::string Lowercase(std::string value)
        {
            std::ranges::transform(
                value, value.begin(), [](const unsigned char character) {
                    return static_cast<char>(std::tolower(character));
                });
            return value;
        }

        bool EqualsInsensitive(const std::wstring_view left,
            const std::wstring_view right)
        {
            return std::ranges::equal(
                left, right, [](const wchar_t a, const wchar_t b) {
                    return std::towlower(a) == std::towlower(b);
                });
        }

        std::vector<std::filesystem::path> ScanBinkVideos(
            const std::filesystem::path& directory,
            const bool recursive)
        {
            std::vector<std::filesystem::path> videos;
            std::error_code error;
            const auto append =
                [&](const std::filesystem::directory_entry& entry) {
                    std::error_code entryError;
                    if (entry.is_regular_file(entryError) && !entryError &&
                        IsBinkVideo(entry.path())) {
                        videos.push_back(entry.path().lexically_normal());
                    }
                };
            if (recursive) {
                for (std::filesystem::recursive_directory_iterator
                         iterator(directory,
                             std::filesystem::directory_options::
                                 skip_permission_denied,
                             error),
                    end;
                    !error && iterator != end;
                    iterator.increment(error)) {
                    append(*iterator);
                }
            } else {
                for (std::filesystem::directory_iterator iterator(directory,
                         std::filesystem::directory_options::
                             skip_permission_denied,
                         error),
                    end;
                    !error && iterator != end;
                    iterator.increment(error)) {
                    append(*iterator);
                }
            }
            return videos;
        }
    } // namespace

    bool IsCarrierPath(const char* path)
    {
        if (!path) {
            return false;
        }

        constexpr std::size_t kMaximumPathLength{ 4096 };
        const std::size_t length = strnlen(path, kMaximumPathLength);
        if (length == 0 || length == kMaximumPathLength) {
            return false;
        }

        std::string normalized(path, length);
        normalized = Lowercase(std::move(normalized));
        const auto separator = normalized.find_last_of("\\/");
        const std::string_view filename =
            separator == std::string::npos
                ? std::string_view(normalized)
                : std::string_view(normalized).substr(separator + 1);
        return filename == "mainmenuloop.bk2";
    }

    bool IsBinkVideo(const std::filesystem::path& path)
    {
        return Lowercase(path.extension().string()) == ".bk2";
    }

    std::string Utf8Path(const std::filesystem::path& path)
    {
        const auto utf8 = path.u8string();
        return { reinterpret_cast<const char*>(utf8.data()), utf8.size() };
    }

    std::vector<std::filesystem::path> ScanVideos(
        const std::filesystem::path& directory,
        const bool recursive,
        const bool includeBink)
    {
        const auto normalized = directory.lexically_normal();
        const auto now = std::chrono::steady_clock::now();
        constexpr auto kCatalogLifetime = std::chrono::seconds(15);
        {
            std::scoped_lock lock(catalogMutex);
            const auto cached = std::ranges::find_if(
                catalogs, [&](const CachedCatalog& catalog) {
                    return catalog.directory == normalized &&
                           catalog.recursive == recursive &&
                           catalog.includeBink == includeBink &&
                           now - catalog.lastScan < kCatalogLifetime;
                });
            if (cached != catalogs.end()) {
                return cached->videos;
            }
        }

        MediaLibrary ordinaryLibrary(normalized, recursive);
        auto videos = ordinaryLibrary.Scan();
        if (includeBink) {
            auto bink = ScanBinkVideos(normalized, recursive);
            std::ranges::move(bink, std::back_inserter(videos));
        }
        std::ranges::sort(videos, {}, [](const auto& path) {
            return Lowercase(Utf8Path(path));
        });
        videos.erase(
            std::ranges::unique(videos,
                {},
                [](const auto& path) { return Lowercase(Utf8Path(path)); })
                .begin(),
            videos.end());

        {
            std::scoped_lock lock(catalogMutex);
            auto cached = std::ranges::find_if(
                catalogs, [&](const CachedCatalog& catalog) {
                    return catalog.directory == normalized &&
                           catalog.recursive == recursive &&
                           catalog.includeBink == includeBink;
                });
            if (cached == catalogs.end()) {
                catalogs.push_back(
                    { normalized, recursive, includeBink, videos, now });
            } else {
                cached->videos = videos;
                cached->lastScan = now;
            }
        }
        return videos;
    }

    std::vector<std::filesystem::path> ScanAudioSources(
        const std::filesystem::path& directory,
        const bool recursive)
    {
        const auto normalized = directory.lexically_normal();
        const auto now = std::chrono::steady_clock::now();
        constexpr auto kCatalogLifetime = std::chrono::seconds(15);
        {
            std::scoped_lock lock(catalogMutex);
            if (audioCatalog && audioCatalog->directory == normalized &&
                audioCatalog->recursive == recursive &&
                now - audioCatalog->lastScan < kCatalogLifetime) {
                return audioCatalog->videos;
            }
        }

        const MediaLibrary library(normalized, recursive);
        auto sources = library.ScanAudioSources();
        {
            std::scoped_lock lock(catalogMutex);
            audioCatalog =
                CachedCatalog{ normalized, recursive, false, sources, now };
        }
        return sources;
    }

    std::optional<std::filesystem::path> FindXwmSidecar(
        const std::filesystem::path& video)
    {
        auto direct = video;
        direct.replace_extension(L".xwm");
        std::error_code error;
        if (std::filesystem::is_regular_file(direct, error) && !error) {
            return direct.lexically_normal();
        }

        error.clear();
        const auto parent = video.parent_path();
        for (std::filesystem::directory_iterator iterator(parent,
                 std::filesystem::directory_options::skip_permission_denied,
                 error),
            end;
            !error && iterator != end;
            iterator.increment(error)) {
            std::error_code entryError;
            if (!iterator->is_regular_file(entryError) || entryError) {
                continue;
            }
            const auto& candidate = iterator->path();
            if (EqualsInsensitive(candidate.extension().wstring(), L".xwm") &&
                EqualsInsensitive(
                    candidate.stem().wstring(), video.stem().wstring())) {
                return candidate.lexically_normal();
            }
        }
        return std::nullopt;
    }
} // namespace MainMenuMedia
