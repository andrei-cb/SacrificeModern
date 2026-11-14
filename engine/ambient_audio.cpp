#include "sacrifice/ambient_audio.hpp"
#include "sacrifice/sound.hpp"
#include <cmath>

namespace sacrifice {
AmbientAudio::AmbientAudio(const ResourceCatalog& catalog, Sink sink)
    : catalog_(catalog), sink_(std::move(sink)) {}
float AmbientAudio::submit(const AmbientCue& cue) {
    if (!cue.sample)
        return 0;
    auto found = cache_.find(*cue.sample);
    if (found == cache_.end()) {
        const auto location = catalog_.find_chunk(FourCC::from_text("SAMP"), *cue.sample);
        if (!location)
            throw Error("Cannot resolve ambient sample " + cue.sample->text());
        const auto sample = decode_sample(catalog_.read(*location));
        // Preserve the source channels/rate. Envelope ranges are not pitch or 2D gain controls.
        auto clip =
            std::make_shared<const AudioClip>(decode_audio(sample, {sample.sample_rate, sample.channels}));
        found = cache_.emplace(*cue.sample, std::move(clip)).first;
    }
    pending_.push_back({found->second, cue.volume});
    const auto& clip = *found->second;
    return clip.channels && clip.sample_rate
               ? static_cast<float>(clip.samples.size() / clip.channels) / static_cast<float>(clip.sample_rate)
               : 0;
}
void AmbientAudio::advance() {
    if (output_)
        output_->update();
    if (pending_.empty())
        return;
    if (!sink_ && !output_) {
        output_ = std::make_unique<AudioOutput>();
        output_->master_volume(volume_);
    }
    for (const auto& request : pending_) {
        const bool accepted = sink_ ? sink_(request.clip, sound_2d_gain(request.volume, volume_))
                                    : output_->queue(request.clip, request.volume) != 0;
        if (accepted)
            ++submitted_;
        else
            ++dropped_;
    }
    pending_.clear();
}
void AmbientAudio::stop() {
    pending_.clear();
    if (output_)
        output_->stop();
}
void AmbientAudio::clear_cache() {
    stop();
    cache_.clear();
}
void AmbientAudio::reset_device() {
    stop();
    output_.reset();
}
void AmbientAudio::volume(float value) {
    if (!std::isfinite(value) || value < 0 || value > 1)
        throw Error("Ambient volume outside 0..1");
    volume_ = value;
    if (output_)
        output_->master_volume(value);
}
} // namespace sacrifice
