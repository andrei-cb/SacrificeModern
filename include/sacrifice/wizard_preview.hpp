#pragma once
#include "assets.hpp"
#include <memory>

namespace sacrifice {
// Menu-sized, depth-buffered rendering of the same SAXS model/idle clip used in gameplay.
// Both frontends upload the resulting image through their ordinary UI texture path.
class WizardPreview {
  public:
    explicit WizardPreview(const ResourceCatalog& catalog);
    ~WizardPreview();
    bool update(FourCC wizard, float seconds, unsigned width, unsigned height);
    const Image& image() const;
    std::string_view error() const;
    unsigned triangles() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace sacrifice
