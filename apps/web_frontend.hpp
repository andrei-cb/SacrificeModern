#pragma once
#include "sacrifice/options.hpp"
#include "sacrifice/character.hpp"
#include "sacrifice/platform.hpp"
#include "sacrifice/scene_match.hpp"
namespace sacrifice {
class WebGpuContext;
class WebMedia;
struct WorldSave;
class PlayerControl;
class WebFrontend {
  public:
    struct Launch {
        std::filesystem::path path;
        std::vector<SkirmishSlot> slots;
        bool campaign{};
        std::shared_ptr<const WorldSave> saved{};
        std::optional<Character> character{};
    };
    WebFrontend(WebGpuContext& gpu, const ResourceCatalog& catalog, std::filesystem::path root);
    ~WebFrontend();
    std::optional<Launch> take_launch();
    void loaded(SceneMatch& scene, const Launch& launch, PlayerControl* player);
    void detach();
    void view(Vec3 position, float yaw, float pitch);
    void failed(std::string error);
    void update(const InputState& input, PlayerControl* player, bool inspector, float seconds);
    void draw(unsigned width, unsigned height, bool inspector, WebMedia& media);
    const GameOptions& options() const;
    bool blocking() const;
    bool front_end() const;
    bool fast_forward(const InputState& input) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace sacrifice
