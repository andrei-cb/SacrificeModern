#pragma once
#include "media.hpp"
#include <array>
#include <filesystem>
#include <future>

namespace sacrifice {
// The music manager created by 0x4084d0 (0x40bf90 / 0x40c010): seven categories, each with a level
// that requests raise (0x40c5d0) and time lowers. The frame update (0x40c2d0) plays the loudest one,
// fading the previous track out over the new category's fade time before switching.
class MusicDirector {
  public:
    enum Category { theme, battle, ritual, ritual_won, ritual_losing, victory, defeat };
    struct Rules { // 24-byte records at 0x4c8578.
        float minimum, maximum, scale, decay, initial, fade;
    };
    static constexpr std::array<Rules, 7> rules{{{7, 7, 0, 0, 7, 5},
                                                 {-5, 100, 1, .01f, 2.5f, 1},
                                                 {0, 10, 10, 1, 0, 1},
                                                 {0, 300, 300, 10, 0, 1},
                                                 {0, 500, 500, 10, 0, 1},
                                                 {0, 1000, 1000, 0, 0, 0},
                                                 {0, 1000, 1000, 0, 0, 0}}};
    struct Track { // 76-byte records at 0x4c8620: category, mode, world key, file under data\music.
        int category, mode;
        std::uint32_t key;
        const char* file;
    };
    static const std::array<Track, 17> tracks;
    // `device` false keeps the selection logic without decoding or opening XAudio2 (smoke captures).
    MusicDirector(std::filesystem::path directory, bool device);
    ~MusicDirector();
    void request(int category, float weight); // 0x40c5d0.
    void enable(bool on);                     // 0x40c290(1, !on): disabled music resets every update.
    // 0x40c2d0 with whole elapsed ticks; `seconds` drives the fade timer (TIMER resolution 1.0).
    void update(int ticks, float seconds, std::uint32_t world, bool front_end);
    void reset(); // 0x40c260: stop, levels back to their initial values.
    void volume(float value);
    void release_device();
    int category() const {
        return category_;
    }
    const Track* playing() const {
        return track_;
    }
    float level(int index) const {
        return levels_[static_cast<std::size_t>(index)];
    }

  private:
    std::filesystem::path directory_;
    bool device_;
    std::array<float, 7> levels_{}, pending_{};
    int category_{-1};
    const Track* track_{};
    bool enabled_{};
    double clock_{}, switch_at_{};
    float fade_{1}, fade_target_{1}, fade_rate_{}, volume_{1};
    std::uint32_t random_{1}; // msvcrt rand() at 0x40c2d0 runs outside the match; kept separate here.
    std::unique_ptr<AudioOutput> output_;
    std::uint64_t voice_{};
    double started_{}, length_{};
    std::future<std::shared_ptr<const AudioClip>> loading_;
    std::vector<std::future<std::shared_ptr<const AudioClip>>> retired_;
    void stop(); // 0x40c600.
    void start(const Track& track);
    void fade_to(float target, float seconds); // 0x40c630.
    bool finished();
};
} // namespace sacrifice
