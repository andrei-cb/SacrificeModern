struct Scene {
    view_projection: mat4x4f, world: mat4x4f, options: vec4f, material: vec4u,
    eye: vec4f, fog: vec4f, fog_color: vec4f, ambient: vec4f, sun_direction: vec4f,
    sun_color: vec4f, view_dir: vec4f, bump: vec4f, detail_light: vec4f,
    windows: array<vec4f, 6>, model_reveal: vec4f, point_options: vec4u, remaster: vec4f, weather: vec4f,
    shadow_matrix: mat4x4f, shadow: vec4f, wind: vec4f,
}
struct TileDetails { slots: array<vec4u, 64> }
@group(0) @binding(0) var<uniform> scene: Scene;
@group(0) @binding(1) var<uniform> tile_details: TileDetails;
struct PointSource { position_range: vec4f, color: vec4f, land_position_range: vec4f, land_color: vec4f }
@group(0) @binding(2) var<storage, read> point_sources: array<PointSource>;
// Remastered soft particles (options.z): a copy of the scene depth (reversed Z, near .1, far 30000).
@group(0) @binding(3) var scene_depth: texture_depth_2d;
fn scene_view_z(d: f32) -> f32 { return 3000 / (0.1 + d * 29999.9); }
// Remastered sun shadows: the shadow map seen from the sun through shadow_matrix; shadow = enabled,
// sampling spread, depth bias, model strength. 3x3 filtered comparisons, fading out at the square's edge.
@group(0) @binding(4) var shadow_map: texture_depth_2d;
@group(0) @binding(5) var shadow_sampler: sampler_comparison;
fn sun_visibility(w: vec3f) -> f32 {
    if (scene.shadow.x <= 0) { return 1.0; }
    let s = scene.shadow_matrix * vec4f(w, 1);
    let uv = s.xy * vec2f(0.5, -0.5) + 0.5;
    let edge = clamp((0.5 - abs(uv - 0.5)) * 10, vec2f(0), vec2f(1));
    let fade = min(edge.x, edge.y);
    if (fade <= 0 || s.z >= 1) { return 1.0; }
    var lit = 0.0;
    for (var y = -1; y <= 1; y++) {
        for (var x = -1; x <= 1; x++) {
            lit += textureSampleCompareLevel(shadow_map, shadow_sampler, uv + vec2f(f32(x), f32(y)) * scene.shadow.y,
                                             s.z - scene.shadow.z);
        }
    }
    return mix(1.0, lit / 9, fade);
}
@group(1) @binding(0) var image_sampler: sampler;
@group(1) @binding(1) var image: texture_2d<f32>;
@group(1) @binding(2) var terrain_tiles: texture_2d_array<f32>;
@group(1) @binding(3) var terrain_details: texture_2d_array<f32>;
@group(1) @binding(4) var terrain_windows: texture_2d_array<f32>;
@group(1) @binding(5) var terrain_edge: texture_2d<f32>;
struct In {
    @location(0) p: vec3f, @location(1) n: vec3f, @location(2) uv: vec2f,
    @location(3) c: vec4f, @location(4) tile: u32,
}
struct Out {
    @builtin(position) p: vec4f, @location(0) n: vec3f, @location(1) uv: vec2f,
    @location(2) c: vec4f, @location(3) @interpolate(flat) tile: u32,
    @location(4) w: vec3f, @location(5) detail: f32, @location(6) light: vec3f, @location(7) point_only: vec3f,
}
// Remastered foliage vertices (widgets.hpp foliage_layer): tile 0x40000000 with the height above the plant's
// root in 1/16 units (bits 8..19) and a per-plant hash (bits 0..7); 0x20000000 when the colour is the
// ground's baked light (the grass layer). Only widget-pass draws carry them.
fn is_foliage(tile: u32) -> bool {
    return scene.remaster.x > 0 && scene.material.w != 0 && (tile & 0xC0000000u) == 0x40000000u;
}
fn foliage_height(tile: u32) -> f32 { return f32((tile >> 8) & 0xfffu) / 16.0; }
// Wind (wind = time, strength, direction X/Z): a lean with slow gusts rolling across the map, plus a sway
// per plant; it grows with height, so roots stay put.
fn foliage_wind(w: vec3f, tile: u32) -> vec3f {
    let t = scene.wind.x; let phase = dot(w.xz, vec2f(0.13, 0.09)) + f32(tile & 0xffu) * 0.0246;
    let gust = 0.55 + 0.45 * sin(t * 0.6 - dot(w.xz, scene.wind.zw) * 0.035);
    let sway = sin(t * 1.9 + phase) * 0.35 + sin(t * 3.3 + phase * 1.7) * 0.15;
    let bend = WIND_BEND * pow(foliage_height(tile), 0.6) * scene.wind.y;
    return vec3f(scene.wind.z, 0, scene.wind.w) * bend * (gust + sway)
         + vec3f(-scene.wind.w, 0, scene.wind.z) * bend * 0.3 * sin(t * 2.7 + phase * 1.3);
}
// Remastered foliage shading: sun shadows and the lightning flash on the (CPU-lit) plants, extra sun on lit
// ones, leaves glowing against the sun, a colour shift per plant and shade at the root.
fn foliage(colour: vec3f, w: vec3f, tile: u32, h: f32) -> vec3f {
    var rgb = colour;
    let vis = sun_visibility(w); let sun = select(0.0, 1.0, scene.sun_color.w > 0);
    let baked = (tile & 0x20000000u) != 0;
    rgb *= mix(1 - SHADOW_GROUND * shadow_amount() * select(0.6, 1.0, baked), 1, vis) * (1 + LIGHTNING_LIGHT * scene.weather.w * vis);
    var up = 1.0;
    if (!baked) {
        rgb *= 1 + FOLIAGE_SUN * vis * sun * dot(sun_light(), vec3f(0.299, 0.587, 0.114));
        up = clamp(h / FOLIAGE_ROOT_HEIGHT, 0, 1);
    }
    let back = pow(clamp(dot(normalize(w - scene.eye.xyz), scene.sun_direction.xyz), 0, 1), 4) * FOLIAGE_BACKLIGHT * vis * sun * up;
    rgb += rgb * sun_light() * back * vec3f(1, 1.1, 0.7);
    let k1 = f32(tile & 15u) / 15.0 - 0.5; let k2 = f32((tile >> 4) & 15u) / 15.0 - 0.5;
    rgb *= (1 + FOLIAGE_TINT * 2 * k1) * (1 + FOLIAGE_TINT * vec3f(k2, -0.4 * k2, -k2));
    if (!baked) { rgb *= mix(FOLIAGE_ROOT, 1, up); }
    return rgb;
}
@vertex fn vs_main(v: In) -> Out {
    var o: Out;
    var world = scene.world * vec4f(v.p, 1);
    if (is_foliage(v.tile)) { world = vec4f(world.xyz + foliage_wind(world.xyz, v.tile), world.w); }
    o.p = scene.view_projection * world;
    o.w = world.xyz;
    o.n = normalize((scene.world * vec4f(v.n, 0)).xyz);
    o.uv = v.uv; o.c = v.c; o.tile = v.tile;
    var points = vec3f(0);
    if (scene.material.z == 0 && scene.material.w == 0) {
        let land = scene.material.y == 0 && scene.options.y > 0.5;
        var normal = o.n;
        if (land && v.tile != 0x80000006u) { normal = vec3f(0,1,0); }
        for (var i=0u; i<scene.point_options.x; i++) {
            let source = point_sources[i];
            var position = source.position_range; var color = source.color.rgb;
            if (land) { position=source.land_position_range; color=source.land_color.rgb; }
            let delta = position.xyz-o.w; let d2=dot(delta,delta); let r2=position.w*position.w;
            if (d2<=r2) { points += color*clamp(dot(normal,delta)*inverseSqrt(max(d2,1e-12)),0,1)/(1+3*d2/r2); }
        }
    }
    o.light = points;
    o.point_only = points;
    if (scene.material.y != 0 && scene.material.z == 0 && scene.material.w == 0) {
        o.light = clamp(scene.ambient.rgb + scene.sun_color.rgb * max(dot(o.n, scene.sun_direction.xyz), 0) + points, vec3f(0), vec3f(1));
        if (scene.sun_color.w == 0) {
            o.light = clamp(vec3f(0.4 + 0.6 * max(dot(o.n, normalize(vec3f(0.3, 1, -0.5))), 0)) + points,vec3f(0),vec3f(1));
        }
    }
    if (scene.model_reveal.w>0 && scene.model_reveal.z!=1) {
        let extent=scene.model_reveal.y-scene.model_reveal.x;
        let band=max(extent*0.25,5);
        let top=band+scene.model_reveal.y-(band+extent)*clamp(1-scene.model_reveal.z,0,1);
        o.c.a=floor(clamp((top-v.p.y)/band,0,1)*255)/255;
    }
    let delta = scene.detail_light.xyz - o.w;
    let d = length(delta); let range = scene.options.w * 1.25;
    o.detail = 0;
    if (range > 0 && d <= range) {
        o.detail = clamp(1.25 * max(delta.y / max(d, 0.001), 0) / (1 + 3 * d*d / (range*range)), 0, 1);
    }
    if (is_foliage(v.tile)) { o.detail = foliage_height(v.tile); } // Models don't use the detail light.
    return o;
}
// Weather on the sun: a storm (weather.z) overcasts it, dimming it and fading its shadows; a lightning
// flash (weather.w) lights the scene from the sun's side, bringing the shadows back for its moment.
fn shadow_amount() -> f32 {
    return clamp(1 - STORM_SHADOW_FADE * scene.weather.z + LIGHTNING_SHADOW * scene.weather.w, 0, 1);
}
fn sun_light() -> vec3f {
    return scene.sun_color.rgb * (1 - STORM_SUN_DIM * scene.weather.z) + vec3f(0.8, 0.87, 1) * LIGHTNING_LIGHT * scene.weather.w;
}
// A flash seen in the sky along `dir`: a glow gathered on the side it lights from (the sun's, raised to the
// shadow map's lowest elevation, so it agrees with the shadows), over a faint flicker everywhere.
fn lightning_glow(dir: vec3f) -> f32 {
    let lit_from = normalize(vec3f(scene.sun_direction.x, max(scene.sun_direction.y, SHADOW_MIN_ELEVATION), scene.sun_direction.z));
    return scene.weather.w * pow(clamp(0.5 + 0.5 * dot(dir, lit_from), 0, 1), LIGHTNING_SKY_FOCUS);
}
// The fog colour under a storm (weather.z): darker and greyer, matching the storm sky's horizon.
fn storm_fog_color() -> vec3f {
    let f = scene.fog_color.rgb * (1 - STORM_FOG_DARKEN * scene.weather.z);
    return mix(f, vec3f(dot(f, vec3f(0.299, 0.587, 0.114))), STORM_FOG_GREY * scene.weather.z);
}
// The rain mist's colour: a blue-grey lit by the scene's light (ambient and the storm-dimmed sun), so it
// reads as pale haze on any map, whatever its fog colour.
fn rain_fog_color() -> vec3f {
    let l = dot(scene.ambient.rgb + scene.sun_color.rgb * (1 - STORM_SUN_DIM * scene.weather.z), vec3f(0.299, 0.587, 0.114));
    return vec3f(0.86, 0.9, 1) * l * RAIN_FOG_LIGHT;
}
// Remastered Look (remaster.x): fog that eases in over the map's own range, mist that pools below the
// floor height (remaster.z) with falloff remaster.w, fog lit towards the sun, and rain mist that thickens
// with distance past a clear area around the camera while it rains or hails (weather.z).
fn remastered_fog(rgb: vec3f, w: vec3f, additive: bool, original: f32) -> vec3f {
    let ray = w - scene.eye.xyz; let dist = length(ray); let dir = ray / max(dist, 0.001);
    let t = max(dist - scene.fog.x, 0) / max(scene.fog.y - scene.fog.x, 0.001);
    var visible = exp(-FOG_EASE * t * t);
    if (scene.fog.w != 0) { visible = original; }
    let f = max(scene.remaster.w, 1); let x = clamp(dir.y * dist / f, -20, 60);
    var g = 1 - 0.5 * x;
    if (abs(x) > 0.001) { g = (1 - exp(-x)) / x; }
    let mist = min(MIST_DENSITY / max(scene.fog.y, 1) * dist * exp(clamp((scene.remaster.z - scene.eye.y) / f, -60, 4)) * g, 40);
    visible *= 1 - MIST_STRENGTH * (1 - exp(-mist));
    let rain = 1 - exp(-scene.weather.z * max(dist - RAIN_FOG_START, 0) / RAIN_FOG_DISTANCE);
    let sun = scene.sun_color.rgb * FOG_SUN_TINT * pow(clamp(dot(dir, scene.sun_direction.xyz), 0, 1), FOG_SUN_FOCUS) * (1 - scene.weather.z);
    if (additive) { return rgb * visible * (1 - rain); }
    let flash = scene.fog_color.rgb * LIGHTNING_FOG * lightning_glow(dir);
    return mix(mix(storm_fog_color() + sun + flash, rgb, visible), rain_fog_color() + flash, rain);
}
// The sky's layers: haze towards the fog colour at the horizon and a glow around the sun. Storms
// (weather.z) darken and grey the sky and hide the sun; a lightning flash (weather.w) lights the clouds on
// the side it lights the land from.
fn remastered_sky(rgb: vec3f, w: vec3f, additive: bool) -> vec3f {
    let dir = normalize(w - scene.eye.xyz);
    let storm = scene.weather.z;
    let h = pow(1 - abs(dir.y), HORIZON_FOCUS) * HORIZON_HAZE;
    let rain = pow(1 - abs(dir.y), 2) * RAIN_SKY_HAZE * storm; // Rain mist on the horizon.
    let lit = 1 - STORM_SKY_DARKEN * storm + LIGHTNING_SKY_AMBIENT * scene.weather.w + STORM_CLOUD_FLASH * lightning_glow(dir);
    if (additive) { return rgb * (1 - h) * (1 - rain) * lit; }
    let s = clamp(dot(dir, scene.sun_direction.xyz), 0, 1);
    let grey = mix(rgb, vec3f(dot(rgb, vec3f(0.299, 0.587, 0.114))), STORM_FOG_GREY * storm);
    let sky = mix(mix(grey, storm_fog_color(), h), rain_fog_color(), rain) * lit;
    return sky + scene.sun_color.rgb * (SUN_CORE * pow(s, SUN_CORE_FOCUS) + SUN_HALO * pow(s, SUN_HALO_FOCUS)) * (1 - storm);
}
// Remastered model lighting per pixel: a soft terminator, sky/ground ambient and a rim on the sun's side.
fn remastered_light(normal: vec3f, points: vec3f, w: vec3f) -> vec3f {
    let n = normalize(normal); let v = normalize(scene.eye.xyz - w); let ndl = dot(n, scene.sun_direction.xyz);
    let diffuse = clamp((ndl + LIGHT_WRAP) / (1 + LIGHT_WRAP), 0, 1);
    let sky = scene.ambient.rgb * (SKY_AMBIENT_BASE + SKY_AMBIENT_UP * (n.y * 0.5 + 0.5));
    let rim = pow(1 - clamp(dot(n, v), 0, 1), 3) * clamp(0.5 - 0.5 * dot(v, scene.sun_direction.xyz), 0, 1);
    let lit = mix(1 - scene.shadow.w * shadow_amount(), 1.0, sun_visibility(w));
    return min((sky + sun_light() * (diffuse + rim * RIM_LIGHT) * lit) * MODEL_BRIGHTNESS + points, vec3f(LIGHT_MAX));
}
// Smooth 2D value noise in 0..1 (drawing-only patterns).
fn noise_hash(p: vec2f) -> f32 { return fract(sin(dot(p, vec2f(127.1, 311.7))) * 43758.5453); }
fn value_noise(p: vec2f) -> f32 {
    let i = floor(p); var f = fract(p); f = f * f * (3 - 2 * f);
    return mix(mix(noise_hash(i), noise_hash(i + vec2f(1, 0)), f.x), mix(noise_hash(i + vec2f(0, 1)), noise_hash(i + vec2f(1, 1)), f.x), f.y);
}
// Remastered ground weather: settled snow (weather.y) whitens the lit ground, patchy by texture brightness;
// wet ground (weather.x) darkens and glints where it mirrors the sun.
fn remastered_ground(albedo: vec3f, unshadowed: vec3f, w: vec3f) -> vec3f {
    let vis = sun_visibility(w);
    let light = unshadowed * GROUND_BRIGHTNESS * mix(1 - SHADOW_GROUND * shadow_amount(), 1.0, vis) * (1 + LIGHTNING_LIGHT * scene.weather.w * vis);
    let luma = vec3f(0.299, 0.587, 0.114); let l = dot(albedo, luma);
    var snow = clamp(scene.weather.y * SNOW_COVER * 1.2 + (l - 0.45) * 1.5, 0, 1) * step(0.001, scene.weather.y);
    snow *= snow * (3 - 2 * snow);
    let v = normalize(scene.eye.xyz - w); let r = reflect(-v, vec3f(0, 1, 0));
    var glint = 0.0;
    if (scene.sun_color.w > 0) {
        // The sun mirrors in world-fixed puddle patches, and not while the storm overcasts it.
        glint = pow(clamp(dot(r, scene.sun_direction.xyz), 0, 1), WET_GLOSS) * WET_SHINE * scene.weather.x * vis * (1 - scene.weather.z)
              * smoothstep(1 - PUDDLE_COVER, 1.15 - PUDDLE_COVER, value_noise(w.xz / PUDDLE_SIZE));
    }
    let ground = albedo * (1 - WET_DARKEN * scene.weather.x) * light + scene.sun_color.rgb * glint;
    // Snow takes the light's brightness, not the baked lightmap's tint.
    return mix(ground, vec3f(0.9, 0.92, 0.96) * max(dot(light, luma) * 1.4, 0.3), snow);
}
fn fogged(rgb: vec3f, w: vec3f, additive: bool) -> vec3f {
    if (scene.remaster.y > 0) { return remastered_sky(rgb, w, additive); }
    if (scene.fog.z <= 0 || scene.material.z != 0) { return rgb; }
    let d = dot(w - scene.eye.xyz, scene.view_dir.xyz);
    var f = clamp((scene.fog.y - d) / max(scene.fog.y - scene.fog.x, 0.001), 0, 1);
    if (scene.fog.w != 0) { f = exp(-d * scene.view_dir.w); }
    if (scene.remaster.x > 0) { return remastered_fog(rgb, w, additive, f); }
    if (additive) { return rgb*f; }
    return mix(scene.fog_color.rgb, rgb, f);
}
// Terrain/material selectors are uniform per draw, but tile layers vary between triangles.
// Explicit gradients keep all samples valid across those nonuniform branches.
// Remastered sun shadow pass: depth seen from the sun; textured materials cut their alpha shapes (foliage).
struct ShadowOut { @builtin(position) p: vec4f, @location(0) uv: vec2f, @location(1) a: f32 }
@vertex fn vs_shadow(v: In) -> ShadowOut {
    var o: ShadowOut;
    o.p = scene.shadow_matrix * (scene.world * vec4f(v.p, 1));
    o.uv = v.uv; o.a = v.c.a;
    return o;
}
@fragment fn ps_shadow(v: ShadowOut) {
    if (scene.options.x > 0.5 && (scene.material.x & 0x10000u) != 0) {
        var a = textureSample(image, image_sampler, v.uv).a;
        if ((scene.material.x & 8u) != 0) { a *= v.a; }
        if (a < 0.5) { discard; }
    }
}
@fragment fn ps_main(v: Out) -> @location(0) vec4f {
    let dx = dpdx(v.uv); let dy = dpdy(v.uv);
    let wx = dpdx(v.w.xz); let wy = dpdy(v.w.xz);
    if (scene.material.y != 0) {
        let flags = scene.material.x;
        var tex = vec4f(1);
        if ((flags & 0x10000u) != 0 && scene.options.x > 0.5) {
            tex = textureSampleGrad(image, image_sampler, v.uv, dx, dy);
        }
        if (scene.options.y > 0.5) { tex = vec4f(vec3f(dot(tex.rgb, vec3f(0.299, 0.587, 0.114))), tex.a); }
        var rgb = v.c.rgb; var alpha = v.c.a;
        if ((flags & 0x10000u) != 0) {
            var diffuse = v.c.rgb;
            if ((flags & 4u) != 0) { diffuse = v.c.aaa; }
            rgb = tex.rgb;
            if ((flags & 1u) != 0) { rgb *= diffuse; }
            else if ((flags & 2u) != 0) { rgb += diffuse; }
            alpha = tex.a;
            if ((flags & 8u) != 0) { alpha *= v.c.a; }
        }
        alpha *= scene.options.w;
        if ((flags & 0x20000000u) == 0 && alpha < 1.0/255.0) { discard; }
        if (scene.options.z > 0) {
            let size = textureDimensions(scene_depth);
            let at = min(vec2u(v.p.xy), size - 1);
            let fade = clamp((scene_view_z(textureLoad(scene_depth, at, 0)) - scene_view_z(v.p.z)) / SOFT_PARTICLE_RANGE, 0, 1);
            alpha *= fade;
            if ((flags & 0x7f00u) == 0x400u) { rgb *= fade; } // Darkening blends by colour, not alpha.
        }
        if ((flags & 1u) != 0 && scene.material.z == 0 && scene.material.w == 0) {
            if (scene.remaster.x > 0 && scene.sun_color.w > 0) { rgb *= remastered_light(v.n, v.point_only, v.w); }
            else { rgb *= v.light; }
        }
        if (is_foliage(v.tile)) { rgb = foliage(rgb, v.w, v.tile, v.detail); }
        return vec4f(fogged(rgb, v.w, (flags & 0x7f00u) == 0x2000u), alpha);
    }
    var layer = 7u;
    if ((v.tile & 0x80000000u) != 0) { layer = v.tile & 7u; }
    var base: vec4f;
    if (layer == 7u) {
        base = textureSampleGrad(terrain_tiles, image_sampler, v.uv, i32(v.tile & 0xffffu), dx, dy);
    } else if (layer == 6u) {
        base = textureSampleGrad(terrain_edge, image_sampler, v.uv, dx, dy);
    } else {
        let window = scene.windows[layer];
        base = textureSampleGrad(terrain_windows, image_sampler, (v.w.xz-window.xy)*window.z, i32(layer), wx*window.z, wy*window.z);
    }
    var rgb = fogged(base.rgb * clamp(v.c.rgb+v.light,vec3f(0),vec3f(1)), v.w, false);
    if (layer == 7u && scene.options.w > 0) {
        let tile = v.tile >> 16u;
        let slot = tile_details.slots[tile >> 2u][tile & 3u];
        let dt = textureSampleGrad(terrain_details, image_sampler, v.uv, i32(slot), dx, dy);
        var k = 0.0;
        if (dt.b != 0) {
            // Bump detail maps when bump.w is set, else the detail texture's intensity.
            k = select(dt.a, 1-clamp(4*dot(dt.rgb-vec3f(0.5),scene.bump.rgb-vec3f(0.5)),0,1), scene.bump.w > 0);
        }
        rgb *= 1-k*v.detail;
    }
    return vec4f(rgb,base.a);
}
