#pragma once
#include "assets.hpp"
#include "media.hpp"
#include <functional>
#include <map>
#include <optional>

namespace sacrifice {
// One play request in the native 48-byte block passed to the sound manager's +48/+52 entries
// (0x44de40, 0x455460, 0x45c6b0, 0x465910). Word 0 holds the flags below.
struct EffectSound {
    enum : std::uint32_t {
        loop = 1,               // Miles loop count 0.
        positional = 0xe,       // Any of these bits selects a 3D sample handle (0x12002740 counts).
        fade_in = 0x40,         // 0x12002320: fade starts at 0 and rises at 1 per second.
        fixed_point = 0x20000,  // Block +16 holds a world point.
        follow = 0x40000,       // Position re-read from the emitter's matrix (0x12002d70).
        channel_cap = 0x1000000, // At most `limit` active sounds sharing `channel` (block +4).
        sample_cap = 0x2000000,  // At most `limit` active sounds of the same sample.
        priority = 0x4000000,   // Evicts the least audible capped sound unconditionally.
    };
    FourCC sample, group; // Group is retained for 0x44cbf0's stop lookup by the definition id.
    std::uint32_t flags{}, channel{}, limit{2};
    std::uint32_t emitter{}; // MatchObject::reference(); 0 for none.
    Vec3 point;
    float volume{1};
};
// The positional half of sound.dll / sound_audio.dll: Miles 3D samples with SAMP distances (max =
// far * 200, min = max / 10, 0x120042b0 / 0x12002bd0), emitter tracking, loops and fades. The device
// mix is XAudio2: Miles' inverse rolloff and its software panning are approximated in `attenuation`.
class EffectAudio {
  public:
    struct Voice {
        std::uint64_t key{};
        EffectSound request;
        std::shared_ptr<const AudioClip> clip;
        float min_distance{}, max_distance{};
        Vec3 position;
        float fade{1}, fade_target{1}, fade_rate{}; // SAMPLAY +76/+80/+84.
        bool stop_at_target{}, out_of_range{};      // 0x100 stop flag, +60 bit 2.
        float gain{}, pan{}, audibility{};          // Device gain (with master), -1..1, +56.
        double elapsed{};
        std::uint64_t device{};
    };
    using Locate = std::function<std::optional<Vec3>(std::uint32_t emitter)>;
    explicit EffectAudio(const ResourceCatalog& catalog, bool device = true);
    // Returns the voice key, or 0 when the request was rejected by a cap or had no sample.
    std::uint64_t play(const EffectSound& request, const Locate& locate = {});
    // SAMPLAY +16(target, seconds, stop): 0x44cbf0 stop events and 0x465910's one-second building fade.
    void fade(std::uint64_t key, float target, float seconds, bool stop);
    // Manager +60: the first live sound of `emitter` with this sample or definition group id.
    std::uint64_t find(std::uint32_t emitter, FourCC id) const;
    void stop_emitter(std::uint32_t emitter); // Manager +44 (0x451fb0 case 4).
    void stop_loops(std::uint32_t emitter, FourCC group = {});
    void listener(Vec3 position, Vec3 right);
    // Elapsed seconds; emitters that no longer resolve keep their last position.
    void advance(float seconds, const Locate& locate);
    void stop();
    void clear_cache(); // Map-local SAMP ids can be reused by the next mission.
    void reset_device();
    void volume(float value);
    const std::vector<Voice>& voices() const {
        return voices_;
    }
    bool alive(std::uint64_t key) const;
    float attenuation(const Voice& voice) const; // Distance gain 0..1 at the listener.

  private:
    const ResourceCatalog& catalog_;
    bool use_device_;
    std::map<FourCC, std::pair<std::shared_ptr<const AudioClip>, Sample>> cache_;
    std::vector<Voice> voices_;
    std::unique_ptr<AudioOutput> output_;
    Vec3 listener_, right_{1, 0, 0};
    float volume_{1};
    std::uint64_t next_key_{1};
    const std::pair<std::shared_ptr<const AudioClip>, Sample>* load(FourCC sample);
    void refresh(Voice& voice);
    void release(Voice& voice);
};
} // namespace sacrifice
