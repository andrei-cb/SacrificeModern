#pragma once
#include "assets.hpp"
#include <memory>

namespace sacrifice {
struct AudioClip {
    unsigned sample_rate{48000}, channels{2};
    std::vector<float> samples;
    [[nodiscard]] double seconds() const {
        return static_cast<double>(samples.size()) / (sample_rate * channels);
    }
};
struct AudioDecodeFormat {
    unsigned sample_rate{48000}, channels{2};
};
AudioClip decode_audio(ByteView bytes, AudioDecodeFormat format = {});
AudioClip decode_audio(const std::filesystem::path& path);
AudioClip decode_audio(const Sample& sample, AudioDecodeFormat format = {});
class VideoClip {
  public:
    explicit VideoClip(const std::filesystem::path& path);
    ~VideoClip();
    bool next(Image& image);
    [[nodiscard]] double frame_seconds() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
class AudioOutput {
  public:
    AudioOutput();
    ~AudioOutput();
    void play(AudioClip clip);
    void stop();
    // Native nonspatial pool: keep overlapping samples, at most 16 active 2D voices.
    std::uint64_t queue(std::shared_ptr<const AudioClip> clip, float volume = 1);
    void update();
    void master_volume(float volume);
    // Positional voices (EffectAudio): outside the 2D pool and master scaling, `gain` is final and
    // `pan` runs -1 (left) .. 1 (right) with constant power. A looping voice repeats until halted.
    std::uint64_t start(std::shared_ptr<const AudioClip> clip, float gain, float pan, bool loop);
    void adjust(std::uint64_t id, float gain, float pan);
    void halt(std::uint64_t id);
    bool active(std::uint64_t id) const;
    struct VoiceState {
        std::uint64_t id{}, samples_played{};
        unsigned buffers_queued{};
        float gain{};
    };
    std::vector<VoiceState> voices() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace sacrifice
