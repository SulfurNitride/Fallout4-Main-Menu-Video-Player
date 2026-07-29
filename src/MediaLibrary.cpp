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
    std::vector<std::filesystem::path> videos;
    std::error_code error;
    if (!std::filesystem::is_directory(root_, error)) {
        return videos;
    }

    const auto append = [&](const std::filesystem::directory_entry& entry) {
        std::error_code entryError;
        if (entry.is_regular_file(entryError) &&
            !entryError &&
            IsSupported(entry.path())) {
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

std::optional<std::filesystem::path> MediaLibrary::Resolve(
    const std::string_view mediaId) const
{
    if (mediaId.empty()) {
        return std::nullopt;
    }

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
    if (relative.is_absolute()) {
        return std::nullopt;
    }

    const auto candidate = (root_ / relative).lexically_normal();
    std::error_code error;
    if (!std::filesystem::is_regular_file(candidate, error) ||
        error ||
        !IsSupported(candidate) ||
        !IsInsideRoot(root_, candidate)) {
        return std::nullopt;
    }
    return candidate;
}

std::string MediaLibrary::MediaId(
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
