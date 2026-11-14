#include "sacrifice/sound.hpp"
#include <algorithm>
#include <cmath>

namespace sacrifice {
SoundSet SoundSet::decode(ByteView data) {
    Reader reader(data, "SSET");
    SoundSet set;
    set.id = {reader.u32()};
    const auto count = reader.u32();
    if (count > reader.remaining() / 8)
        throw Error("SSET group table outside resource");
    for (std::uint32_t i = 0; i < count; ++i) {
        Group group;
        group.id = {reader.u32()};
        const auto samples = reader.u32();
        if (!samples || samples > reader.remaining() / 4)
            throw Error("Invalid SSET sample table");
        group.samples.reserve(samples);
        for (std::uint32_t s = 0; s < samples; ++s)
            group.samples.push_back({reader.u32()});
        set.groups.push_back(std::move(group));
    }
    if (reader.remaining())
        throw Error("Unconsumed SSET data");
    return set;
}
FourCC SoundSet::select(FourCC group, CrtRandom& random, int index, int* selected) const {
    for (const auto& entry : groups)
        if (entry.id == group) {
            if (entry.samples.empty())
                throw Error("Empty runtime sound group");
            const auto count = entry.samples.size();
            index = static_cast<int>(index < 0 ? count * random.next() / 32768 : index % count);
            if (selected)
                *selected = index;
            return entry.samples[index];
        }
    if (selected)
        *selected = index;
    return {};
}
float sound_2d_gain(float volume, float master) {
    if (!std::isfinite(volume) || !std::isfinite(master) || volume < 0 || master < 0)
        throw Error("Invalid sound volume");
    const double value = static_cast<double>(volume) * master * 127;
    return static_cast<float>(value >= 127 ? 1 : static_cast<int>(value) / 127.0);
}
} // namespace sacrifice
