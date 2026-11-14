#include "sacrifice/particles.hpp"
#include <algorithm>
#include <cmath>

namespace sacrifice {
namespace {
constexpr float rand_scale = 0.000030517578125f; // 1 / 32768.
Vec3 rotate(const Orientation& m, const Vec3& v) { // sub_16013FD0: rotation only.
    return {v.x * m[0] + v.y * m[3] + v.z * m[6], v.x * m[1] + v.y * m[4] + v.z * m[7],
            v.x * m[2] + v.y * m[5] + v.z * m[8]};
}
Vec3 unrotate(const Orientation& m, const Vec3& v) { // Transposed rotation.
    return {v.x * m[0] + v.y * m[1] + v.z * m[2], v.x * m[3] + v.y * m[4] + v.z * m[5],
            v.x * m[6] + v.y * m[7] + v.z * m[8]};
}
float length(const Vec3& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}
} // namespace
ParticleSystem::ParticleSystem(CrtRandom& random, int capacity) : random_(&random) {
    // InitModel 0x1601e330: life 2..2, size 1..1, one cycle, cleared to 128 slots.
    clear(capacity);
}
float ParticleSystem::uniform(float low, float high) {
    return static_cast<float>(static_cast<double>(random_->next()) * (high - low) * rand_scale + low);
}
void ParticleSystem::reset_properties(bool identity) {
    // memset(+252, 0, 0x108): flags through the swarm block, including both frames and the position flag.
    flags = 0;
    cycles = 0;
    life = size = size_end = {};
    color_start = color_end = {};
    velocity_min = velocity_max = acceleration_min = acceleration_max = {};
    const Frame zero{{0, 0, 0, 0, 0, 0, 0, 0, 0}, {}};
    previous = current = identity ? Frame{} : zero; // 0x160128D0 makes identities for 132 and 139.
    box_min = box_max = swarm_scale = swarm_center = {};
    swarm_125 = swarm_126 = swarm_jitter = 0;
    positioned_ = false;
}
void ParticleSystem::clear(int capacity) {
    capacity_ = capacity;
    particles_.clear();
}
void ParticleSystem::enumerate(const std::function<int(Particle&)>& visitor) {
    std::erase_if(particles_, [&](Particle& particle) { return visitor(particle) == 2; });
}
void ParticleSystem::set_material(std::uint32_t mode, FourCC texture) {
    material_flags_ = mode;
    texture_ = texture;
}
void ParticleSystem::set_position(const Vec3& position, const Vec3* previous_position) {
    previous.position = previous_position ? *previous_position : positioned_ ? current.position : position;
    current.position = position;
    positioned_ = true;
}
void ParticleSystem::set_frame(const Frame& frame, const Frame* previous_frame) {
    previous = previous_frame ? *previous_frame : positioned_ ? current : frame;
    current = frame;
    positioned_ = true;
}
void ParticleSystem::fade(float minimum, float maximum) { // FadeParticle 0x16020ed0.
    for (auto& p : particles_) {
        p.life = uniform(minimum, maximum) * 60;
        for (unsigned i = 0; i < 4; ++i)
            p.color_rate[i] = -(p.color[i] / p.life);
    }
}
void ParticleSystem::set_property(int id, const float* a, const float* b) {
    const auto vec = [](const float* v) { return Vec3{v[0], v[1], v[2]}; };
    const auto frame = [](const float* v) {
        Frame f;
        std::copy_n(v, 9, f.rotation.begin());
        f.position = {v[9], v[10], v[11]};
        return f;
    };
    // The presets share their colour and alpha defaults: white, opaque, fading to transparent.
    const auto white = [&](float end_alpha) {
        color_start = {255, 255, 255, 255};
        color_end = {255, 255, 255, end_alpha};
    };
    const auto box = [&](float half_xy, float half_z) {
        box_min = {-half_xy, -half_xy, -half_z};
        box_max = {half_xy, half_xy, half_z};
    };
    const auto fill = [&](int limit) {
        for (int i = 0; i < limit && emit(0, 1); ++i) {
        }
    };
    switch (id) {
    case 0: {
        const auto position = vec(a);
        const auto previous_position = b ? vec(b) : Vec3{};
        set_position(position, b ? &previous_position : nullptr);
        return;
    }
    case 1: {
        const auto f = frame(a);
        const auto p = b ? frame(b) : Frame{};
        set_frame(f, b ? &p : nullptr);
        return;
    }
    case 2:
        flags |= 8;
        box_min = vec(a);
        box_max = vec(b);
        break;
    case 3:
        flags |= 2;
        velocity_min = vec(a);
        velocity_max = vec(b);
        return;
    case 4:
        flags |= 0x22;
        acceleration_min = vec(a);
        acceleration_max = vec(b);
        return;
    case 5:
        size = {*a, *b};
        return;
    case 6:
        size_end = {*a, *b};
        return;
    case 7:
        life = {*a, *b};
        return;
    case 8:
        clear(static_cast<int>(std::bit_cast<std::int32_t>(*a)));
        return;
    case 9:
        cycles = std::bit_cast<std::int32_t>(*a);
        return;
    case 10:
        fade(a[0], a[1]);
        return;
    case 11:
        set_flags(std::bit_cast<std::uint32_t>(*a), std::bit_cast<std::uint32_t>(*b));
        return;
    case 12:
        if (a)
            std::copy_n(a, 3, color_start.begin());
        if (b)
            std::copy_n(b, 3, color_end.begin());
        break;
    case 13:
        if (a)
            std::copy_n(a, 4, color_start.begin());
        if (b)
            std::copy_n(b, 4, color_end.begin());
        break;
    case 14:
        if (a)
            color_start[3] = *a;
        if (b)
            color_end[3] = *b;
        break;
    case 128: { // Ember fountain: a3 = (scale, rise, life).
        const float scale = a[0], rise = a[1], seconds = a[2];
        positioned_ = false;
        acceleration_min = acceleration_max = velocity_min = velocity_max = {};
        current = previous = {};
        box_min = {scale * -0.9f, scale * -0.9f, 0};
        box_max = {scale * 0.9f, scale * 0.9f, 0};
        size = {scale * 0.7f, scale * 0.7f + scale * 0.7f};
        size_end = {0.15f, 0.25f};
        life = {seconds * 0.75f, seconds};
        velocity_min.z = rise / (life[0] * 60);
        velocity_max.z = rise / (life[1] * 60);
        cycles = 4;
        white(0);
        flags = 0x20001b;
        break;
    }
    case 129: {
        const float scale = a[0];
        life = {b[0] * 0.5f, b[0]};
        positioned_ = false;
        box(scale * 0.2f, scale * 0.2f);
        size = {scale * 0.7f, scale * 0.7f + scale * 0.7f};
        size_end = {0.5f, 0.25f};
        cycles = 1;
        white(0);
        flags = 0x600009;
        break;
    }
    case 130: { // Burst: a3 = scale, a4 = speed (negative keeps full size with no alpha fade).
        const float scale = a[0];
        float speed = b[0];
        positioned_ = false;
        life = {1, 2};
        if (speed < 0) {
            color_end[3] = 255;
            color_start[3] = 255;
            size_end = {0, 0};
            speed = -speed;
        } else {
            color_start[3] = 255;
            color_end[3] = 0;
            size_end = {0.5f, 0.25f};
        }
        const float v = speed * scale;
        velocity_min = {-v / 120, -v / 120, -v / 240};
        velocity_max = {v / 120, v / 120, v * 0.0125f};
        acceleration_min = acceleration_max = {};
        acceleration_min.z = acceleration_max.z = -0.002725f;
        box(scale * 0.9f, scale * 0.9f);
        color_start[0] = color_start[1] = 255;
        size = {scale * 0.5f, scale * 0.5f + scale * 0.5f};
        cycles = 1;
        color_end[0] = color_end[1] = color_end[2] = 255;
        color_start[2] = 255;
        flags = 0x61002b;
        while (emit(0, 1)) {
        }
        break;
    }
    case 131: { // Drifting sparks: a3 = scale, a4 = life.
        const float scale = a[0];
        positioned_ = false;
        life = {b[0] * 0.5f, b[0]};
        box(scale * 0.2f, scale * 0.2f);
        velocity_min = velocity_max = {};
        acceleration_min = acceleration_max = {};
        acceleration_min.z = acceleration_max.z = -0.002725f;
        size = {scale * 0.5f, scale * 0.5f + scale * 0.5f};
        size_end = {0.5f, 0.25f};
        cycles = 4;
        white(0);
        flags = 0x61002f;
        break;
    }
    case 132: {
        reset_properties(true);
        const float scale = a[0];
        life = {b[0] * 0.5f, b[0]};
        box(scale * 0.2f, scale * 0.2f);
        size = {scale * 0.7f, scale * 0.7f + scale * 0.7f};
        size_end = {0.5f, 0.25f};
        cycles = 1;
        white(0);
        flags = 0x600018;
        break;
    }
    case 133: { // a3 = (scale, count, solid).
        const float scale = a[0];
        const int count = std::bit_cast<std::int32_t>(a[1]);
        positioned_ = false;
        life = {1.5f, 2.5f};
        box(scale * 0.2f, scale * 0.2f);
        size = {scale * 0.7f, scale * 0.7f + scale * 0.7f};
        size_end = {0.5f, 0.25f};
        white(0);
        acceleration_min = acceleration_max = {};
        acceleration_min.z = acceleration_max.z = -0.002725f;
        cycles = 1;
        flags = std::bit_cast<std::int32_t>(a[2]) ? 0x73002b : 0x63002b;
        fill(count);
        break;
    }
    case 134: {
        const float scale = a[0], seconds = b[0];
        reset_properties(false);
        positioned_ = false;
        life = {seconds * 0.9f, seconds};
        velocity_min.y = velocity_max.y = 0.01666666753590107f;
        size = {scale * 0.95f, scale * 1.05f};
        size_end = {1.9f, 2};
        cycles = 1;
        white(0);
        flags = 0x4000f;
        break;
    }
    case 135: { // a3 = scale, a4 = size.
        const float scale = a[0], extent = b[0];
        positioned_ = false;
        life = {2, 3};
        velocity_min = {-0.005f, -0.005f, scale * 5 / 120};
        velocity_max = {0.005f, 0.005f, scale * 10 / 120};
        acceleration_min = acceleration_max = {};
        box(scale * 0.9f, scale * 0.9f);
        color_start = {60, 60, 60, 60};
        size = {extent, extent * 1.5f};
        size_end = {0.5f, 0.25f};
        cycles = 1;
        color_end = {0, 0, 0, 0};
        flags = 0x61000b;
        while (emit(0, 1)) {
        }
        break;
    }
    case 136:
    case 141: { // Swarm: a3 = count, a4 = (scale, extent xyz, 126, 125, jitter bits).
        const int count = std::bit_cast<std::int32_t>(a[0]);
        positioned_ = false;
        swarm_scale = {b[1], b[2], b[3]};
        swarm_126 = b[4];
        swarm_125 = b[5];
        swarm_jitter = b[6];
        box_min = {-swarm_scale.x, -swarm_scale.x, -swarm_scale.x};
        box_max = {swarm_scale.x, swarm_scale.x, swarm_scale.x};
        life = {1000000, 1000000};
        cycles = 1000000;
        velocity_min = {-swarm_126, -swarm_126, -swarm_126};
        velocity_max = {swarm_126, swarm_126, swarm_126};
        size = {b[0] * 0.7f, b[0] * 0.7f + b[0] * 0.7f};
        size_end = {0, 0};
        color_start = color_end = {255, 255, 255, 255};
        if (id == 141) {
            flags = 0x11a;
            swarm_center = {};
        } else {
            flags = 0x1010b;
            swarm_center = current.position;
        }
        fill(count);
        break;
    }
    case 137: { // a3 = (scale, half extent, life).
        const float scale = a[0], half = a[1], seconds = a[2];
        reset_properties(false);
        box(half, half);
        size = {scale * 0.7f, scale * 0.7f + scale * 0.7f};
        size_end = {0, 0};
        cycles = 1;
        white(0);
        life = {seconds * 0.75f, seconds};
        flags = 0x68000a;
        break;
    }
    case 138: { // Ring: a3 = (scale, life, velocity scale); a4 = (radius, count).
        if (a) {
            const float scale = a[0];
            positioned_ = false;
            box_min = box_max = {};
            size = {scale * 0.7f, scale * 0.7f + scale * 0.7f};
            velocity_min.x = a[2];
            size_end = {0, 0};
            cycles = 1;
            white(0);
            life = {a[1] * 0.75f, a[1]};
            flags = 0x140061b;
        }
        if (b) {
            box_min = {-b[0], -b[0], -b[0]};
            box_max = {b[0], b[0], b[0]};
            const int count = static_cast<int>(static_cast<std::int64_t>(b[1]));
            float angles[2]{};
            const float step = static_cast<float>(6.28318 / count);
            for (int i = 0; i < count && emit(0, 1, angles); ++i)
                angles[1] += step;
        }
        break;
    }
    case 139:
    case 140: { // Segment: a3 = life (and end alpha for 140), a4 = the box corners.
        const bool fading = id == 140;
        reset_properties(!fading);
        positioned_ = false;
        life = {a[0] * 0.5f, a[0]};
        box_min = vec(b);
        box_max = vec(b + 3);
        const double span = length({box_min.x - box_max.x, box_min.y - box_max.y, box_min.z - box_max.z});
        float base;
        if (fading)
            base = static_cast<float>(std::clamp(span * 0.08838749999999999, 0.4, 1.0) * 0.7);
        else
            base = static_cast<float>(span * 0.1 * 0.7);
        size = {base, base + base};
        size_end = {0.5f, 0.25f};
        cycles = 1;
        white(fading ? a[1] : 0);
        if (fading) {
            acceleration_min = acceleration_max = {};
            acceleration_min.z = acceleration_max.z = -0.002725f;
            const float v = 5 / (life[1] * 60);
            velocity_min = {-v, -v, (v + v) * 0.3f};
            velocity_max = {v, v, v + v};
            flags = 0x61042b;
        } else
            flags = 0x600418;
        break;
    }
    default:
        return;
    }
    // LABEL_80: order the spawn box per axis.
    if (box_min.x > box_max.x)
        std::swap(box_min.x, box_max.x);
    if (box_min.y > box_max.y)
        std::swap(box_min.y, box_max.y);
    if (box_min.z > box_max.z)
        std::swap(box_min.z, box_max.z);
}
ParticleSystem::Particle* ParticleSystem::emit(float time, float fraction, const float* angles) {
    if (!texture_.value)
        return nullptr;
    const float lifetime = uniform(life[0], life[1]) * 60;
    if (static_cast<int>(particles_.size()) >= capacity_)
        return nullptr;
    auto& p = particles_.emplace_front();
    p.life = lifetime;
    const float detail = static_cast<float>(random_->next()) * rand_scale;
    p.detail = detail * 0.85000002f + 0.15000001f;
    if (time == 0)
        time = time_;
    if (fraction <= 0 || fraction > 1)
        fraction = detail;
    p.lag = time - run_time_;
    const auto& from = current.position;
    const auto& to = previous.position;
    p.position = {(to.x - from.x) * fraction + from.x, (to.y - from.y) * fraction + from.y,
                  (to.z - from.z) * fraction + from.z};
    for (unsigned i = 0; i < 4; ++i) {
        p.color[i] = color_start[i];
        p.color_rate[i] = (color_end[i] - color_start[i]) / p.life;
    }
    p.size = uniform(size[0], size[1]);
    p.size_rate = (uniform(size_end[0], size_end[1]) - 1) * p.size / p.life;
    Vec3 offset;
    if (flags & 8) {
        if (flags & 0x400)
            offset = box_min;
        else {
            offset.x = uniform(box_min.x, box_max.x);
            offset.y = uniform(box_min.y, box_max.y);
            offset.z = uniform(box_min.z, box_max.z);
        }
        if (flags & 0x600000) {
            const Vec3 middle{(box_max.x + box_min.x) * 0.5f, (box_max.y + box_min.y) * 0.5f,
                              (box_max.z + box_min.z) * 0.5f};
            offset = {offset.x - middle.x, offset.y - middle.y, offset.z - middle.z};
            const float* source = angles;
            if (flags & 0x200000) {
                const double angle = source ? source[0] : random_->next() * 3.14159 * 0.00006103515625;
                offset.x = static_cast<float>(std::sin(angle) * offset.x);
                offset.y = static_cast<float>(std::cos(angle) * offset.y);
            }
            if (flags & 0x400000) {
                const double angle = source ? source[1] : random_->next() * 3.14159 * 0.00006103515625;
                const double s = std::sin(angle);
                offset.x = static_cast<float>(s * offset.x);
                offset.y = static_cast<float>(s * offset.y);
                offset.z = static_cast<float>(std::cos(angle) * offset.z);
            }
            offset = {middle.x + offset.x, middle.y + offset.y, middle.z + offset.z};
        }
        if (flags & 0x800000)
            offset.x = 0;
        if (flags & 0x1000000)
            offset.y = 0;
        if (flags & 0x2000000)
            offset.z = 0;
        if (flags & 0x10)
            offset = rotate(current.rotation, offset);
        p.position = {p.position.x + offset.x, p.position.y + offset.y, p.position.z + offset.z};
    }
    if ((flags & 0x1000) && ground_)
        p.position.z = ground_(p.position, nullptr) + p.size;
    if (flags & 0x100)
        p.velocity = p.acceleration = {};
    else if (flags & 0x20) {
        Vec3 a{uniform(acceleration_min.x, acceleration_max.x), uniform(acceleration_min.y, acceleration_max.y),
               uniform(acceleration_min.z, acceleration_max.z)};
        if (flags & 0x40)
            a = rotate(current.rotation, a);
        p.acceleration = a;
    }
    if (flags & 2) {
        if (flags & 0x80000)
            p.velocity = {-(offset.x / p.life), -(offset.y / p.life), -(offset.z / p.life)};
        else if (flags & 0x200)
            p.velocity = {offset.x * velocity_min.x / p.life, offset.y * velocity_min.x / p.life,
                          offset.z * velocity_min.x / p.life};
        else {
            Vec3 v;
            if (flags & 0x800) {
                const double speed = uniform(velocity_min.x, velocity_max.x) /
                                     std::sqrt(offset.y * offset.y + offset.x * offset.x);
                v.x = static_cast<float>(speed * offset.x);
                v.y = static_cast<float>(speed * offset.y);
            } else {
                v.x = uniform(velocity_min.x, velocity_max.x);
                v.y = uniform(velocity_min.y, velocity_max.y);
            }
            v.z = uniform(velocity_min.z, velocity_max.z);
            if (flags & 4)
                v = rotate(current.rotation, v);
            p.velocity = v;
        }
    }
    p.frame = 0;
    p.frame_rate = static_cast<float>(cycles * static_cast<int>(frames_)) / p.life;
    return &p;
}
void ParticleSystem::advance(float time, float rate) {
    rate_ = rate;
    const double span = static_cast<double>(time) - time_;
    if (span > 0) {
        const auto total = static_cast<std::int64_t>(rate * span * 32768.0);
        if (static_cast<int>(total) > 0) {
            const double scale = 1.0 / static_cast<int>(total);
            int remaining = static_cast<int>(total), current_value = remaining;
            do {
                if (remaining < 0x8000) {
                    const int draw = random_->next();
                    if (draw > remaining)
                        break;
                    remaining = current_value = draw;
                }
                const float fraction = static_cast<float>(current_value * scale);
                emit(static_cast<float>((time - time_) * fraction + time_), fraction);
                remaining -= 0x8000;
                current_value = remaining;
            } while (remaining > 0);
        }
        time_ = time;
    }
    if (time_ - run_time_ <= ((flags & 0x100000) ? 8.0f : 16.0f))
        prune();
    else
        run();
}
void ParticleSystem::prune() { // DeleteOldParticles 0x16020e60.
    const float elapsed = time_ - run_time_;
    while (!particles_.empty() && particles_.back().life < elapsed)
        particles_.pop_back();
}
void ParticleSystem::run() {
    const float elapsed = time_ - run_time_;
    run_time_ = time_;
    if (elapsed <= 0)
        return;
    if (!texture_.value) {
        particles_.clear();
        return;
    }
    // Swarm frame (flags 0x100): look from the swarm centre towards the emitter, origin three quarters along.
    Orientation swarm_rotation{1, 0, 0, 0, 1, 0, 0, 0, 1};
    Vec3 swarm_origin = current.position, scale = swarm_scale;
    Vec3 sum;
    int alive = 0;
    if (flags & 0x100) {
        if (flags & 1) {
            const Vec3 d{current.position.x - swarm_center.x, current.position.y - swarm_center.y,
                         current.position.z - swarm_center.z};
            const float quarter = length(d) * 0.25f;
            if (quarter != 0) {
                const float n = 1 / (quarter * 4);
                const Vec3 y{d.x * n, d.y * n, d.z * n};
                Vec3 x{y.y, -y.x, 0};
                const float lx = length(x);
                x = lx > 0 ? Vec3{x.x / lx, x.y / lx, 0} : Vec3{1, 0, 0};
                const Vec3 z{x.y * y.z - x.z * y.y, x.z * y.x - x.x * y.z, x.x * y.y - x.y * y.x};
                swarm_rotation = {x.x, x.y, x.z, y.x, y.y, y.z, z.x, z.y, z.z};
                swarm_origin = {(current.position.x - swarm_center.x) * 0.75f + swarm_center.x,
                                (current.position.y - swarm_center.y) * 0.75f + swarm_center.y,
                                (current.position.z - swarm_center.z) * 0.75f + swarm_center.z};
                if (quarter > scale.y) {
                    const float k = quarter / quarter; // The native ratio is always one.
                    scale = {k * scale.x, quarter, k * scale.z};
                }
            }
        }
    }
    const float frames = static_cast<float>(frames_);
    for (auto it = particles_.begin(); it != particles_.end();) {
        auto& p = *it;
        const float d = elapsed - p.lag;
        p.lag = 0;
        if (flags & 2) {
            if (flags & 0x100) {
                const Vec3 rel{p.position.x - swarm_origin.x, p.position.y - swarm_origin.y,
                               p.position.z - swarm_origin.z};
                Vec3 local = unrotate(swarm_rotation, rel);
                const auto jitter = [&] {
                    return static_cast<float>((random_->next() * 2.0 * rand_scale - 1.0) * swarm_jitter);
                };
                p.velocity.x += jitter();
                p.velocity.y += jitter();
                p.velocity.z += jitter();
                local = {d * p.velocity.x + local.x / scale.x, d * p.velocity.y + local.y / scale.y,
                         d * p.velocity.z + local.z / scale.z};
                float reach = length(local);
                if (reach > 2) {
                    const float k = 2 / reach;
                    const Vec3 edge{local.x * k, local.y * k, local.z * k};
                    const float pull = std::sqrt(k);
                    local = {(local.x - edge.x) * pull + edge.x, (local.y - edge.y) * pull + edge.y,
                             (local.z - edge.z) * pull + edge.z};
                    reach = 2;
                }
                if (reach > 1) {
                    const Vec3 back{-(local.x / (reach * 30)), -(local.y / (reach * 30)), -(local.z / (reach * 30))};
                    const float t = (reach - 1) * 0.1f;
                    p.velocity = {(back.x - p.velocity.x) * t + p.velocity.x, (back.y - p.velocity.y) * t + p.velocity.y,
                                  (back.z - p.velocity.z) * t + p.velocity.z};
                }
                local = {local.x * scale.x, local.y * scale.y, local.z * scale.z};
                const auto world = rotate(swarm_rotation, local);
                p.position = {world.x + swarm_origin.x, world.y + swarm_origin.y, world.z + swarm_origin.z};
            } else if (flags & 0x20) {
                const float half = d * d * 0.5f;
                p.position = {p.acceleration.x * half + d * p.velocity.x + p.position.x,
                              p.acceleration.y * half + d * p.velocity.y + p.position.y,
                              p.acceleration.z * half + d * p.velocity.z + p.position.z};
                p.velocity = {d * p.acceleration.x + p.velocity.x, d * p.acceleration.y + p.velocity.y,
                              d * p.acceleration.z + p.velocity.z};
            } else
                p.position = {d * p.velocity.x + p.position.x, d * p.velocity.y + p.position.y,
                              d * p.velocity.z + p.position.z};
        }
        bool removed = false;
        bool collided = false;
        if ((flags & 0x10000) && ground_) {
            Vec3 normal{0, 0, 1};
            const float floor = ground_(p.position, &normal) + p.size * 0.5f;
            if (floor > p.position.z) {
                p.position.z = floor;
                const float bounce = (normal.z * p.velocity.z + normal.y * p.velocity.y + normal.x * p.velocity.x) * -1.5f;
                if (bounce > 0) {
                    p.velocity = {normal.x * bounce + p.velocity.x, normal.y * bounce + p.velocity.y,
                                  normal.z * bounce + p.velocity.z};
                    collided = (flags & 0x20000) != 0;
                    if (collided && (flags & 0x100000) && scar_)
                        scar_(p.position); // Native 8x8 SCAR:blud, at the ground-clamped collision point.
                }
            }
        }
        if (collided)
            removed = true;
        else {
            for (unsigned i = 0; i < 4; ++i)
                p.color[i] = std::clamp(d * p.color_rate[i] + p.color[i], 0.0f, 255.0f);
            p.size = d * p.size_rate + p.size;
            p.frame = d * p.frame_rate + p.frame;
            while (p.frame >= frames)
                p.frame -= frames;
            p.life -= d;
            removed = p.life < 0;
        }
        if (removed) {
            it = particles_.erase(it);
            continue;
        }
        sum = {sum.x + p.position.x, sum.y + p.position.y, sum.z + p.position.z};
        ++alive;
        ++it;
    }
    if (flags & 0x100) {
        if (flags & 1)
            swarm_center = alive ? Vec3{sum.x / alive, sum.y / alive, sum.z / alive} : current.position;
        else
            swarm_center = {};
    }
}
} // namespace sacrifice
