#pragma once
#include "core.hpp"
#include <nlohmann/json.hpp>
#include <optional>

namespace sacrifice {
// Portable documents contain fields/IDs, never native pointers or raw object memory.
class DocumentStore {
  public:
    DocumentStore(std::filesystem::path path, std::string kind, unsigned version)
        : path_(std::move(path)), kind_(std::move(kind)), version_(version) {}
    [[nodiscard]] std::optional<nlohmann::json> read() const;
    void write(const nlohmann::json& payload) const;

  private:
    std::filesystem::path path_;
    std::string kind_;
    unsigned version_;
};
} // namespace sacrifice
