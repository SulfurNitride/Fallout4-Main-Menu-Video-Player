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

    bool IsInsideRoot(
        const std::filesystem::path& root,
        const std::filesystem::path& candidate)
    {
        const auto normalizedRoot = std::filesystem::weakly_canonical(root);
        const auto normalizedCandidate =
            std::filesystem::weakly_canonical(candidate);
        const auto relative = normalizedCandidate.lexically_relative(
            normalizedRoot);
        return !relative.empty() &&
               *relative.begin() != ".." &&
               !relative.is_absolute();
    }

    std::uint64_t Fnv1a(
        const std::string_view value,
        std::uint64_t hash)
    {
        constexpr std::uint64_t prime{ 1099511628211ULL };
        for (const unsigned char byte : value) {
            hash ^= byte;
            hash *= prime;
        }
        return hash;
    }

    std::string DisplayName(const std::filesystem::path& path)
    {
        const auto utf8 = path.stem().generic_u8string();
        return {
            reinterpret_cast<const char*>(utf8.data()),
            utf8.size()
        };
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

void MediaLibrary::SetRecursive(const bool recursive) noexcept
{
    recursive_ = recursive;
}

const std::filesystem::path& MediaLibrary::Root() const noexcept
{
    return root_;
}

bool MediaLibrary::Recursive() const noexcept
{
    return recursive_;
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

std::vector<MediaLibrary::Item> MediaLibrary::Catalog(
    const std::vector<std::filesystem::path>& media) const
{
    std::vector<Item> result;
    result.reserve(media.size());
    for (const auto& path : media) {
        auto relative = RelativePath(path);
        auto id = MediaId(path);
        if (relative.empty() || id.empty()) {
            continue;
        }
        result.push_back(Item{
            .id = std::move(id),
            .displayName = DisplayName(path),
            .relativePath = std::move(relative)
        });
    }
    return result;
}

std::optional<std::filesystem::path> MediaLibrary::Resolve(
    const std::string_view mediaId) const
{
    if (mediaId.empty()) {
        return std::nullopt;
    }

    // Version-1 saves stored the category-relative path directly. Preserve
    // that fast path before resolving the new opaque 128-bit catalog ID.
    {
        std::u8string utf8;
        utf8.reserve(mediaId.size());
        std::ranges::transform(
            mediaId,
            std::back_inserter(utf8),
            [](const char value) {
                return static_cast<char8_t>(
                    static_cast<unsigned char>(value));
            });
        std::filesystem::path relative{ utf8 };
        if (!relative.is_absolute()) {
            const auto candidate = (root_ / relative).lexically_normal();
            std::error_code error;
            if (std::filesystem::is_regular_file(candidate, error) &&
                !error &&
                IsSupported(candidate) &&
                IsInsideRoot(root_, candidate)) {
                return candidate;
            }
        }
    }

    for (const auto& path : Scan()) {
        if (MediaId(path) == mediaId) {
            return path;
        }
    }
    return std::nullopt;
}

std::string MediaLibrary::MediaId(
    const std::filesystem::path& path) const
{
    const auto relative = RelativePath(path);
    if (relative.empty()) {
        return {};
    }

    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        return {};
    }

    const auto rootName = ComparablePath(root_.filename());
    std::string key;
    key.reserve(rootName.size() + relative.size() + 32);
    key.append(rootName);
    key.push_back('/');
    key.append(Lowercase(relative));
    key.push_back('\0');
    for (std::uint32_t shift = 0; shift < 64; shift += 8) {
        key.push_back(static_cast<char>((size >> shift) & 0xFFU));
    }

    constexpr std::uint64_t firstSeed{ 14695981039346656037ULL };
    constexpr std::uint64_t secondSeed{ 7809847782465536322ULL };
    const auto first = Fnv1a(key, firstSeed);
    const auto second = Fnv1a(key, secondSeed);
    return std::format("{:016x}{:016x}", first, second);
}

std::string MediaLibrary::RelativePath(
    const std::filesystem::path& path) const
{
    std::error_code error;
    auto relative = std::filesystem::relative(path, root_, error);
    if (error || relative.empty() || relative.is_absolute() ||
        *relative.begin() == "..") {
        return {};
    }
    const auto utf8 = relative.generic_u8string();
    return {
        reinterpret_cast<const char*>(utf8.data()),
        utf8.size()
    };
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

ShuffleBag::ShuffleBag(const std::uint64_t seed) :
    random_(seed)
{}

void ShuffleBag::Reset(std::vector<std::filesystem::path> media)
{
    source_ = std::move(media);
    bag_.clear();
    if (source_.empty()) {
        previous_.clear();
    }
}

std::optional<std::filesystem::path> ShuffleBag::Next()
{
    if (bag_.empty()) {
        Refill();
    }
    if (bag_.empty()) {
        return std::nullopt;
    }

    auto next = std::move(bag_.back());
    bag_.pop_back();
    previous_ = next;
    return next;
}

bool ShuffleBag::Empty() const noexcept
{
    return source_.empty();
}

std::size_t ShuffleBag::Size() const noexcept
{
    return source_.size();
}

void ShuffleBag::Refill()
{
    bag_ = source_;
    std::ranges::shuffle(bag_, random_);
    if (bag_.size() > 1 && bag_.back() == previous_) {
        std::swap(bag_.back(), bag_.front());
    }
}
