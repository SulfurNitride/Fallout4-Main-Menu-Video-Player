#pragma once

class MediaLibrary
{
public:
    explicit MediaLibrary(
        std::filesystem::path root = {},
        bool recursive = true);

    void SetRoot(std::filesystem::path root);
    void SetRecursive(bool recursive) noexcept;
    [[nodiscard]] const std::filesystem::path& Root() const noexcept;
    [[nodiscard]] bool Recursive() const noexcept;

    [[nodiscard]] std::vector<std::filesystem::path> Scan() const;
    [[nodiscard]] std::optional<std::filesystem::path> Resolve(
        std::string_view mediaId) const;
    [[nodiscard]] std::string MediaId(
        const std::filesystem::path& path) const;

    [[nodiscard]] static bool IsSupported(
        const std::filesystem::path& path);

private:
    std::filesystem::path root_;
    bool recursive_{ true };
};

class ShuffleBag
{
public:
    explicit ShuffleBag(std::uint64_t seed = std::random_device{}());

    void Reset(std::vector<std::filesystem::path> media);
    [[nodiscard]] std::optional<std::filesystem::path> Next();
    [[nodiscard]] bool Empty() const noexcept;
    [[nodiscard]] std::size_t Size() const noexcept;

private:
    void Refill();

    std::vector<std::filesystem::path> source_;
    std::vector<std::filesystem::path> bag_;
    std::filesystem::path previous_;
    std::mt19937_64 random_;
};
