#include "sacrifice/effect_audio.hpp"
#include "sacrifice/sound.hpp"
#include <algorithm>
#include <cmath>

namespace sacrifice {
namespace {
// ISOUND +80 / +84: the 3D provider's sample limit is a provider attribute; both pools use the 2D cap.
constexpr std::size_t positional_voices = 16, plain_voices = 16;
bool spatial(const EffectSound& request) {
    return request.flags & EffectSound::positional;
}
double distance(Vec3 a, Vec3 b) {
    const double x = static_cast<double>(a.x) - b.x, y = static_cast<double>(a.y) - b.y,
                 z = static_cast<double>(a.z) - b.z;
    return std::sqrt(x * x + y * y + z * z);
}
} // namespace

EffectAudio::EffectAudio(const ResourceCatalog& catalog, bool device) : catalog_(catalog), use_device_(device) {}

const std::pair<std::shared_ptr<const AudioClip>, Sample>* EffectAudio::load(FourCC id) {
    auto found = cache_.find(id);
    if (found == cache_.end()) {
        const auto location = catalog_.find_chunk(FourCC::from_text("SAMP"), id);
        if (!location)
            return nullptr; // 0x44de40 passes unresolved ids on; the manager's lookup then fails silently.
        auto sample = decode_sample(catalog_.read(*location));
        auto clip = std::make_shared<const AudioClip>(decode_audio(sample, {sample.sample_rate, sample.channels}));
        sample.audio.clear();
        found = cache_.emplace(id, std::pair{std::move(clip), std::move(sample)}).first;
    }
    return &found->second;
}

float EffectAudio::attenuation(const Voice& voice) const {
    if (!spatial(voice.request))
        return 1;
    // Miles 3D: full volume inside the minimum distance, inverse rolloff to the maximum, silent beyond.
    const double d = distance(voice.position, listener_);
    if (d <= voice.min_distance)
        return 1;
    if (d > voice.max_distance || d <= 0)
        return 0;
    return static_cast<float>(voice.min_distance / d);
}

void EffectAudio::refresh(Voice& voice) {
    const double d = distance(voice.position, listener_);
    // 0x12002fc0 for 3D handles; 2D samples report full audibility. SAMPLAY +56 = that * fade.
    voice.audibility = (spatial(voice.request) ? static_cast<float>(d >= 1 ? 1 / d : 1) : 1.0f) * voice.fade;
    // 0x12002d70: looping 3D sounds go quiet beyond max + 5 and return inside max - 5.
    if (voice.request.flags & EffectSound::loop && spatial(voice.request)) {
        if (d > voice.max_distance + 5.0)
            voice.out_of_range = true;
        if (d <= voice.max_distance - 5.0)
            voice.out_of_range = false;
    }
    voice.gain = voice.out_of_range ? 0 : sound_2d_gain(std::clamp(voice.request.volume * voice.fade, 0.0f, 1.0f),
                                                         volume_) * attenuation(voice);
    voice.pan = 0;
    if (spatial(voice.request) && d > 0) {
        const double side = (static_cast<double>(voice.position.x) - listener_.x) * right_.x +
                            (static_cast<double>(voice.position.y) - listener_.y) * right_.y +
                            (static_cast<double>(voice.position.z) - listener_.z) * right_.z;
        voice.pan = static_cast<float>(std::clamp(side / d, -1.0, 1.0));
    }
}

void EffectAudio::release(Voice& voice) {
    if (voice.device && output_)
        output_->halt(voice.device);
    voice.device = 0;
}

std::uint64_t EffectAudio::play(const EffectSound& request, const Locate& locate) {
    if (!request.sample.value)
        return 0;
    const auto* loaded = load(request.sample);
    if (!loaded || !loaded->first || loaded->first->samples.empty())
        return 0;
    Voice voice;
    voice.request = request;
    voice.request.limit = std::max(request.limit, 1u); // 0x12002740 raises a zero limit to one.
    voice.clip = loaded->first;
    voice.max_distance = loaded->second.far_range * 200;
    voice.min_distance = voice.max_distance * .1f;
    voice.position = request.point;
    if (request.flags & EffectSound::follow && locate)
        if (const auto at = locate(request.emitter))
            voice.position = *at;
    if (request.flags & EffectSound::fade_in) {
        voice.fade = 0;
        voice.fade_target = voice.fade_rate = 1;
    }
    refresh(voice);

    // 0x12002740: same-sample (0x2000000) or same-channel (0x1000000) sounds beyond the limit give way,
    // least audible first, unless they loop or carry priority themselves.
    const auto caps = request.flags & (EffectSound::sample_cap | EffectSound::channel_cap);
    std::size_t spatial_count = 0, plain_count = 0;
    std::vector<Voice*> matching;
    for (auto& other : voices_) {
        (spatial(other.request) ? spatial_count : plain_count)++;
        if ((other.request.flags & caps) &&
            ((caps & EffectSound::sample_cap && other.request.sample == request.sample) ||
             (caps & EffectSound::channel_cap && other.request.channel == request.channel &&
              other.request.emitter == request.emitter)) &&
            !(other.request.flags & (EffectSound::priority | EffectSound::loop)))
            matching.push_back(&other);
    }
    const auto limit = voice.request.limit;
    const bool full = spatial(request) ? spatial_count >= positional_voices : plain_count >= plain_voices;
    if (full || matching.size() >= limit) {
        std::ranges::stable_sort(matching, [](const Voice* a, const Voice* b) { return a->audibility > b->audibility; });
        std::vector<std::uint64_t> evicted;
        while (matching.size() >= limit) {
            auto* least = matching.back();
            if (!(request.flags & EffectSound::priority) && least->audibility >= voice.audibility)
                break;
            (spatial(least->request) ? spatial_count : plain_count)--;
            evicted.push_back(least->key);
            matching.pop_back();
        }
        for (auto& other : voices_)
            if (std::ranges::find(evicted, other.key) != evicted.end())
                release(other);
        std::erase_if(voices_, [&](const Voice& other) { return std::ranges::find(evicted, other.key) != evicted.end(); });
        if (matching.size() >= limit ||
            (spatial(request) ? spatial_count >= positional_voices : plain_count >= plain_voices))
            return 0;
    }
    voice.key = next_key_++;
    if (use_device_ && voice.gain > 0) {
        if (!output_)
            output_ = std::make_unique<AudioOutput>();
        voice.device = output_->start(voice.clip, voice.gain, voice.pan, request.flags & EffectSound::loop);
    }
    voices_.push_back(std::move(voice));
    return voices_.back().key;
}

void EffectAudio::fade(std::uint64_t key, float target, float seconds, bool stop) {
    for (auto& voice : voices_)
        if (voice.key == key) {
            voice.fade_target = target;
            voice.stop_at_target = stop;
            voice.fade_rate = seconds > 0 ? std::abs(voice.fade - target) / seconds : 0;
            if (seconds <= 0) {
                voice.fade = target;
                if (stop)
                    release(voice); // Stop before the next speech line starts, not on the next audio tick.
            }
        }
}

std::uint64_t EffectAudio::find(std::uint32_t emitter, FourCC id) const {
    for (const auto& voice : voices_)
        if (voice.request.emitter == emitter && (voice.request.sample == id || voice.request.group == id) &&
            !voice.stop_at_target)
            return voice.key;
    return 0;
}

void EffectAudio::stop_emitter(std::uint32_t emitter) {
    for (auto& voice : voices_)
        if (voice.request.emitter == emitter)
            release(voice);
    std::erase_if(voices_, [&](const Voice& voice) { return voice.request.emitter == emitter; });
}

void EffectAudio::stop_loops(std::uint32_t emitter, FourCC group) {
    std::erase_if(voices_, [&](Voice& voice) {
        if (voice.request.emitter != emitter || !(voice.request.flags & EffectSound::loop) ||
            (group.value && voice.request.group != group && voice.request.sample != group))
            return false;
        release(voice);
        return true;
    });
}

void EffectAudio::listener(Vec3 position, Vec3 right) {
    listener_ = position;
    const double length = std::sqrt(static_cast<double>(right.x) * right.x + static_cast<double>(right.y) * right.y +
                                    static_cast<double>(right.z) * right.z);
    if (length > 0)
        right_ = {static_cast<float>(right.x / length), static_cast<float>(right.y / length),
                  static_cast<float>(right.z / length)};
}

bool EffectAudio::alive(std::uint64_t key) const {
    return std::ranges::any_of(voices_, [&](const Voice& voice) { return voice.key == key; });
}

void EffectAudio::advance(float seconds, const Locate& locate) {
    if (output_)
        output_->update();
    for (auto& voice : voices_) {
        voice.elapsed += seconds;
        if (voice.request.flags & EffectSound::follow && locate)
            if (const auto at = locate(voice.request.emitter))
                voice.position = *at;
        // 0x120024a0: the fade moves at its rate and finishes with a stop or a hold.
        if (voice.fade != voice.fade_target) {
            const float step = seconds * voice.fade_rate;
            voice.fade = voice.fade < voice.fade_target ? std::min(voice.fade + step, voice.fade_target)
                                                        : std::max(voice.fade - step, voice.fade_target);
            if (voice.fade_rate == 0)
                voice.fade = voice.fade_target;
        }
        refresh(voice);
    }
    const auto finished = [&](Voice& voice) {
        if (voice.stop_at_target && voice.fade == voice.fade_target)
            return true;
        if (voice.request.flags & EffectSound::loop || !voice.clip->channels || !voice.clip->sample_rate)
            return false;
        const double length =
            static_cast<double>(voice.clip->samples.size() / voice.clip->channels) / voice.clip->sample_rate;
        return voice.elapsed >= length;
    };
    for (auto& voice : voices_) {
        if (finished(voice)) {
            release(voice);
            voice.key = 0;
            continue;
        }
        if (!use_device_)
            continue;
        const bool audible = voice.gain > 0;
        if (voice.device && !output_->active(voice.device))
            voice.device = 0;
        if (voice.device && !audible && voice.request.flags & EffectSound::loop)
            release(voice); // Out-of-range loops release their device voice and restart on return.
        else if (voice.device)
            output_->adjust(voice.device, voice.gain, voice.pan);
        else if (audible && voice.request.flags & EffectSound::loop) {
            if (!output_)
                output_ = std::make_unique<AudioOutput>();
            voice.device = output_->start(voice.clip, voice.gain, voice.pan, true);
        }
    }
    std::erase_if(voices_, [](const Voice& voice) { return voice.key == 0; });
}

void EffectAudio::stop() {
    for (auto& voice : voices_)
        release(voice);
    voices_.clear();
    if (output_)
        output_->stop();
}

void EffectAudio::clear_cache() {
    stop();
    cache_.clear();
}

void EffectAudio::reset_device() {
    stop();
    output_.reset();
}

void EffectAudio::volume(float value) {
    if (!std::isfinite(value) || value < 0 || value > 1)
        throw Error("Effect volume outside 0..1");
    volume_ = value;
}
} // namespace sacrifice
