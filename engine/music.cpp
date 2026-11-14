#include "sacrifice/music.hpp"
#include <algorithm>
#include <chrono>

namespace sacrifice {
const std::array<MusicDirector::Track, 17> MusicDirector::tracks{{
    {0, 1, 0x4a415f41, "james_normal.mp3"}, // JA_A
    {1, 0, 0, "Battle 3.mp3"},
    {0, 1, 0x70727363, "persephone_normal.mp3"}, // prsc
    {1, 0, 0, "Battle 5.mp3"},
    {3, 0, 0, "Sacrifice Victory.mp3"},
    {5, 2, 0, "Victory Theme.mp3"},
    {6, 2, 0, "Defeat Theme.mp3"},
    {2, 0, 0, "Sacrifice 1.mp3"},
    {1, 0, 0, "Battle 1.mp3"},
    {1, 0, 0, "Battle 2.mp3"},
    {1, 0, 0, "Battle 4.mp3"},
    {0, 1, 0x50595f41, "pyro_normal.mp3"},    // PY_A
    {0, 1, 0x53545f41, "stratos_normal.mp3"}, // ST_A
    {0, 1, 0x63686172, "charnel_normal.mp3"}, // char
    {0, 1, 0x6d656e75, "menu.mp3"},           // menu
    {0, 1, 0x65746872, "God Realm.mp3"},      // ethr
    {4, 0, 0, "Sacrifice Losing.mp3"},
}};

MusicDirector::MusicDirector(std::filesystem::path directory, bool device)
    : directory_(std::move(directory)), device_(device) {
    reset();
}
MusicDirector::~MusicDirector() {
    stop();
}

void MusicDirector::request(int category, float weight) {
    if (category < 0 || category > 6)
        return;
    const auto index = static_cast<std::size_t>(category);
    pending_[index] += weight * rules[index].scale;
}

void MusicDirector::enable(bool on) {
    enabled_ = on;
}

void MusicDirector::fade_to(float target, float seconds) {
    fade_target_ = target;
    fade_rate_ = seconds > 0 ? std::abs(fade_ - target) / seconds : 0;
    if (seconds <= 0)
        fade_ = target;
}

void MusicDirector::stop() {
    if (voice_ && output_)
        output_->halt(voice_);
    voice_ = 0;
    if (loading_.valid())
        retired_.push_back(std::move(loading_)); // A decode in flight finishes in the background.
    track_ = nullptr;
    category_ = -1;
}

void MusicDirector::reset() {
    stop();
    for (std::size_t i = 0; i < rules.size(); ++i) {
        levels_[i] = rules[i].initial;
        pending_[i] = 0;
    }
}

void MusicDirector::start(const Track& track) {
    track_ = &track;
    fade_ = fade_target_ = 1;
    fade_rate_ = 0;
    started_ = clock_;
    length_ = 0;
    if (!device_)
        return;
    const auto path = directory_ / track.file;
#ifdef __EMSCRIPTEN__
    // The browser build has no pthreads; retain the ready-future handoff used by update().
    std::promise<std::shared_ptr<const AudioClip>> result;
    loading_ = result.get_future();
    try {
        result.set_value(std::filesystem::exists(path)
                             ? std::make_shared<const AudioClip>(decode_audio(path))
                             : std::shared_ptr<const AudioClip>{});
    } catch (...) {
        result.set_exception(std::current_exception());
    }
#else
    loading_ = std::async(std::launch::async, [path] {
        if (!std::filesystem::exists(path))
            return std::shared_ptr<const AudioClip>{};
        return std::make_shared<const AudioClip>(decode_audio(path));
    });
#endif
}

bool MusicDirector::finished() {
    if (!device_ || loading_.valid() || !voice_ || !output_)
        return false;
    return !output_->active(voice_);
}

void MusicDirector::update(int ticks, float seconds, std::uint32_t world, bool front_end) {
    clock_ += seconds;
    std::erase_if(retired_, [](auto& future) {
        return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    });
    if (loading_.valid() && loading_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto clip = loading_.get();
        if (clip && !clip->samples.empty()) {
            if (!output_)
                output_ = std::make_unique<AudioOutput>();
            voice_ = output_->start(clip, std::clamp(volume_ * fade_, 0.0f, 1.0f), 0, false);
            length_ = clip->seconds();
        }
    }
    if (fade_ != fade_target_) {
        const float step = seconds * fade_rate_;
        fade_ = fade_ < fade_target_ ? std::min(fade_ + step, fade_target_) : std::max(fade_ - step, fade_target_);
    }
    if (voice_ && output_)
        output_->adjust(voice_, std::clamp(volume_ * fade_, 0.0f, 1.0f), 0);
    if (!enabled_) {
        reset();
        return;
    }
    // A finished track: one-shot themes (mode 2) reset every level, others reselect next update.
    if (track_ && finished()) {
        if (track_->mode & 2)
            reset();
        else
            stop();
    }
    int best = -1;
    float top = 0;
    for (std::size_t i = 0; i < rules.size(); ++i) {
        const auto& rule = rules[i];
        float value = std::clamp(levels_[i] + pending_[i], rule.minimum, rule.maximum);
        pending_[i] = std::clamp(pending_[i] * .1f, -rule.maximum, rule.maximum);
        if (best == -1 || top < value) {
            top = value;
            best = static_cast<int>(i);
        }
        levels_[i] = std::clamp(value - static_cast<float>(ticks) * rule.decay, rule.minimum, rule.maximum);
    }
    if (best == category_)
        return; // Natively also while a fade-out started for a category that lost again.
    if (track_) {
        if (switch_at_ == 0) {
            const float fade = rules[static_cast<std::size_t>(best)].fade;
            fade_to(0, fade);
            switch_at_ = clock_ + fade;
            return;
        }
        if (clock_ < switch_at_)
            return;
        stop();
    }
    switch_at_ = 0;
    category_ = best;
    const Track* chosen = nullptr;
    if (best >= 2) {
        for (const auto& track : tracks)
            if (track.category == best) {
                chosen = &track;
                break;
            }
    } else if (best == 1) {
        std::vector<const Track*> battle;
        for (const auto& track : tracks)
            if (track.category == 1)
                battle.push_back(&track);
        random_ = random_ * 214013u + 2531011u;
        chosen = battle[((random_ >> 16) & 0x7fff) % battle.size()];
    } else {
        // dword_4dbfac == 1 is the front end; otherwise the map's world. Pyro maps use tileset PYRO.
        auto key = front_end ? 0x6d656e75u : world == 0x5059524fu ? 0x50595f41u : world;
        for (const auto& track : tracks)
            if (track.category == 0 && track.key == key) {
                chosen = &track;
                break;
            }
    }
    if (!chosen) {
        stop();
        return;
    }
    start(*chosen);
    category_ = best;
    fade_to(1, rules[static_cast<std::size_t>(best)].fade);
}

void MusicDirector::volume(float value) {
    volume_ = std::clamp(value, 0.0f, 1.0f);
}

void MusicDirector::release_device() {
    stop();
    output_.reset();
}
} // namespace sacrifice
