#pragma once
#include "sacrifice/platform.hpp"
#include "sacrifice/renderer.hpp"

namespace sacrifice {
class WebGpuContext;
class WebSceneRenderer;
class PlayerControl;
class WebMedia;
class WebGameplay {
  public:
    WebGameplay(WebGpuContext& gpu, WebSceneRenderer& scene, const ResourceCatalog& catalog, WebMedia& media);
    ~WebGameplay();
    void drive(const InputState& input, bool blocked);
    void frame(const InputState& input, Camera& camera, unsigned width, unsigned height, float seconds,
               bool blocked);
    // The follow view behind the local wizard, before the first tick (a first-tick cinematic starts from it).
    void place_view(Camera& camera);
    void options(const GameOptions& options);
    bool following() const;
    bool cursor_drawn() const;
    PlayerControl* player();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace sacrifice
