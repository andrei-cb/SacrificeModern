#pragma once
#include "core.hpp"

namespace sacrifice {
// SSET table used by 0x495490: set id, group count, then {group id, count, sample ids}.
struct SoundSet {
    struct Group {
        FourCC id;
        std::vector<FourCC> samples;
    };
    FourCC id;
    std::vector<Group> groups;
    static SoundSet decode(ByteView data);
    // First matching group; negative index draws once even for a one-sample group. Missing draws none.
    FourCC select(FourCC group, CrtRandom& random, int index = -1, int* selected = nullptr) const;
};
// SAMPLAY 0x120024a0 passes a truncated 0..127 volume to the 2D backend.
float sound_2d_gain(float volume, float master = 1);
} // namespace sacrifice
