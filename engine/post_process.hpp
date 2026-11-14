#pragma once
// Remastered look (a modern addition, the graphics page's "Remastered Look"): the scene draws into a
// half-float target, then bloom, a per-world grade and tone mapping run before the HUD. With the option off,
// the scene draws straight to the screen as the original did. Both backends use these passes and values.
#include "remaster_tuning.hpp"
#include "sacrifice/renderer.hpp"
#include "sacrifice/scene.hpp"
#include <algorithm>
#include <cmath>

namespace sacrifice::post {
// One 80-byte constant block per pass.
struct Constants {
    float texel[4]{};       // Source texel size (xy); bloom threshold (z) and soft knee (w), linear.
    float bloom[4]{};       // Strength (x), pass (y: 0 first downsample, 1 downsample, 2 upsample),
                            // saturation (z), contrast (w).
    float shadow_tint[4]{}; // Multiplier for dark tones (rgb); vignette (w).
    float light_tint[4]{};  // Multiplier for bright tones (rgb); exposure (w).
    float occlusion[4]{};   // Ambient occlusion: view tangents (xy), world radius (z), strength (w).
};
static_assert(sizeof(Constants) == 80);

constexpr unsigned max_levels = 6;
// Bloom mip chain from half resolution down, stopping before a side drops under 8 pixels.
inline unsigned bloom_levels(unsigned width, unsigned height) {
    unsigned levels = 0;
    for (unsigned w = width / 2, h = height / 2; levels < max_levels && w >= 8 && h >= 8; w /= 2, h /= 2)
        ++levels;
    return levels;
}
inline unsigned level_size(unsigned size, unsigned level) { return std::max(1u, size >> (level + 1)); }

// The composite's look. Each world keeps its identity: dark tones lean towards the ENVI ambient hue and
// bright tones towards the sun's.
inline Constants look(const Environment* environment, unsigned levels, unsigned width, unsigned height,
                      const WeatherLook& weather = {}) {
    Constants c;
    c.texel[0] = 1.0f / width; // For the occlusion blur.
    c.texel[1] = 1.0f / height;
    c.bloom[0] = levels ? tuning::bloom_strength / levels : 0; // Spread over the summed levels.
    c.bloom[1] = 3;
    // Falling ash drains colour and dims the light; a lightning flash brightens the whole view.
    c.bloom[2] = tuning::saturation * (1 - weather.ash * tuning::ash_desaturate) *
                 (1 - weather.storm * tuning::storm_desaturate);
    c.bloom[3] = tuning::contrast;
    c.shadow_tint[3] = tuning::vignette;
    c.light_tint[3] = tuning::exposure * (1 - weather.ash * tuning::ash_dim - weather.storm * tuning::storm_dim +
                                          weather.flash * tuning::lightning_flash);
    const auto tint = [](Vec3 color, float amount, float* out) {
        const float luma = color.x * .2126f + color.y * .7152f + color.z * .0722f;
        const Vec3 hue = luma > 1e-4f ? Vec3{color.x / luma, color.y / luma, color.z / luma} : Vec3{1, 1, 1};
        out[0] = 1 + (std::clamp(hue.x, .5f, 1.6f) - 1) * amount;
        out[1] = 1 + (std::clamp(hue.y, .5f, 1.6f) - 1) * amount;
        out[2] = 1 + (std::clamp(hue.z, .5f, 1.6f) - 1) * amount;
    };
    if (environment) {
        tint(environment->ambient, tuning::shadow_tint, c.shadow_tint);
        tint(environment->sun_color, tuning::light_tint, c.light_tint);
    } else {
        tint({1, 1, 1}, 0, c.shadow_tint);
        tint({1, 1, 1}, 0, c.light_tint);
    }
    return c;
}
inline Constants downsample(unsigned source_width, unsigned source_height, bool first) {
    Constants c;
    c.texel[0] = 1.0f / source_width;
    c.texel[1] = 1.0f / source_height;
    c.texel[2] = tuning::bloom_threshold;
    c.texel[3] = tuning::bloom_knee;
    c.bloom[1] = first ? 0.f : 1.f;
    return c;
}
// Screen-space ambient occlusion from the reversed-Z depth (near .1, far 30000): twelve samples within a
// world radius, rotated in a 4x4 pattern that the composite's 4x4 box blur removes.
inline Constants occlusion(unsigned width, unsigned height, float tan_width, float tan_height) {
    Constants c;
    c.texel[0] = 1.0f / width;
    c.texel[1] = 1.0f / height;
    c.occlusion[0] = tan_width;
    c.occlusion[1] = tan_height;
    c.occlusion[2] = tuning::ao_radius;
    c.occlusion[3] = tuning::ao_strength;
    return c;
}
inline Constants upsample(unsigned source_width, unsigned source_height) {
    Constants c;
    c.texel[0] = 1.0f / source_width;
    c.texel[1] = 1.0f / source_height;
    c.bloom[1] = 2;
    return c;
}

// The scene target holds the original's gamma-encoded colors, now free to exceed 1 where additive effects
// stack. Bloom works in linear light; the composite applies the Khronos PBR Neutral tone map, which leaves
// colors below about 0.76 alone and rolls highlights off, then the grade and the vignette.
inline constexpr char hlsl[] = R"(
cbuffer Post:register(b0){float4 texel;float4 bloom;float4 shadow_tint;float4 light_tint;float4 occlusion;};
Texture2D source:register(t0);Texture2D glow:register(t1);SamplerState linear_clamp:register(s0);
Texture2D<float> depth_map:register(t2);Texture2D occluded:register(t3);
struct V{float4 p:SV_POSITION;float2 uv:TEXCOORD;};
V vs_post(uint id:SV_VertexID){V o;o.uv=float2((id<<1)&2,id&2);o.p=float4(o.uv.x*2-1,1-o.uv.y*2,0,1);return o;}
float3 to_linear(float3 c){return pow(max(c,0),2.2);}
float peak(float3 c){return max(c.r,max(c.g,c.b));}
float3 tap(float2 uv){float3 c=source.SampleLevel(linear_clamp,uv,0).rgb;
 if(bloom.y<0.5){c=to_linear(c);float b=peak(c);float s=clamp(b-texel.z+texel.w,0,2*texel.w);s=s*s/(4*texel.w+1e-5);
  c*=max(s,b-texel.z)/max(b,1e-5);c/=1+peak(c);}
 return c;}
float4 ps_down(V v):SV_TARGET{float2 d=texel.xy;
 float3 c=tap(v.uv+float2(-d.x,-d.y))+tap(v.uv+float2(d.x,-d.y))+tap(v.uv+float2(-d.x,d.y))+tap(v.uv+d);
 c*=0.25;if(bloom.y<0.5)c/=max(1-peak(c),0.05);return float4(c,1);}
float4 ps_up(V v):SV_TARGET{float2 d=texel.xy;float3 c=0;
 [unroll]for(int y=-1;y<=1;++y)[unroll]for(int x=-1;x<=1;++x)
  c+=source.SampleLevel(linear_clamp,v.uv+float2(x,y)*d,0).rgb*((2-abs(x))*(2-abs(y)));
 return float4(c/16,1);}
float view_z(float d){return 3000/(0.1+d*29999.9);}
float3 view_pos(int2 p,int2 size){p=clamp(p,0,size-1);float2 uv=(p+0.5)*texel.xy;float z=view_z(depth_map.Load(int3(p,0)));
 return float3((uv.x*2-1)*occlusion.x*z,(1-uv.y*2)*occlusion.y*z,z);}
float4 ps_ao(V v):SV_TARGET{int2 size=int2(round(1/texel.xy));int2 p=int2(v.p.xy);
 if(depth_map.Load(int3(p,0))<=0)return 1;
 float3 P=view_pos(p,size);float3 N=normalize(cross(view_pos(p+int2(1,0),size)-P,view_pos(p+int2(0,1),size)-P));
 if(dot(N,P)>0)N=-N;
 float spin=((p.x&3)+4*(p.y&3))/16.0;float pixels=clamp(occlusion.z/(P.z*occlusion.y*2*texel.y),2,64);float occ=0;
 [loop]for(int i=0;i<12;++i){float a=(i+spin)*2.39996;float r=sqrt((i+0.5)/12)*pixels;
  float3 d=view_pos(p+int2(cos(a)*r,sin(a)*r),size)-P;float l2=dot(d,d);
  occ+=saturate(dot(d,N)*rsqrt(l2+1e-4)-AO_BIAS)*saturate(1-l2/(occlusion.z*occlusion.z));}
 return saturate(1-occ/12*occlusion.w).xxxx;}
float3 neutral(float3 c){float x=min(c.r,min(c.g,c.b));float o=x<0.08?x-6.25*x*x:0.04;c-=o;float p=peak(c);
 if(p<0.76)return c;float n=1-0.0576/(p-0.52);c*=n/p;float g=1-1/(0.15*(p-n)+1);return lerp(c,n.xxx,g);}
float4 ps_composite(V v):SV_TARGET{
 float ao=0;[unroll]for(int j=0;j<4;++j)ao+=occluded.SampleLevel(linear_clamp,v.uv+float2(j&1?0.5:-1.5,j&2?0.5:-1.5)*texel.xy,0).r;
 float3 c=to_linear(source.SampleLevel(linear_clamp,v.uv,0).rgb)*(ao*0.25)+glow.SampleLevel(linear_clamp,v.uv,0).rgb*bloom.x;
 c*=light_tint.w;float l=dot(c,float3(0.2126,0.7152,0.0722));
 c*=lerp(shadow_tint.rgb,light_tint.rgb,smoothstep(0,1,l));
 c=neutral(c);l=dot(c,float3(0.2126,0.7152,0.0722));c=max(lerp(l.xxx,c,bloom.z),0);
 float3 g=pow(c,1/2.2);g=lerp(g,g*g*(3-2*g),bloom.w);
 float2 q=v.uv-0.5;g*=1-shadow_tint.w*dot(q,q)*2;return float4(saturate(g),1);}
)";

inline constexpr char wgsl[] = R"(
struct Post { texel: vec4f, bloom: vec4f, shadow_tint: vec4f, light_tint: vec4f, occlusion: vec4f }
@group(0) @binding(0) var<uniform> post: Post;
@group(0) @binding(1) var linear_clamp: sampler;
@group(0) @binding(2) var source: texture_2d<f32>;
@group(0) @binding(3) var glow: texture_2d<f32>;
@group(0) @binding(4) var depth_map: texture_depth_2d;
@group(0) @binding(5) var occluded: texture_2d<f32>;
struct V { @builtin(position) p: vec4f, @location(0) uv: vec2f }
@vertex fn vs_post(@builtin(vertex_index) id: u32) -> V {
    var o: V; o.uv = vec2f(f32((id << 1u) & 2u), f32(id & 2u));
    o.p = vec4f(o.uv.x * 2 - 1, 1 - o.uv.y * 2, 0, 1); return o;
}
fn to_linear(c: vec3f) -> vec3f { return pow(max(c, vec3f(0)), vec3f(2.2)); }
fn peak(c: vec3f) -> f32 { return max(c.r, max(c.g, c.b)); }
fn tap(uv: vec2f) -> vec3f {
    var c = textureSampleLevel(source, linear_clamp, uv, 0).rgb;
    if (post.bloom.y < 0.5) {
        c = to_linear(c); let b = peak(c);
        var s = clamp(b - post.texel.z + post.texel.w, 0, 2 * post.texel.w); s = s * s / (4 * post.texel.w + 1e-5);
        c *= max(s, b - post.texel.z) / max(b, 1e-5); c /= 1 + peak(c);
    }
    return c;
}
@fragment fn ps_down(v: V) -> @location(0) vec4f {
    let d = post.texel.xy;
    var c = tap(v.uv + vec2f(-d.x, -d.y)) + tap(v.uv + vec2f(d.x, -d.y)) + tap(v.uv + vec2f(-d.x, d.y)) + tap(v.uv + d);
    c *= 0.25;
    if (post.bloom.y < 0.5) { c /= max(1 - peak(c), 0.05); }
    return vec4f(c, 1);
}
@fragment fn ps_up(v: V) -> @location(0) vec4f {
    let d = post.texel.xy; var c = vec3f(0);
    for (var y = -1; y <= 1; y++) {
        for (var x = -1; x <= 1; x++) {
            c += textureSampleLevel(source, linear_clamp, v.uv + vec2f(f32(x), f32(y)) * d, 0).rgb *
                 f32((2 - abs(x)) * (2 - abs(y)));
        }
    }
    return vec4f(c / 16, 1);
}
fn view_z(d: f32) -> f32 { return 3000 / (0.1 + d * 29999.9); }
fn view_pos(q: vec2i, size: vec2i) -> vec3f {
    let p = clamp(q, vec2i(0), size - 1); let uv = (vec2f(p) + 0.5) * post.texel.xy;
    let z = view_z(textureLoad(depth_map, p, 0));
    return vec3f((uv.x * 2 - 1) * post.occlusion.x * z, (1 - uv.y * 2) * post.occlusion.y * z, z);
}
@fragment fn ps_ao(v: V) -> @location(0) vec4f {
    let size = vec2i(round(1 / post.texel.xy)); let p = vec2i(v.p.xy);
    if (textureLoad(depth_map, p, 0) <= 0) { return vec4f(1); }
    let P = view_pos(p, size);
    var N = normalize(cross(view_pos(p + vec2i(1, 0), size) - P, view_pos(p + vec2i(0, 1), size) - P));
    if (dot(N, P) > 0) { N = -N; }
    let spin = f32((p.x & 3) + 4 * (p.y & 3)) / 16;
    let pixels = clamp(post.occlusion.z / (P.z * post.occlusion.y * 2 * post.texel.y), 2, 64);
    var occ = 0.0;
    for (var i = 0; i < 12; i++) {
        let a = (f32(i) + spin) * 2.39996; let r = sqrt((f32(i) + 0.5) / 12) * pixels;
        let d = view_pos(p + vec2i(i32(cos(a) * r), i32(sin(a) * r)), size) - P; let l2 = dot(d, d);
        occ += clamp(dot(d, N) * inverseSqrt(l2 + 1e-4) - AO_BIAS, 0, 1) *
               clamp(1 - l2 / (post.occlusion.z * post.occlusion.z), 0, 1);
    }
    return vec4f(vec3f(clamp(1 - occ / 12 * post.occlusion.w, 0, 1)), 1);
}
fn neutral(color: vec3f) -> vec3f {
    var c = color; let x = min(c.r, min(c.g, c.b));
    let o = select(0.04, x - 6.25 * x * x, x < 0.08); c -= vec3f(o);
    let p = peak(c); if (p < 0.76) { return c; }
    let n = 1 - 0.0576 / (p - 0.52); c *= n / p;
    let g = 1 - 1 / (0.15 * (p - n) + 1); return mix(c, vec3f(n), g);
}
@fragment fn ps_composite(v: V) -> @location(0) vec4f {
    var ao = 0.0;
    for (var j = 0; j < 4; j++) {
        let o = vec2f(select(-1.5, 0.5, (j & 1) != 0), select(-1.5, 0.5, (j & 2) != 0));
        ao += textureSampleLevel(occluded, linear_clamp, v.uv + o * post.texel.xy, 0).r;
    }
    var c = to_linear(textureSampleLevel(source, linear_clamp, v.uv, 0).rgb) * (ao * 0.25) +
            textureSampleLevel(glow, linear_clamp, v.uv, 0).rgb * post.bloom.x;
    c *= post.light_tint.w;
    var l = dot(c, vec3f(0.2126, 0.7152, 0.0722));
    c *= mix(post.shadow_tint.rgb, post.light_tint.rgb, smoothstep(0, 1, l));
    c = neutral(c); l = dot(c, vec3f(0.2126, 0.7152, 0.0722));
    c = max(mix(vec3f(l), c, post.bloom.z), vec3f(0));
    var g = pow(c, vec3f(1 / 2.2)); g = mix(g, g * g * (3 - 2 * g), post.bloom.w);
    let q = v.uv - 0.5; g *= 1 - post.shadow_tint.w * dot(q, q) * 2;
    return vec4f(clamp(g, vec3f(0), vec3f(1)), 1);
}
)";
} // namespace sacrifice::post
