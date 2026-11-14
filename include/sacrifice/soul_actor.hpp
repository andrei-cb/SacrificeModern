#pragma once
#include "scene.hpp"

namespace sacrifice {
// Native type-2 FLAG soul bundle. This run intentionally bypasses the common object run.
class SoulActor : public MatchObject {
  public:
    ~SoulActor() override;
    ObjectMotion motion;
    SoulState visual;
    int count{}, network_identity{-1};
    std::uint32_t soul_flags{}, eligible_sides{~0u}; // +1080 / +1084.
    MatchObject* anchor{};                           // +1072, corpse/unit whose center the soul follows.
    MatchObject* collector{};                        // +1124, followed during the absorption animation.
    Vec3 destination, collected_from;
    unsigned frames{1};
    float lift{};
    bool accounted{};
    struct SoundRequest {
        MatchObject* owner{};
        FourCC group, sample; // 0x475b70 -> 0x44de40 selects from the global table inline.
        Vec3 point;
    };
    std::vector<SoundRequest> sounds;
    std::function<void(SoulActor&, bool visible)> sync_visual;
    void initialize(Match& match, Vec3 position, MatchObject* source, int amount, unsigned atlas_frames);
    void bind_spatial(SpatialWorld& spatial);
    void collect(Match& match, MatchObject* target); // Offline 0x4765f0.
    bool reconstitute(Match& match, CreatureActor& target); // Offline 0x4764d0.
    bool run(Match& match, int ticks) override;
    void reference_event(Match& match, MatchObject& object, int code) override;
    const ObjectMotion* source_motion() const override {
        return &motion;
    }

  protected:
    void activated(Match& match) override;
    void deactivated(Match& match) override;

  private:
    SpatialWorld* spatial_{};
    void find_collector(Match& match);
};
} // namespace sacrifice
