#pragma once
// Remastered Look tuning: every adjustable value in one place. Edit a value and rebuild; both the desktop
// and the browser builds read these. The shader values are added to the HLSL and WGSL sources as constants
// when the renderers start (see shader_constants below). Each comment gives a sensible range as [min, max]
// and what raising the value does. Nothing here affects the simulation.
#include <array>
#include <format>
#include <string>
#include <utility>

namespace sacrifice::tuning {
// ---- Bloom (glow around bright light) ----
inline constexpr float bloom_strength = .16f;   // [0, 1]     Overall glow amount.
inline constexpr float bloom_threshold = 1.05f; // [.5, 2]    Brightness (linear) where glow starts.
inline constexpr float bloom_knee = .45f;       // [0, 1]     Softness of the start of the glow.

// ---- Colour grade and tone ----
inline constexpr float exposure = 1.05f;        // [.5, 2]    Overall brightness before tone mapping.
inline constexpr float saturation = 1.0f;       // [0, 2]     1 keeps the original colour intensity.
inline constexpr float contrast = .12f;         // [0, 1]     Blend towards an S-curve.
inline constexpr float vignette = .22f;         // [0, 1]     Darkening at the screen corners.
inline constexpr float shadow_tint = .12f;      // [0, .5]    Dark tones lean to the world's ambient hue.
inline constexpr float light_tint = .08f;       // [0, .5]    Bright tones lean to the world's sun hue.

// ---- Ambient occlusion (contact shading) ----
inline constexpr float ao_radius = 3.0f;        // [.5, 10]   World radius searched for occluders.
inline constexpr float ao_strength = 1.2f;      // [0, 4]     Darkening amount.
inline constexpr float ao_bias = .15f;          // [0, .5]    Ignores nearly flat surroundings (less noise).

// ---- Fog, mist and sky ----
inline constexpr float fog_ease = 3.0f;         // [1, 6]     Fog falloff over the map's fog range; higher
                                                //            clears near ground and thickens far ground.
inline constexpr float mist_strength = .45f;    // [0, 1]     Most low-ground mist can hide.
inline constexpr float mist_density = 1.4f;     // [0, 5]     How quickly the mist builds with distance.
inline constexpr float mist_floor = 1.0f / 6;   // [0, 1]     Mist floor as a fraction of the map's height range.
inline constexpr float mist_falloff = .2f;      // [.05, 1]   Mist thinning height, fraction of the range.
inline constexpr float fog_sun_tint = .45f;     // [0, 1.5]   Sun colour in fog when looking towards the sun.
inline constexpr float fog_sun_focus = 8.0f;    // [1, 64]    Narrowness of that sun-lit fog.
inline constexpr float horizon_haze = .35f;     // [0, 1]     Sky fades into the fog colour at the horizon.
inline constexpr float horizon_focus = 6.0f;    // [1, 16]    Narrowness of the horizon haze band.
inline constexpr float sun_core = .4f;          // [0, 2]     Bright glow right around the sun.
inline constexpr float sun_core_focus = 48.0f;  // [8, 512]   Size of that glow (higher is smaller).
inline constexpr float sun_halo = .12f;         // [0, 1]     Wide halo around the sun.
inline constexpr float sun_halo_focus = 6.0f;   // [1, 32]    Size of the halo (higher is smaller).

// ---- Soft particles ----
inline constexpr float soft_particle_range = 1.5f; // [.1, 10] World distance over which smoke, glows and
                                                   //          spell sprites fade into the surface behind.

// ---- Sun shadows ----
inline constexpr float shadow_strength = .6f;   // [0, 1]     Sunlight removed from models in shadow.
inline constexpr float shadow_ground = .5f;     // [0, .9]    Ground darkening in shadow.
inline constexpr float ground_brightness = 1.2f; // [.5, 2]   Overall ground (terrain) lighting.
inline constexpr float bump_relief = .45f;      // [0, 1]     Strength of the bump detail relief (lit from the
                                                //            sun), times the original's. Flat ground is unchanged.
inline constexpr float terrain_shadows = 0;     // [0, 1]     1: hills cast sun shadows too. Off, since the
                                                //            original's ground lighting already shades them and
                                                //            low suns stretch them into hard, faceted shapes.
inline constexpr float shadow_range = 160.0f;   // [30, 400]  Half-width of the shadowed square around the view;
                                                //            larger reaches further but blurs the shadows.
inline constexpr float shadow_map_size = 2048;  // [512, 4096] Shadow map resolution (texels per side).
inline constexpr float shadow_bias = .15f;      // [0, 2]     World distance that hides self-shadow speckle;
                                                //            too high detaches shadows from their casters.
inline constexpr float shadow_softness = 1.0f;  // [.5, 4]    Shadow edge blur, in shadow-map texels.
inline constexpr float shadow_min_elevation = .35f; // [0, 1] Lowest sun height used, so shadows of a low
                                                    //        sun do not stretch across the map.

// ---- Model lighting ----
inline constexpr float light_wrap = .25f;       // [0, 1]     Softens the lit/shadow edge on models.
inline constexpr float sky_ambient_base = 1.0f; // [0, 1.5]   Ambient on downward-facing surfaces.
inline constexpr float sky_ambient_up = .35f;   // [0, 1]     Extra ambient on upward-facing surfaces.
inline constexpr float rim_light = .35f;        // [0, 1.5]   Sunlit rim on edges away from the sun.
inline constexpr float light_max = 1.6f;        // [1, 3]     Brightest a model can be lit.
inline constexpr float model_brightness = 1.15f; // [.5, 2]   Overall model (creature, building) lighting.

// ---- Weather ----
inline constexpr float weather_density = 2.0f;  // [1, 4]     Drops drawn per simulated drop (drawing only).
inline constexpr float rain_opacity = 2.2f;     // [1, 8]     Rain streak opacity, times the original's.
inline constexpr float rain_width = 1.4f;       // [1, 6]     Rain streak width, times the original's.
inline constexpr float rain_length = 1.2f;      // [.5, 4]    Rain streak length, times the original's.
inline constexpr float wet_darken = .3f;        // [0, .6]    Ground darkening when fully wet.
inline constexpr float wet_shine = .5f;         // [0, 2]     Sun glint on wet ground (puddles), once the sky clears.
inline constexpr float wet_gloss = 140.0f;      // [4, 512]   Tightness of the glint (higher is smaller, sharper).
inline constexpr float puddle_size = 9.0f;      // [2, 40]    World size of the wet patches that catch the glint.
inline constexpr float puddle_cover = .35f;     // [0, 1]     Share of the wet ground that catches it.
inline constexpr float wet_rise_seconds = 20.0f;   // [1, 120]  Time for heavy rain to soak the ground.
inline constexpr float wet_dry_seconds = 60.0f;    // [1, 600]  Time for the ground to dry.
inline constexpr float snow_cover = .85f;       // [0, 1]     Most ground snow can cover.
inline constexpr float snow_rise_seconds = 90.0f;  // [1, 600]  Time for heavy snow to settle.
inline constexpr float snow_melt_seconds = 120.0f; // [1, 600]  Time for settled snow to melt.
inline constexpr float ember_size = 1.0f;       // [.5, 4]    Ember size, times the original's.
inline constexpr float ember_halo = .3f;        // [0, 1]     Opacity of the soft glow drawn around each ember.
inline constexpr float ember_halo_size = 2.2f;  // [1, 10]    Size of that glow, times the ember's.
inline constexpr float ash_desaturate = .35f;   // [0, 1]     Colour drained while ash falls.
inline constexpr float ash_dim = .15f;          // [0, .5]    Light dimmed while ash falls.
inline constexpr float lightning_flash = .15f;  // [0, 3]     Extra exposure (everywhere) during a lightning flash.
inline constexpr float lightning_light = 1.5f;  // [0, 4]     Extra light on sunlit surfaces during a flash; it
                                                //            lights the scene from the sun's side, casting shadows.
inline constexpr float lightning_shadow = 1.5f; // [0, 2]     How far a flash brings back the shadows a storm faded.
inline constexpr float terrain_detail_range = 220.0f; // [88, 400] Distance the land keeps full-detail
                                                     //  cells (the original's 88, shrunk while moving).
inline constexpr float storm_shadow_fade = .85f; // [0, 1]    Shadows faded under heavy rain or hail (overcast).
inline constexpr float storm_sun_dim = .35f;    // [0, 1]     Direct sunlight on models dimmed under heavy rain or hail.
inline constexpr float storm_dim = .2f;         // [0, .6]    Light dimmed under heavy rain or hail.
inline constexpr float storm_desaturate = .2f;  // [0, 1]     Colour drained under heavy rain or hail.
inline constexpr float storm_sky_darken = .45f; // [0, .9]    Sky darkening under heavy rain or hail.
inline constexpr float storm_fog_darken = .35f; // [0, .9]    Fog darkening under heavy rain or hail.
inline constexpr float rain_fog_distance = 400.0f; // [100, 3000] Rain mist: distance it hides about two thirds
                                                   //  of the view at full rain or hail (lower is thicker).
inline constexpr float rain_fog_light = .4f;       // [0, 2]    Rain mist brightness, times the scene's light
                                                   //           (rain scatters light, so it is paler than the land).
inline constexpr float rain_fog_start = 20.0f;     // [0, 200]  Clear distance around the camera before it starts.
inline constexpr float rain_sky_haze = .9f;        // [0, 1]    Horizon hidden in the rain mist, so far land melts
                                                   //           into the sky.
inline constexpr float storm_fog_grey = .6f;       // [0, 1]    Colour drained from fog and sky under rain or hail.
inline constexpr float storm_cloud_flash = 1.6f;   // [0, 4]   Sky brightening during a lightning flash, at its
                                                   //          brightest on the sun's side (where the flash lights from).
inline constexpr float lightning_sky_focus = 3.0f; // [1, 12]  How tightly that glow gathers on the sun's side.
inline constexpr float lightning_sky_ambient = .08f; // [0, .5] Faint flicker over the whole sky during a flash.
inline constexpr float lightning_fog = .45f;       // [0, 2]   Distant fog lit by the flash on the sun's side.
inline constexpr float splash_share = 1.0f;     // [0, 1]     Share of the drawn raindrops that splash where they
                                                //            hit the ground (within 50 of the camera).
inline constexpr float splash_count = 160.0f;   // [0, 1000]  Far splashes (beyond the drops) per circle of
                                                //            splash_radius, at full rain.
inline constexpr float splash_radius = 65.0f;   // [5, 120]   Radius of the far splash area ahead of the camera.
inline constexpr float splash_size = .45f;      // [.1, 2]    Widest a splash ring grows.
inline constexpr float splash_seconds = .4f;    // [.1, 2]    Life of one splash.
inline constexpr float splash_opacity = .8f;    // [0, 1]     Splash opacity at its start.

// Vegetation (plants, trees and the grass layer).
inline constexpr float wind_strength = .35f;    // [0, 1]     Calm-weather wind: how much plants sway.
inline constexpr float wind_storm = .65f;       // [0, 2]     Wind added under heavy rain or hail.
inline constexpr float wind_bend = .14f;        // [0, .5]    Sway at full wind, per unit of plant height (tapered).
inline constexpr float foliage_sun = .25f;      // [0, 1]     Extra sunlight on sunlit plants (shadowed ones miss it).
inline constexpr float foliage_backlight = .35f; // [0, 1.5]  Glow of leaves and grass looking towards the sun.
inline constexpr float foliage_sheen = .18f;    // [0, 1]     Sunlit specular sheen on curved blades and leaves.
inline constexpr float foliage_tint = .12f;     // [0, .4]    Per-plant colour variation (hue and brightness).
inline constexpr float foliage_root = .55f;     // [0, 1]     Brightness at a plant's root (darker in its own shade).
inline constexpr float foliage_root_height = 1.5f; // [.1, 10] Height over which that root shade lifts.
inline constexpr float widget_min_range = 100.0f; // [20, 200] Shortest plant draw range (before the scale),
                                                 //           the same for every plant kind and view detail.
inline constexpr float widget_range_scale = 1.5f; // [1, 3]   Plant draw range, times the original's.
inline constexpr float widget_copies = 1.0f;    // [0, 6]     Extra drawing-only copies of each small plant.
inline constexpr float widget_copy_max_height = 3.0f; // [0, 20] Tallest plant model that gets copies (not trees).
inline constexpr float widget_copy_spread = 2.5f; // [.5, 10] How far from its plant a copy is placed.
inline constexpr float grass_density = 1.5f;    // [0, 4]     Grass tufts (three blades each) per square unit on the
                                                //            lushest ground.
inline constexpr float grass_range = 150.0f;    // [20, 200]  Distance the grass layer reaches (thinned like the plants,
                                                //            by flora_thin_start and flora_far_keep).
inline constexpr float grass_height = .85f;     // [.2, 2]    Average blade height.
inline constexpr float clutter_density = .06f;  // [0, .5]    Ground clutter (flowers, mushrooms, pebbles, twigs)
                                                //            per square unit, within the grass range.
inline constexpr float clutter_size = 1.8f;     // [.3, 3]    Clutter scale (about one unit across).
inline constexpr float grass_width = .18f;      // [.02, .4]  Average blade width at the root.
// Distance detail for the new plants (fractions of their draw range), as open-world games thin foliage:
inline constexpr float flora_thin_start = .4f;  // [0, 1]     Plants start thinning out (shrinking away) here.
inline constexpr float flora_far_keep = .4f;    // [0, 1]     Share of plants still drawn at the range's edge.
inline constexpr float flora_lod_start = .3f;   // [0, 1]     Plants switch to their low-detail models here,
inline constexpr float flora_lod_band = .1f;    // [.01, .5]  dissolving from one to the other over this band.
inline constexpr float flora_shadows = 0.0f;    // [0, 1]     1: the new plants cast sun shadows. Off: thin blades are
                                                //            narrower than a shadow-map texel, so they barely showed.
inline constexpr float foliage_fill = .5f;      // [0, 1]     Thinned plants and grass grow up to this much wider
                                                //            (plants: bigger), so thinning keeps the coverage.

// The shader-side values, as constants named in upper case in both shading languages.
inline std::string shader_constants(bool wgsl) {
    static constexpr std::array<std::pair<const char*, float>, 55> values{{
        {"FOLIAGE_FILL", foliage_fill},
        {"FLORA_THIN_START", flora_thin_start},
        {"FLORA_FAR_KEEP", flora_far_keep},
        {"AO_BIAS", ao_bias},
        {"FOG_EASE", fog_ease},
        {"MIST_STRENGTH", mist_strength},
        {"MIST_DENSITY", mist_density},
        {"FOG_SUN_TINT", fog_sun_tint},
        {"FOG_SUN_FOCUS", fog_sun_focus},
        {"HORIZON_HAZE", horizon_haze},
        {"HORIZON_FOCUS", horizon_focus},
        {"SUN_CORE", sun_core},
        {"SUN_CORE_FOCUS", sun_core_focus},
        {"SUN_HALO", sun_halo},
        {"SUN_HALO_FOCUS", sun_halo_focus},
        {"LIGHT_WRAP", light_wrap},
        {"SKY_AMBIENT_BASE", sky_ambient_base},
        {"SKY_AMBIENT_UP", sky_ambient_up},
        {"RIM_LIGHT", rim_light},
        {"LIGHT_MAX", light_max},
        {"MODEL_BRIGHTNESS", model_brightness},
        {"WET_DARKEN", wet_darken},
        {"WET_SHINE", wet_shine},
        {"WET_GLOSS", wet_gloss},
        {"PUDDLE_SIZE", puddle_size},
        {"PUDDLE_COVER", puddle_cover},
        {"SNOW_COVER", snow_cover},
        {"ASH_DESATURATE", ash_desaturate},
        {"ASH_DIM", ash_dim},
        {"STORM_SKY_DARKEN", storm_sky_darken},
        {"STORM_FOG_DARKEN", storm_fog_darken},
        {"STORM_CLOUD_FLASH", storm_cloud_flash},
        {"RAIN_FOG_DISTANCE", rain_fog_distance},
        {"RAIN_FOG_START", rain_fog_start},
        {"RAIN_FOG_LIGHT", rain_fog_light},
        {"RAIN_SKY_HAZE", rain_sky_haze},
        {"STORM_FOG_GREY", storm_fog_grey},
        {"LIGHTNING_SKY_FOCUS", lightning_sky_focus},
        {"LIGHTNING_SKY_AMBIENT", lightning_sky_ambient},
        {"LIGHTNING_FOG", lightning_fog},
        {"SHADOW_MIN_ELEVATION", shadow_min_elevation},
        {"SOFT_PARTICLE_RANGE", soft_particle_range},
        {"SHADOW_GROUND", shadow_ground},
        {"GROUND_BRIGHTNESS", ground_brightness},
        {"LIGHTNING_LIGHT", lightning_light},
        {"LIGHTNING_SHADOW", lightning_shadow},
        {"STORM_SHADOW_FADE", storm_shadow_fade},
        {"STORM_SUN_DIM", storm_sun_dim},
        {"WIND_BEND", wind_bend},
        {"FOLIAGE_SUN", foliage_sun},
        {"FOLIAGE_BACKLIGHT", foliage_backlight},
        {"FOLIAGE_SHEEN", foliage_sheen},
        {"FOLIAGE_TINT", foliage_tint},
        {"FOLIAGE_ROOT", foliage_root},
        {"FOLIAGE_ROOT_HEIGHT", foliage_root_height},
    }};
    std::string text;
    for (const auto& [name, value] : values)
        text += wgsl ? std::format("const {}: f32 = {:.6f};\n", name, value)
                     : std::format("static const float {}={:.6f};\n", name, value);
    return text;
}
} // namespace sacrifice::tuning
