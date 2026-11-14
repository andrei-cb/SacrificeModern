#pragma once
#include "match.hpp"
#include "media.hpp"
#include <functional>
#include <map>

namespace sacrifice {
// Consumes resolved 0x452090 sample requests; weather loops/thunder use the EffectAudio host.
class AmbientAudio {
  public:
    // Test/offline sink receives the same quantized gain sent to XAudio2. Default uses the device.
    using Sink = std::function<bool(std::shared_ptr<const AudioClip>, float gain)>;
    explicit AmbientAudio(const ResourceCatalog& catalog, Sink sink = {});
    // Returns the queued clip's length in seconds (0 when the cue has no sample).
    float submit(const AmbientCue& cue);
    // The main loop's audio service is before match ticks: requests start on the following service.
    void advance();
    void stop();
    void clear_cache();
    void reset_device(); // Retain decoded samples but release an invalidated device before retry.
    void volume(float value);
    std::size_t cached_samples() const {
        return cache_.size();
    }
    std::size_t pending() const {
        return pending_.size();
    }
    std::uint64_t submitted() const {
        return submitted_;
    }
    std::uint64_t dropped() const {
        return dropped_;
    }
    const AudioOutput* output() const {
        return output_.get();
    }

  private:
    const ResourceCatalog& catalog_;
    Sink sink_;
    std::map<FourCC, std::shared_ptr<const AudioClip>> cache_;
    struct Pending {
        std::shared_ptr<const AudioClip> clip;
        float volume;
    };
    std::vector<Pending> pending_;
    std::unique_ptr<AudioOutput> output_;
    float volume_{1};
    std::uint64_t submitted_{}, dropped_{};
};
} // namespace sacrifice
