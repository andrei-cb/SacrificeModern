#pragma once
#include "core.hpp"
#include <memory>
#include <optional>

namespace sacrifice {
struct ResourceRecord {
    FourCC name, tag;
    std::uint32_t stored_size{}, size{}, children{}, flags{}, timestamp{};
    std::size_t offset{}, parent{}, subtree_end{};
    std::string path;
    [[nodiscard]] bool folder() const {
        return (flags & 1) != 0;
    }
    [[nodiscard]] bool compressed() const {
        return (flags & 2) != 0;
    }
};
class Archive {
  public:
    static Archive open(const std::filesystem::path& path);
    static Archive open_index(const std::filesystem::path& path);
    static Archive parse(Bytes data, std::string label);
    [[nodiscard]] const std::vector<ResourceRecord>& records() const {
        return records_;
    }
    [[nodiscard]] std::string_view label() const {
        return label_;
    }
    [[nodiscard]] std::size_t file_size() const {
        return file_size_;
    }
    [[nodiscard]] std::size_t resource_count() const;
    [[nodiscard]] Bytes read(std::size_t index) const;
    [[nodiscard]] std::optional<std::size_t> find(FourCC tag, std::optional<FourCC> name = {}) const;
    [[nodiscard]] std::optional<std::size_t> find_path(std::string_view path) const;
    // Scoped lookup searches descendants; global lookup also considers the root.
    [[nodiscard]] std::optional<std::size_t> find_chunk(FourCC tag, std::optional<FourCC> name = {},
                                                        std::optional<std::size_t> scope = {}) const;
    void validate_payloads() const;

  private:
    Bytes bytes_;
    std::filesystem::path backing_file_;
    std::size_t file_size_{};
    std::string label_;
    std::vector<ResourceRecord> records_;
    static Archive parse_directory(ByteView header, ByteView compressed, std::size_t file_size,
                                   std::string label);
};
std::vector<std::filesystem::path> container_paths(const std::filesystem::path& game_dir);
struct ResourceLocation {
    std::size_t archive{}, record{};
};
class ResourceCatalog {
  public:
    explicit ResourceCatalog(std::filesystem::path game_dir) : game_dir_(std::move(game_dir)) {}
    std::size_t mount(const std::filesystem::path& file);
    void mount_game();
    [[nodiscard]] std::optional<ResourceLocation> find(FourCC tag, FourCC name) const;
    [[nodiscard]] std::optional<ResourceLocation>
    find_chunk(FourCC tag, FourCC name, std::optional<ResourceLocation> scope = {}) const;
    [[nodiscard]] Bytes read(ResourceLocation location) const;
    [[nodiscard]] const Archive& archive(ResourceLocation location) const;
    [[nodiscard]] const std::vector<Archive>& archives() const {
        return archives_;
    }

  private:
    std::filesystem::path game_dir_;
    std::vector<Archive> archives_;
};
} // namespace sacrifice
