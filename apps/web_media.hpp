#pragma once
#include "sacrifice/renderer.hpp"
namespace sacrifice {
class SceneMatch;
class PlayerControl;
class WebGpuContext;
class WebMedia {
  public:
    WebMedia(const ResourceCatalog& catalog, std::filesystem::path root);
    ~WebMedia();
    void reset();
    void bind(SceneMatch* scene);
    void update(SceneMatch* scene, PlayerControl* player, const Camera& camera, float seconds,
                bool fast_forward = false);
    void controls();
    void options(const GameOptions& options);
    // Native talking-head Smacker layers, drawn above the caption.
    float portrait(WebGpuContext& gpu, int god, unsigned emotion, std::uint64_t line, unsigned width,
                   unsigned height, float seconds);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace sacrifice
