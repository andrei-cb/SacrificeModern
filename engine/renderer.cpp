#include "sacrifice/renderer.hpp"
#include "post_process.hpp"
#include <array>
#include <optional>
#include <chrono>
#include "renderer_shared.hpp"
#include "sacrifice/widgets.hpp"
#include <DirectXMath.h>
#include <algorithm>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <format>
#include <fstream>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <memory>
#include <wrl/client.h>

namespace sacrifice {
using Microsoft::WRL::ComPtr;
using namespace DirectX;
namespace {
void hr(HRESULT h, std::string_view operation) {
    if (FAILED(h))
        throw Error(std::format("{} failed: HRESULT 0x{:08x}", operation, static_cast<unsigned>(h)));
}
constexpr char shader[] = R"(
cbuffer Scene : register(b0) {float4x4 view_projection;float4x4 world;float4 options;uint4 material;
 float4 eye;float4 fog;float4 fog_color;float4 ambient;float4 sun_direction;float4 sun_color;float4 view_dir;
 float4 bump;float4 detail_light;float4 windows[6];float4 model_reveal;uint4 point_options;float4 remaster;float4 weather;
 float4x4 shadow_matrix;float4 shadow;float4 wind;float4 grass;};
struct PointSource {float4 position_range;float4 color;float4 land_position_range;float4 land_color;};
StructuredBuffer<PointSource> point_sources:register(t5);
float3 point_lighting(float3 position,float3 normal,bool land) {
 float3 total=0;
 [loop] for(uint i=0;i<point_options.x;++i) {
   PointSource light=point_sources[i];
   float4 p=land?light.land_position_range:light.position_range;
   float3 color=land?light.land_color.rgb:light.color.rgb;
   float3 delta=p.xyz-position;float d2=dot(delta,delta);float r2=p.w*p.w;
   if(d2<=r2)total+=color*saturate(dot(normal,delta)*rsqrt(max(d2,1e-12)))/(1+3*d2/r2);
 }
 return total;
}
float3 lighting(float3 n,float3 points) {
 if(sun_color.w>0) return saturate(ambient.rgb+sun_color.rgb*saturate(dot(normalize(n),sun_direction.xyz))+points);
 return saturate((0.4+0.6*saturate(dot(normalize(n),normalize(float3(0.3,1,-0.5))))).xxx+points);
}
// Remastered foliage vertices (widgets.hpp foliage_layer): tile 0x40000000 with the height above the plant's
// root in 1/16 units (bits 8..19) and a per-plant hash (bits 0..7); 0x20000000 when the colour is the
// ground's baked light (the grass layer). Only widget-pass draws carry them.
bool is_foliage(uint tile){return remaster.x>0 && material.w!=0 && (tile&0xC0000000)==0x40000000;}
float foliage_height(uint tile){return ((tile>>8)&0xfff)/16.0;}
// Wind (wind = time, strength, direction X/Z): a lean with slow gusts rolling across the map, plus a sway
// per plant; it grows with height, so roots stay put.
float3 foliage_wind(float3 w,uint tile){
 float t=wind.x;float phase=dot(w.xz,float2(0.13,0.09))+(tile&0xff)*0.0246;
 float gust=0.55+0.45*sin(t*0.6-dot(w.xz,wind.zw)*0.035);
 float sway=sin(t*1.9+phase)*0.35+sin(t*3.3+phase*1.7)*0.15;
 float h=foliage_height(tile);
 float bend=WIND_BEND*pow(h,0.6)*wind.y;
 float3 disp=float3(wind.z,0,wind.w)*bend*(gust+sway)+float3(-wind.w,0,wind.z)*bend*0.3*sin(t*2.7+phase*1.3);
 disp.y=-0.18*dot(disp.xz,disp.xz)/max(h,0.25);
 return disp;
}
struct In {float3 p:POSITION;float3 n:NORMAL;float2 uv:TEXCOORD;float4 c:COLOR;uint tile:LAYER;};
// Remastered static grass (grass = camera X/Z, reach, enabled): the vertex's tuft root is in n, its rank in
// uv.x, and its root-to-tip fraction in uv.y. As TerrainView::grass does on the CPU, tufts thin out past
// FLORA_THIN_START and shrink to their roots as their rank passes the thinning line and at the edge; a shrunk
// tuft is degenerate and draws nothing. Thinned tufts widen by up to FOLIAGE_FILL, so the ground stays covered.
In grass_vertex(In v){float3 root=v.n;float3 rel=v.p-root;float t=length(root.xz-grass.xy)/grass.z;
 float keep=t<FLORA_THIN_START?1.1:1.1-(t-FLORA_THIN_START)/(1-FLORA_THIN_START)*(1.1-FLORA_FAR_KEEP);float g=t>=1?0:min(1,(1-t)*5)*saturate((keep-v.uv.x)*8);
 float fill=1+FOLIAGE_FILL*(1-min(keep,1));
 v.p=root+rel*g*float3(fill,1,fill);uint h=(uint)(((v.tile>>8)&0xfff)*g);v.tile=(v.tile&~0xfff00u)|(h<<8);
 v.n=normalize(float3(rel.x*1.4,max(rel.y,0.25)+0.45,rel.z*1.4));v.uv=float2(0,v.uv.y);return v;}
struct Out {float4 p:SV_POSITION;float3 n:NORMAL;float2 uv:TEXCOORD;float4 c:COLOR;nointerpolation uint tile:LAYER;float3 w:TEXCOORD1;nointerpolation float3 anchor:TEXCOORD2;float detail:TEXCOORD3;float3 points:TEXCOORD4;float3 point_only:TEXCOORD5;};
Out vs_main(In v) {Out o;if(grass.w>0 && is_foliage(v.tile))v=grass_vertex(v);float4 w=mul(float4(v.p,1),world);if(is_foliage(v.tile))w.xyz+=foliage_wind(w.xyz,v.tile);
 o.p=mul(w,view_projection);o.w=w.xyz;o.anchor=v.n;o.n=normalize(mul(v.n,(float3x3)world));o.uv=v.uv;o.c=v.c;o.tile=v.tile;
 // Fixed-function D3D7 lighting is evaluated at vertices, before texture modulation and fog.
 bool land=material.y==0 && options.y>0.5;
 o.points=(material.z==0 && material.w==0)?point_lighting(w.xyz,land && v.tile!=0x80000006?float3(0,1,0):o.n,land):0;
 o.point_only=o.points;
 if(material.y!=0 && material.z==0 && material.w==0)o.points=lighting(o.n,o.points);
 // MRMINSTANCE::FillVertexBuffer 0x1601d5c0: construction reveals from bottom to top, with a
 // max(quarter height,5) fade band and byte-quantized vertex alpha. Collision bounds do not scale.
 if(model_reveal.w>0 && model_reveal.z!=1) {
   float extent=model_reveal.y-model_reveal.x;
   float band=max(extent*0.25,5.0);
   float top=band+model_reveal.y-(band+extent)*saturate(1-model_reveal.z);
   o.c.a=floor(saturate((top-v.p.y)/band)*255.0)/255.0;
 }
 // Detail light, lit per vertex by D3D7 (3d_d3d 0x180124f0): a point light with range and attenuation
 // 1 + 3 d^2 / range^2 from the outer range that sub_1603B080 scaled by 1.25 along with the colour. The near
 // batches (sub_1603A070) give every vertex the normal (0, 0, 1) (up here) and diffuse white; the ambient is 0.
 float3 to_light=detail_light.xyz-w.xyz;float d=length(to_light);float range=options.w*1.25;
 o.detail=d>range?0:saturate(1.25*saturate(to_light.y/max(d,1e-3))/(1+3*d*d/(range*range)));
 if(is_foliage(v.tile))o.detail=foliage_height(v.tile); // Models don't use the detail light.
 return o;}
// Remastered sun shadows: the shadow map (t6) seen from the sun through shadow_matrix; shadow = enabled,
// sampling spread, depth bias, model strength. 3x3 filtered comparisons, fading out at the square's edge.
Texture2D<float> shadow_map:register(t6);SamplerComparisonState shadow_sampler:register(s1);
float sun_visibility(float3 w) {
 if(shadow.x<=0) return 1;
 float4 s=mul(float4(w,1),shadow_matrix);float2 uv=s.xy*float2(0.5,-0.5)+0.5;
 float2 edge=saturate((0.5-abs(uv-0.5))*10);float fade=min(edge.x,edge.y);
 if(fade<=0 || s.z>=1) return 1;
 float lit=0;
 [unroll] for(int y=-1;y<=1;y++) [unroll] for(int x=-1;x<=1;x++)
   lit+=shadow_map.SampleCmpLevelZero(shadow_sampler,uv+float2(x,y)*shadow.y,s.z-shadow.z);
 return lerp(1,lit/9,fade);
}
// Weather on the sun: a storm (weather.z) overcasts it, dimming it and fading its shadows; a lightning
// flash (weather.w) lights the scene from the sun's side, bringing the shadows back for its moment.
float shadow_amount(){return saturate(1-STORM_SHADOW_FADE*weather.z+LIGHTNING_SHADOW*weather.w);}
float3 sun_light(){return sun_color.rgb*(1-STORM_SUN_DIM*weather.z)+float3(0.8,0.87,1)*LIGHTNING_LIGHT*weather.w;}
// A flash seen in the sky along `dir`: a glow gathered on the side it lights from (the sun's, raised to the
// shadow map's lowest elevation, so it agrees with the shadows), over a faint flicker everywhere.
float lightning_glow(float3 dir){
 float3 from=normalize(float3(sun_direction.x,max(sun_direction.y,SHADOW_MIN_ELEVATION),sun_direction.z));
 return weather.w*pow(saturate(0.5+0.5*dot(dir,from)),LIGHTNING_SKY_FOCUS);}
// The fog colour under a storm (weather.z): darker and greyer, matching the storm sky's horizon.
float3 storm_fog_color(){float3 f=fog_color.rgb*(1-STORM_FOG_DARKEN*weather.z);
 return lerp(f,dot(f,float3(0.299,0.587,0.114)).xxx,STORM_FOG_GREY*weather.z);}
// The rain mist's colour: a blue-grey lit by the scene's light (ambient and the storm-dimmed sun), so it
// reads as pale haze on any map, whatever its fog colour.
float3 rain_fog_color(){
 float l=dot(ambient.rgb+sun_color.rgb*(1-STORM_SUN_DIM*weather.z),float3(0.299,0.587,0.114));
 return float3(0.86,0.9,1)*l*RAIN_FOG_LIGHT;}
// Remastered Look (remaster.x): fog that eases in over the map's own range, mist that pools below the
// floor height (remaster.z) with falloff remaster.w, fog lit towards the sun, and rain mist that thickens
// with distance past a clear area around the camera while it rains or hails (weather.z).
float3 remastered_fog(float3 rgb,float3 w,bool additive,float original) {
 float3 ray=w-eye.xyz;float dist=length(ray);float3 dir=ray/max(dist,1e-3);
 float t=max(dist-fog.x,0)/max(fog.y-fog.x,1e-3);
 float visible=fog.w!=0?original:exp(-FOG_EASE*t*t);
 float f=max(remaster.w,1);float x=clamp(dir.y*dist/f,-20,60);
 float g=abs(x)>1e-3?(1-exp(-x))/x:1-0.5*x;
 float mist=min(MIST_DENSITY/max(fog.y,1)*dist*exp(clamp((remaster.z-eye.y)/f,-60,4))*g,40);
 visible*=1-MIST_STRENGTH*(1-exp(-mist));
 float rain=1-exp(-weather.z*max(dist-RAIN_FOG_START,0)/RAIN_FOG_DISTANCE);
 float3 sun=sun_color.rgb*FOG_SUN_TINT*pow(saturate(dot(dir,sun_direction.xyz)),FOG_SUN_FOCUS)*(1-weather.z);
 float3 flash=fog_color.rgb*LIGHTNING_FOG*lightning_glow(dir);
 return additive?rgb*visible*(1-rain):lerp(lerp(storm_fog_color()+sun+flash,rgb,visible),rain_fog_color()+flash,rain);
}
// The sky's layers: haze towards the fog colour at the horizon and a glow around the sun. Storms
// (weather.z) darken and grey the sky and hide the sun; a lightning flash (weather.w) lights the clouds on
// the side it lights the land from.
float3 remastered_sky(float3 rgb,float3 w,bool additive) {
 float3 dir=normalize(w-eye.xyz);float h=pow(1-abs(dir.y),HORIZON_FOCUS)*HORIZON_HAZE;
 float storm=weather.z;float rain=pow(1-abs(dir.y),2)*RAIN_SKY_HAZE*storm; // Rain mist on the horizon.
 float lit=1-STORM_SKY_DARKEN*storm+LIGHTNING_SKY_AMBIENT*weather.w+STORM_CLOUD_FLASH*lightning_glow(dir);
 if(additive) return rgb*(1-h)*(1-rain)*lit;
 float s=saturate(dot(dir,sun_direction.xyz));
 float3 sky=lerp(lerp(lerp(rgb,dot(rgb,float3(0.299,0.587,0.114)).xxx,STORM_FOG_GREY*storm),storm_fog_color(),h),
                rain_fog_color(),rain)*lit;
 return sky+sun_color.rgb*(SUN_CORE*pow(s,SUN_CORE_FOCUS)+SUN_HALO*pow(s,SUN_HALO_FOCUS))*(1-storm);
}
// Remastered model lighting per pixel: a soft terminator, sky/ground ambient and a rim on the sun's side.
float3 remastered_light(float3 n,float3 points,float3 w) {
 n=normalize(n);float3 v=normalize(eye.xyz-w);float ndl=dot(n,sun_direction.xyz);
 float diffuse=saturate((ndl+LIGHT_WRAP)/(1+LIGHT_WRAP));
 float3 sky=ambient.rgb*(SKY_AMBIENT_BASE+SKY_AMBIENT_UP*(n.y*0.5+0.5));
 float rim=pow(1-saturate(dot(n,v)),3)*saturate(0.5-0.5*dot(v,sun_direction.xyz));
 float lit=lerp(1-shadow.w*shadow_amount(),1,sun_visibility(w));
 return min((sky+sun_light()*(diffuse+rim*RIM_LIGHT)*lit)*MODEL_BRIGHTNESS+points,LIGHT_MAX);
}
// Smooth 2D value noise in 0..1 (drawing-only patterns).
float noise_hash(float2 p){return frac(sin(dot(p,float2(127.1,311.7)))*43758.5453);}
float value_noise(float2 p){float2 i=floor(p),f=frac(p);f=f*f*(3-2*f);
 return lerp(lerp(noise_hash(i),noise_hash(i+float2(1,0)),f.x),lerp(noise_hash(i+float2(0,1)),noise_hash(i+float2(1,1)),f.x),f.y);}
// Remastered ground weather: settled snow (weather.y) whitens the lit ground, patchy by texture brightness;
// wet ground (weather.x) darkens and glints where it mirrors the sun.
float3 remastered_ground(float3 albedo,float3 light,float3 w) {
 float vis=sun_visibility(w);
 light*=GROUND_BRIGHTNESS*lerp(1-SHADOW_GROUND*shadow_amount(),1,vis)*(1+LIGHTNING_LIGHT*weather.w*vis);
 float3 luma=float3(0.299,0.587,0.114);float l=dot(albedo,luma);
 float snow=saturate(weather.y*SNOW_COVER*1.2+(l-0.45)*1.5)*step(0.001,weather.y);snow*=snow*(3-2*snow);
 float3 v=normalize(eye.xyz-w);float3 r=reflect(-v,float3(0,1,0));
 // The sun mirrors in world-fixed puddle patches, and not while the storm overcasts it.
 float glint=sun_color.w>0?pow(saturate(dot(r,sun_direction.xyz)),WET_GLOSS)*WET_SHINE*weather.x*vis*(1-weather.z)
  *smoothstep(1-PUDDLE_COVER,1.15-PUDDLE_COVER,value_noise(w.xz/PUDDLE_SIZE)):0;
 float3 ground=albedo*(1-WET_DARKEN*weather.x)*light+sun_color.rgb*glint;
 // Snow takes the light's brightness, not the baked lightmap's tint.
 return lerp(ground,float3(0.9,0.92,0.96)*max(dot(light,luma)*1.4,0.3),snow);
}
// Remastered foliage shading: wrapped directional sunlight from the plant's normal, sky/ground hemisphere
// softness, sun shadows and lightning flash, subsurface backlight, sunlit sheen, per-plant tint, and root AO.
float3 foliage(float3 rgb,float3 w,float3 n,uint tile,float h,float uv_y) {
 n=normalize(n);
 float vis=sun_visibility(w);float sun=sun_color.w>0?1:0;
 bool baked=(tile&0x20000000)!=0;
 bool rigid=baked && h==0;
 float ndl=dot(n,sun_direction.xyz);
 float wrap=saturate((ndl+0.65)/1.65);
 float3 sl=sun_light();
 float sun_luma=dot(sl,float3(0.299,0.587,0.114));
 float hemi=0.92+0.14*(n.y*0.5+0.5);
 rgb*=lerp(1-SHADOW_GROUND*shadow_amount()*(baked?1:0.65),1,vis)*(1+LIGHTNING_LIGHT*weather.w*vis);
 if(!baked)
  rgb*=hemi*(0.78+0.28*wrap+FOLIAGE_SUN*vis*sun*sun_luma*(0.35+0.95*wrap));
 else
  rgb*=lerp(1.0,lerp(0.84,1.18,wrap),vis*sun)*(0.94+0.1*(n.y*0.5+0.5));
 float leaf_up=rigid?0:(baked?saturate(uv_y):saturate(h/FOLIAGE_ROOT_HEIGHT));
 float3 vdir=normalize(w-eye.xyz);
 float back=pow(saturate(dot(vdir,sun_direction.xyz)),4)*FOLIAGE_BACKLIGHT*vis*sun*leaf_up;
 rgb+=rgb*sl*back*float3(1,1.12,0.65);
 float3 hv=normalize(sun_direction.xyz-vdir);
 float sheen=pow(saturate(dot(n,hv)),18.0)*FOLIAGE_SHEEN*vis*sun*leaf_up;
 rgb+=sl*sheen*lerp(rgb,float3(1,1,0.9),0.35);
 float k1=(tile&15)/15.0-0.5,k2=((tile>>4)&15)/15.0-0.5;
 rgb*=(1+FOLIAGE_TINT*2*k1)*(1+FOLIAGE_TINT*float3(k2,-0.4*k2,-k2));
 if(!baked)rgb*=lerp(FOLIAGE_ROOT,1,leaf_up);
 else if(!rigid)rgb*=lerp(0.76,1.0,saturate(uv_y*1.4));
 return rgb;
}
float3 fogged(float3 rgb,float3 w,bool additive) {
 if(remaster.y>0) return remastered_sky(rgb,w,additive);
 if(fog.z<=0 || material.z!=0) return rgb;
 float d=dot(w-eye.xyz,view_dir.xyz);
 float f=fog.w!=0?exp(-d*view_dir.w):saturate((fog.y-d)/max(fog.y-fog.x,1e-3));
 if(remaster.x>0) return remastered_fog(rgb,w,additive,f);
 return additive?rgb*f:lerp(fog_color.rgb,rgb,f);
}
Texture2D image:register(t0);SamplerState image_sampler:register(s0);
Texture2DArray terrain_tiles:register(t1);Texture2DArray terrain_details:register(t2);
cbuffer TerrainDetails:register(b1) {uint4 tile_details[64];};
Texture2DArray terrain_windows:register(t3);Texture2D terrain_edge:register(t4);
// Remastered soft particles (options.z): a copy of the scene depth (reversed Z, near .1, far 30000).
Texture2D scene_depth:register(t5);
float scene_view_z(float d){return 3000/(0.1+d*29999.9);}
// A 4x4 ordered dither threshold in (0, 1).
float bayer4(uint2 q){const uint m[16]={0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};return (m[q.y*4+q.x]+0.5)/16;}
float4 ps_main(Out v):SV_TARGET {
 if(material.y!=0) {
   uint flags=material.x;
   float4 tex=(flags&0x10000)!=0 && options.x>0.5?image.Sample(image_sampler,v.uv):float4(1,1,1,1);
   if(options.y>0.5)tex.rgb=dot(tex.rgb,float3(0.299,0.587,0.114)).xxx;
   float3 rgb=v.c.rgb;float alpha=v.c.a;
   // WIDGET's BSP, anchor lighting, range and byte alpha are prepared together before submission.
   if((flags&0x10000)!=0) {
     float3 diffuse=(flags&4)!=0?v.c.aaa:v.c.rgb;
     rgb=(flags&1)!=0?tex.rgb*diffuse:(flags&2)!=0?tex.rgb+diffuse:tex.rgb;
     alpha=(flags&8)!=0?tex.a*alpha:tex.a;
   }
   alpha*=options.w;
   // Remastered plants switching detail level: the near (opaque) pass dissolves each model by its alpha.
   if(material.w==1 && is_foliage(v.tile) && alpha<0.999){
     uint2 q=uint2(v.p.xy)&3;
     clip(alpha-bayer4(q));
   }
   if((flags&0x20000000)==0)clip(alpha-1.0/255.0);
   if(options.z>0) {
     float fade=saturate((scene_view_z(scene_depth.Load(int3(v.p.xy,0)).r)-scene_view_z(v.p.z))/SOFT_PARTICLE_RANGE);
     alpha*=fade;
     if((flags&0x7f00)==0x400)rgb*=fade; // Darkening blends by colour, not alpha.
   }
   if((flags&1)!=0 && material.z==0 && material.w==0)
     rgb*=remaster.x>0 && sun_color.w>0?remastered_light(v.n,v.point_only,v.w):v.points;
   if(is_foliage(v.tile))rgb=foliage(rgb,v.w,v.n,v.tile,v.detail,v.uv.y);
   return float4(fogged(rgb,v.w,(flags&0x7f00)==0x2000),alpha);
 }
 bool tiled=options.y>0.5 && v.tile!=0xffffffff;
 uint layer=7;
 // Land view faces of quads above cell level sample their texture window; cell faces draw as near tiles.
 if(tiled && (v.tile&0x80000000)!=0) layer=v.tile&7;
 float4 base=!tiled?(options.x>0.5?image.Sample(image_sampler,v.uv):float4(0.55,0.62,0.53,1))
   :layer==7?terrain_tiles.Sample(image_sampler,float3(v.uv,v.tile&0xffff))
   :layer==6?terrain_edge.Sample(image_sampler,v.uv)
   :terrain_windows.Sample(image_sampler,float3((v.w.xz-windows[layer].xy)*windows[layer].z,layer));
 float light=tiled?1.0:0.4+0.6*saturate(dot(normalize(v.n),normalize(float3(0.3,1,-0.5))));
 float3 diffuse=tiled?saturate(v.c.rgb+v.points):v.c.rgb*light;
 float3 rgb=fogged(remaster.x>0?remastered_ground(base.rgb,diffuse,v.w):base.rgb*diffuse,v.w,false);
 if(tiled && layer==7 && options.w>0) {
   // Detail pass sub_1603AE60, unfogged with src ZERO / dest INVSRCCOLOR, scaled by the diffuse of a point
   // light at the eye. Material 0x16110608 (bump): (1 - DOTPRODUCT3(normal map, texture factor)) x diffuse;
   // 0x16110604: DT intensity x diffuse. The cell's base tile picks the DT.
   uint t=v.tile>>16;uint slot=tile_details[t>>2][t&3];
   float4 dt=terrain_details.Sample(image_sampler,float3(v.uv,slot));
   // Missing DT slots (all zero; packed normals always have blue >= 131) leave the frame unchanged.
   float k=dt.b==0?0:bump.w>0?1-saturate(4*dot(dt.rgb-0.5,bump.rgb-0.5)):dt.a;
   rgb*=1-k*v.detail;
 }
 return float4(rgb,base.a);
}
// Remastered sun shadow pass: depth seen from the sun; textured materials cut their alpha shapes (foliage).
struct ShadowOut {float4 p:SV_POSITION;float2 uv:TEXCOORD;float a:COLOR;};
ShadowOut vs_shadow(In v) {ShadowOut o;o.p=mul(mul(float4(v.p,1),world),shadow_matrix);o.uv=v.uv;o.a=v.c.a;return o;}
// Remastered instanced flora (widgets.hpp FloraInstance): the model-space vertex placed, turned and sized
// per plant, its albedo lit by the plant's anchor colour, and the foliage layer built as foliage_layer does.
struct Inst {float4 place:PLACE;float2 turn:TURN;float4 c:TINT;uint hash:HASH;};
In flora_vertex(In v,Inst i){In o=v;float3 m=v.p;float3 n=v.n;float s=i.place.w;
 o.p=float3((m.x*i.turn.x+m.y*i.turn.y)*s+i.place.x,m.z*s+i.place.y,i.place.z+(m.y*i.turn.x-m.x*i.turn.y)*s);
 o.n=float3(n.x*i.turn.x+n.y*i.turn.y,n.z,n.y*i.turn.x-n.x*i.turn.y);
 o.c=float4(v.c.rgb*i.c.rgb,i.c.a);
 uint h=v.tile!=0?0:(uint)clamp(m.z*s*16,0,4095);
 o.tile=0x40000000u|(v.tile!=0?0x20000000u:0u)|(h<<8)|(i.hash&0xff);return o;}
Out vs_flora(In v,Inst i){return vs_main(flora_vertex(v,i));}
ShadowOut vs_shadow_flora(In v,Inst i){return vs_shadow(flora_vertex(v,i));}
void ps_shadow(ShadowOut v) {
 if(options.x>0.5 && (material.x&0x10000)!=0) {
   float a=image.Sample(image_sampler,v.uv).a;if((material.x&8)!=0)a*=v.a;clip(a-0.5);
 }
})";
struct Constants {
    XMFLOAT4X4 view_projection;
    XMFLOAT4X4 world;
    XMFLOAT4 options;
    XMUINT4 material{};
    XMFLOAT4 eye{}, fog{}, fog_color{}, ambient{}, sun_direction{}, sun_color{}, view_dir{};
    XMFLOAT4 bump{};         // Bump detail texture factor (RGB), w set when the bump path draws.
    XMFLOAT4 detail_light{}; // Detail light position (renderer space).
    XMFLOAT4 windows[6]{};   // Origin (world X/Z), 1 / extent; [0].w enables them, [5].w is the map size.
    XMFLOAT4 model_reveal{}; // Class min/max height, construction reveal, sticky material override.
    XMUINT4 point_options{};
    XMFLOAT4 remaster{}; // Remastered Look: enabled, sky layer, mist floor height and falloff.
    XMFLOAT4 weather{};  // Remastered weather: ground wetness, settled snow, storm, lightning flash.
    XMFLOAT4X4 shadow_matrix{}; // Remastered sun shadows: world to shadow map.
    XMFLOAT4 shadow{};          // Enabled, sampling spread, depth bias, model strength.
    XMFLOAT4 wind{};            // Remastered plant wind: time (s), strength, direction X/Z.
    XMFLOAT4 grass{};           // Remastered static grass: camera X/Z, reach, enabled (TerrainView::grass_chunk).
};
using renderer_shared::detail_range;
// Port diagnostics: GPU timestamps per frame, each attributed to the section named at the mark before it.
struct GpuTimer {
    enum Section : int { other, shadow, land, models, foliage, effects, post, count };
    static constexpr unsigned depth = 5, capacity = 512;
    struct Frame {
        ComPtr<ID3D11Query> disjoint;
        std::vector<ComPtr<ID3D11Query>> stamps = std::vector<ComPtr<ID3D11Query>>(capacity);
        std::vector<int> sections = std::vector<int>(capacity);
        unsigned used{};
        bool open{}, pending{};
    };
    std::array<Frame, depth> frames;
    unsigned index{};
    ID3D11Device* device{};
    ID3D11DeviceContext* context{};
    std::array<double, count> sums{};
    double total{};
    unsigned resolved{};
    void collect(Frame& frame) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        if (context->GetData(frame.disjoint.Get(), &disjoint, sizeof disjoint, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            return;
        frame.pending = false;
        if (disjoint.Disjoint || !disjoint.Frequency || frame.used < 2)
            return;
        std::vector<UINT64> times(frame.used);
        for (unsigned i = 0; i < frame.used; ++i)
            if (context->GetData(frame.stamps[i].Get(), &times[i], sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
                return;
        const double scale = 1000.0 / static_cast<double>(disjoint.Frequency);
        for (unsigned i = 0; i + 1 < frame.used; ++i)
            if (frame.sections[i] >= 0 && times[i + 1] >= times[i])
                sums[frame.sections[i]] += static_cast<double>(times[i + 1] - times[i]) * scale;
        if (times[frame.used - 1] >= times[0])
            total += static_cast<double>(times[frame.used - 1] - times[0]) * scale;
        ++resolved;
    }
    void begin(ID3D11Device* d, ID3D11DeviceContext* c) {
        device = d;
        context = c;
        for (auto& frame : frames)
            if (frame.pending)
                collect(frame);
        auto& frame = frames[index];
        frame.pending = false; // Still unresolved after a full ring: dropped.
        if (!frame.disjoint) {
            D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            if (FAILED(device->CreateQuery(&desc, &frame.disjoint)))
                return;
        }
        context->Begin(frame.disjoint.Get());
        frame.used = 0;
        frame.open = true;
        mark(other);
    }
    void mark(int section) {
        if (!context)
            return;
        auto& frame = frames[index];
        if (!frame.open || frame.used >= capacity)
            return;
        auto& stamp = frame.stamps[frame.used];
        if (!stamp) {
            D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP, 0};
            if (FAILED(device->CreateQuery(&desc, &stamp)))
                return;
        }
        context->End(stamp.Get());
        frame.sections[frame.used++] = section;
    }
    void end() {
        auto& frame = frames[index];
        if (!frame.open)
            return;
        mark(-1);
        context->End(frame.disjoint.Get());
        frame.open = false;
        frame.pending = true;
        index = (index + 1) % depth;
    }
};
// Adds the time of its scope to `sum` (milliseconds).
struct CpuTimer {
    double& sum;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~CpuTimer() { sum += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); }
};
XMFLOAT4 d3dcolor(std::uint32_t color) {
    return {static_cast<float>(color >> 16 & 255) / 255, static_cast<float>(color >> 8 & 255) / 255,
            static_cast<float>(color & 255) / 255, 1};
}
} // namespace
struct Renderer::Impl : renderer_shared::TerrainView {
    RenderDetail detail;
    bool widgets{true};
    int terrain_detail{2}; // 0 off, 1 detail textures, 2 bump detail maps.
    std::optional<Terrain> widget_terrain;
    std::vector<PointLight> widget_lights;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11DepthStencilView> depth;
    ComPtr<ID3D11ShaderResourceView> depth_view; // Readable depth for later remaster passes.
    // The depth buffer and the copy that remastered soft particles read while depth stays bound.
    ComPtr<ID3D11Texture2D> depth_texture, depth_copy;
    ComPtr<ID3D11ShaderResourceView> depth_copy_view;
    // Remastered look (post_process.hpp): the half-float scene target, its bloom chain and the passes.
    bool remastered{}, scene_drawn{}, scene_environment{};
    WeatherLook weather;
    ComPtr<ID3D11RenderTargetView> hdr_target;
    ComPtr<ID3D11ShaderResourceView> hdr_view;
    struct BloomLevel {
        ComPtr<ID3D11RenderTargetView> target;
        ComPtr<ID3D11ShaderResourceView> view;
        unsigned width{}, height{};
    };
    std::vector<BloomLevel> bloom;
    ComPtr<ID3D11VertexShader> post_vs;
    ComPtr<ID3D11PixelShader> post_down, post_up, post_ao, post_composite;
    ComPtr<ID3D11RenderTargetView> ao_target;
    ComPtr<ID3D11ShaderResourceView> ao_view;
    float view_tan[2]{1, 1}; // The last draw's horizontal and vertical half-angle tangents.
    ComPtr<ID3D11Buffer> post_constants;
    ComPtr<ID3D11SamplerState> post_sampler;
    ComPtr<ID3D11BlendState> post_add;
    ID3D11RenderTargetView* scene_target() const { return remastered ? hdr_target.Get() : target.Get(); }
    void post_process();
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;
    // Remastered instanced flora: vertex stream 0 is the model, stream 1 the FloraInstance records.
    ComPtr<ID3D11VertexShader> flora_vs, flora_shadow_vs;
    ComPtr<ID3D11InputLayout> flora_layout;
    ComPtr<ID3D11Buffer> vertices, indices, constants;
    ComPtr<ID3D11SamplerState> sampler, model_sampler;
    std::array<ComPtr<ID3D11SamplerState>, 3> clamp_samplers, wrap_samplers;
    ComPtr<ID3D11ShaderResourceView> image, terrain_tiles, terrain_details;
    ComPtr<ID3D11Texture2D> terrain_tile_array; // terrain_tiles' texture, layers 256 + the scar pool.
    UINT terrain_tile_capacity{};
    void upload_terrain_tiles(const TerrainMaterials& materials, const Terrain& terrain, bool full);
    ComPtr<ID3D11Buffer> tile_details;
    ComPtr<ID3D11Texture2D> window_texture;
    ComPtr<ID3D11ShaderResourceView> terrain_windows, terrain_edge;
    ComPtr<ID3D11Buffer> lod_vertices;
    std::size_t lod_capacity{};
    ComPtr<ID3D11RasterizerState> solid, wire, model_solid, model_wire;
    // Remastered sun shadows: the map, its comparison sampler, the pass's states and shaders, and the casters
    // recorded during the last scene draw. They are drawn from the sun at the start of the next scene draw,
    // so the shadows trail moving casters by one frame.
    ComPtr<ID3D11DepthStencilView> shadow_target;
    ComPtr<ID3D11ShaderResourceView> shadow_view;
    ComPtr<ID3D11SamplerState> shadow_sampler;
    ComPtr<ID3D11RasterizerState> shadow_raster;
    ComPtr<ID3D11DepthStencilState> shadow_depth;
    ComPtr<ID3D11VertexShader> shadow_vs;
    ComPtr<ID3D11PixelShader> shadow_ps;
    struct ShadowCaster {
        ComPtr<ID3D11Buffer> vertices, indices; // No indices: a plain Draw.
        UINT first{}, count{};
        XMFLOAT4X4 world{};
        ComPtr<ID3D11ShaderResourceView> texture;
        std::uint32_t flags{};
        // Instanced flora: the instance stream and range, and the model's base vertex.
        ComPtr<ID3D11Buffer> instances;
        UINT instance_first{}, instance_count{};
        INT base_vertex{};
    };
    std::vector<ShadowCaster> casters;
    bool shadow_recording{};
    // Frame timing (Renderer::timings).
    GpuTimer gpu_timer;
    double cpu_draw{}, cpu_widgets{}, cpu_grass{}, cpu_end{};
    double stat_widget_vertices{}, stat_widget_triangles{}, stat_grass_vertices{}, stat_grass_triangles{}, caster_count{};
    unsigned timed_frames{};
    // Remastered static grass (TerrainView::grass_chunk): chunks built once near the camera, kept while in reach.
    struct GrassChunk {
        ComPtr<ID3D11Buffer> vertices, indices;
        UINT index_count{};
        std::array<unsigned, 17> rank_indices{}; // TerrainView::grass_chunk.
        float low{}, high{};                     // Height range of its vertices, for frustum culling.
    };
    std::unordered_map<std::int64_t, GrassChunk> grass_chunks;
    unsigned grass_chunk_generation{~0u};
    std::vector<Vertex> grass_vertex_data;
    std::vector<std::uint32_t> grass_index_data;
    ComPtr<ID3D11BlendState> alpha_blend, additive_blend, darken_blend;
    ComPtr<ID3D11DepthStencilState> depth_write, depth_read, depth_off;
    struct Material {
        std::uint32_t flags{};
        unsigned first{}, count{};
        ComPtr<ID3D11ShaderResourceView> texture;
    };
    std::vector<Material> materials;
    struct SceneBatch {
        ComPtr<ID3D11Buffer> vertices, indices;
        ComPtr<ID3D11ShaderResourceView> texture_override;
        bool grayscale{};
        std::vector<Material> materials;
        std::vector<XMFLOAT4X4> worlds;
        std::vector<bool> visible;
        std::vector<float> reveal;
        std::vector<float> opacity;
        std::vector<bool> reveal_blend;
        float reveal_min{}, reveal_max{};
        bool screen_oriented{}; // Rotation replaced by the camera's at draw time.
        bool soul{};            // Drawn after the other scene models (see the scene loop).
        std::size_t vertex_count{};
        std::size_t index_capacity{};
        std::vector<std::uint32_t> dynamic_indices;
        bool dynamic_mesh{};
        float widget_range{};
        std::uint32_t widget_fade_flags{};
        std::shared_ptr<const WidgetField> widget_field;
        // Remastered instanced flora: the field's models (uploaded on first use) and the instance stream.
        ComPtr<ID3D11Buffer> flora_vertices, flora_indices, flora_instances;
        std::vector<FloraMesh::Range> flora_models;
        std::size_t flora_instance_capacity{};
        // MRMINSTANCE automatic detail (MRM models only): MODEL::SetDetailLevel 0x1601ae10 radius, the
        // model-space box, and per placement the UpdateLod 0x1601cf30 state with its own face list.
        struct Lod {
            unsigned vertices{};
            int pending{};
            ComPtr<ID3D11Buffer> indices;
            std::vector<Material> materials;
        };
        std::shared_ptr<const MrmGeometry> mrm;
        float detail_radius{};
        XMFLOAT3 box_min{}, box_max{};
        std::vector<Lod> lods;
    };
    std::vector<SceneBatch> scene;
    std::vector<ParticleBatch> particles;
    ComPtr<ID3D11Buffer> particle_vertices, particle_indices;
    std::size_t particle_capacity{};
    struct PointSource {
        XMFLOAT4 position_range, color, land_position_range, land_color;
    };
    std::vector<PointSource> point_sources;
    ComPtr<ID3D11Buffer> point_buffer;
    ComPtr<ID3D11ShaderResourceView> point_view;
    std::size_t point_capacity{};
    std::vector<std::size_t> scene_batch; // Batch index of each scene model (SIZE_MAX when not drawn).
    std::optional<SceneBatch> sky;
    std::size_t sky_vertex_count{};
    std::vector<unsigned> sky_slots; // ENVI slot of each sky material ("sky0".."sky4").
    std::optional<Environment> environment;
    static std::vector<Material> prepare_model(ID3D11Device* device, ID3D11DeviceContext* context,
                                               const ModelAsset& model, Mesh& grouped);
    std::vector<Mesh::Material> model_signatures;
    std::vector<std::uint32_t> model_indices, model_triangle_materials;
    unsigned vertex_count{};
    unsigned width{}, height{}, index_count{};
    std::string ini_file;
    void resize(unsigned w, unsigned h) {
        if (!w || !h || (width == w && height == h))
            return;
        context->OMSetRenderTargets(0, nullptr, nullptr);
        target.Reset();
        depth.Reset();
        depth_view.Reset();
        depth_texture.Reset();
        depth_copy.Reset();
        depth_copy_view.Reset();
        hdr_target.Reset();
        hdr_view.Reset();
        ao_target.Reset();
        ao_view.Reset();
        bloom.clear();
        hr(swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0), "ResizeBuffers");
        ComPtr<ID3D11Texture2D> back;
        hr(swap->GetBuffer(0, IID_PPV_ARGS(&back)), "GetBuffer");
        hr(device->CreateRenderTargetView(back.Get(), nullptr, &target), "CreateRenderTargetView");
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        // Reversed-Z float depth: the 24-bit buffer with near 0.1 left nearby faces of statues and buildings
        // fighting at a distance; 1 at the near plane, 0 at the far plane keeps precision nearly uniform.
        desc.Format = DXGI_FORMAT_R32_TYPELESS;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> tex;
        hr(device->CreateTexture2D(&desc, nullptr, &tex), "CreateDepthTexture");
        D3D11_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = DXGI_FORMAT_D32_FLOAT;
        dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        hr(device->CreateDepthStencilView(tex.Get(), &dsv, &depth), "CreateDepthStencilView");
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        hr(device->CreateShaderResourceView(tex.Get(), &srv, &depth_view), "CreateDepthView");
        depth_texture = tex;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        hr(device->CreateTexture2D(&desc, nullptr, &depth_copy), "CreateDepthCopy");
        hr(device->CreateShaderResourceView(depth_copy.Get(), &srv, &depth_copy_view), "CreateDepthCopyView");
        const auto color_target = [&](unsigned cw, unsigned ch, ComPtr<ID3D11RenderTargetView>& rtv,
                                      ComPtr<ID3D11ShaderResourceView>& view) {
            D3D11_TEXTURE2D_DESC color{};
            color.Width = cw;
            color.Height = ch;
            color.MipLevels = color.ArraySize = 1;
            color.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            color.SampleDesc.Count = 1;
            color.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            ComPtr<ID3D11Texture2D> texture;
            hr(device->CreateTexture2D(&color, nullptr, &texture), "CreatePostTarget");
            hr(device->CreateRenderTargetView(texture.Get(), nullptr, &rtv), "CreatePostTargetView");
            hr(device->CreateShaderResourceView(texture.Get(), nullptr, &view), "CreatePostView");
        };
        color_target(w, h, hdr_target, hdr_view);
        color_target(w, h, ao_target, ao_view);
        bloom.resize(post::bloom_levels(w, h));
        for (unsigned i = 0; i < bloom.size(); ++i) {
            bloom[i].width = post::level_size(w, i);
            bloom[i].height = post::level_size(h, i);
            color_target(bloom[i].width, bloom[i].height, bloom[i].target, bloom[i].view);
        }
        width = w;
        height = h;
    }
};
Renderer::Renderer(HWND window) : impl_(std::make_unique<Impl>()) {
    auto& p = *impl_;
    D3D_FEATURE_LEVEL level{};
    D3D_FEATURE_LEVEL requested[] = {D3D_FEATURE_LEVEL_11_0};
    auto result =
        D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                          requested, 1, D3D11_SDK_VERSION, &p.device, &level, &p.context);
    if (FAILED(result))
        result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   requested, 1, D3D11_SDK_VERSION, &p.device, &level, &p.context);
    hr(result, "D3D11CreateDevice");
    ComPtr<IDXGIDevice> dxgi;
    hr(p.device.As(&dxgi), "IDXGIDevice");
    ComPtr<IDXGIAdapter> adapter;
    hr(dxgi->GetAdapter(&adapter), "GetAdapter");
    ComPtr<IDXGIFactory2> factory;
    hr(adapter->GetParent(IID_PPV_ARGS(&factory)), "GetFactory");
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    hr(factory->CreateSwapChainForHwnd(p.device.Get(), window, &desc, nullptr, nullptr, &p.swap),
       "CreateSwapChain");
    factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
    ComPtr<ID3DBlob> vs, ps, errors;
    auto compile = [&](const char* entry, const char* profile, ComPtr<ID3DBlob>& blob) {
        static const auto source = tuning::shader_constants(false) + shader; // remaster_tuning.hpp values.
        auto h = D3DCompile(source.data(), source.size(), "scene.hlsl", nullptr, nullptr, entry, profile,
                            D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, &errors);
        if (FAILED(h) && errors)
            throw Error(std::string(static_cast<char*>(errors->GetBufferPointer()), errors->GetBufferSize()));
        hr(h, "CompileShader");
    };
    compile("vs_main", "vs_5_0", vs);
    compile("ps_main", "ps_5_0", ps);
    hr(p.device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &p.vs),
       "CreateVertexShader");
    hr(p.device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &p.ps),
       "CreatePixelShader");
    {
        ComPtr<ID3DBlob> shadow_vs, shadow_ps;
        compile("vs_shadow", "vs_5_0", shadow_vs);
        compile("ps_shadow", "ps_5_0", shadow_ps);
        hr(p.device->CreateVertexShader(shadow_vs->GetBufferPointer(), shadow_vs->GetBufferSize(), nullptr,
                                        &p.shadow_vs),
           "CreateShadowVertexShader");
        hr(p.device->CreatePixelShader(shadow_ps->GetBufferPointer(), shadow_ps->GetBufferSize(), nullptr,
                                       &p.shadow_ps),
           "CreateShadowPixelShader");
        const auto size = static_cast<UINT>(tuning::shadow_map_size);
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = size;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32_TYPELESS;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> texture;
        hr(p.device->CreateTexture2D(&desc, nullptr, &texture), "CreateShadowMap");
        D3D11_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = DXGI_FORMAT_D32_FLOAT;
        dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        hr(p.device->CreateDepthStencilView(texture.Get(), &dsv, &p.shadow_target), "CreateShadowTarget");
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        hr(p.device->CreateShaderResourceView(texture.Get(), &srv, &p.shadow_view), "CreateShadowView");
        D3D11_SAMPLER_DESC compare{};
        compare.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        compare.AddressU = compare.AddressV = compare.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
        compare.BorderColor[0] = compare.BorderColor[1] = compare.BorderColor[2] = compare.BorderColor[3] = 1;
        compare.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
        compare.MaxLOD = D3D11_FLOAT32_MAX;
        hr(p.device->CreateSamplerState(&compare, &p.shadow_sampler), "CreateShadowSampler");
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        raster.SlopeScaledDepthBias = 1.5f;
        hr(p.device->CreateRasterizerState(&raster, &p.shadow_raster), "CreateShadowRaster");
        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = TRUE;
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        hr(p.device->CreateDepthStencilState(&depth, &p.shadow_depth), "CreateShadowDepth");
    }
    {
        auto compile_post = [&](const char* entry, const char* profile) {
            ComPtr<ID3DBlob> blob;
            static const auto source = tuning::shader_constants(false) + post::hlsl;
            auto h = D3DCompile(source.data(), source.size(), "post.hlsl", nullptr, nullptr, entry, profile,
                                D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, &errors);
            if (FAILED(h) && errors)
                throw Error(std::string(static_cast<char*>(errors->GetBufferPointer()), errors->GetBufferSize()));
            hr(h, "CompilePostShader");
            return blob;
        };
        const auto post_vs = compile_post("vs_post", "vs_5_0");
        hr(p.device->CreateVertexShader(post_vs->GetBufferPointer(), post_vs->GetBufferSize(), nullptr, &p.post_vs),
           "CreatePostVertexShader");
        for (auto [entry, shader] : {std::pair{"ps_down", p.post_down.GetAddressOf()}, std::pair{"ps_up", p.post_up.GetAddressOf()}, std::pair{"ps_ao", p.post_ao.GetAddressOf()},
                                     std::pair{"ps_composite", p.post_composite.GetAddressOf()}}) {
            const auto blob = compile_post(entry, "ps_5_0");
            hr(p.device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, shader),
               "CreatePostPixelShader");
        }
        D3D11_BUFFER_DESC pc{};
        pc.ByteWidth = sizeof(post::Constants);
        pc.Usage = D3D11_USAGE_DEFAULT;
        pc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        hr(p.device->CreateBuffer(&pc, nullptr, &p.post_constants), "CreatePostConstants");
        D3D11_SAMPLER_DESC ps_desc{};
        ps_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        ps_desc.AddressU = ps_desc.AddressV = ps_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        ps_desc.MaxLOD = D3D11_FLOAT32_MAX;
        hr(p.device->CreateSamplerState(&ps_desc, &p.post_sampler), "CreatePostSampler");
        D3D11_BLEND_DESC add{};
        add.RenderTarget[0].BlendEnable = TRUE;
        add.RenderTarget[0].SrcBlend = add.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        add.RenderTarget[0].DestBlend = add.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        add.RenderTarget[0].BlendOp = add.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        add.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr(p.device->CreateBlendState(&add, &p.post_add), "CreatePostAdd");
    }
    const D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"LAYER", 0, DXGI_FORMAT_R32_UINT, 0, 36, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    static_assert(sizeof(Vertex) == 40);
    hr(p.device->CreateInputLayout(elements, 5, vs->GetBufferPointer(), vs->GetBufferSize(), &p.layout),
       "CreateInputLayout");
    {
        ComPtr<ID3DBlob> flora_vs, flora_shadow_vs;
        compile("vs_flora", "vs_5_0", flora_vs);
        compile("vs_shadow_flora", "vs_5_0", flora_shadow_vs);
        hr(p.device->CreateVertexShader(flora_vs->GetBufferPointer(), flora_vs->GetBufferSize(), nullptr, &p.flora_vs),
           "CreateFloraVertexShader");
        hr(p.device->CreateVertexShader(flora_shadow_vs->GetBufferPointer(), flora_shadow_vs->GetBufferSize(), nullptr,
                                        &p.flora_shadow_vs),
           "CreateFloraShadowVertexShader");
        const D3D11_INPUT_ELEMENT_DESC flora_elements[] = {
            elements[0], elements[1], elements[2], elements[3], elements[4],
            {"PLACE", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"TURN", 0, DXGI_FORMAT_R32G32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"TINT", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 24, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"HASH", 0, DXGI_FORMAT_R32_UINT, 1, 28, D3D11_INPUT_PER_INSTANCE_DATA, 1}};
        hr(p.device->CreateInputLayout(flora_elements, 9, flora_vs->GetBufferPointer(), flora_vs->GetBufferSize(),
                                       &p.flora_layout),
           "CreateFloraInputLayout");
    }
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(Constants);
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr(p.device->CreateBuffer(&cb, nullptr, &p.constants), "CreateConstants");
    D3D11_SAMPLER_DESC sd{};
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    constexpr std::array<D3D11_FILTER, 3> filters{D3D11_FILTER_MIN_MAG_MIP_POINT,
        D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT, D3D11_FILTER_MIN_MAG_MIP_LINEAR};
    for (unsigned i = 0; i < filters.size(); ++i) {
        sd.Filter = filters[i];
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        hr(p.device->CreateSamplerState(&sd, &p.clamp_samplers[i]), "CreateSampler");
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        hr(p.device->CreateSamplerState(&sd, &p.wrap_samplers[i]), "CreateModelSampler");
    }
    p.sampler = p.clamp_samplers[1];
    p.model_sampler = p.wrap_samplers[1];
    D3D11_RASTERIZER_DESC rs{};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    rs.DepthClipEnable = true;
    hr(p.device->CreateRasterizerState(&rs, &p.solid), "CreateRasterizer");
    rs.FillMode = D3D11_FILL_WIREFRAME;
    hr(p.device->CreateRasterizerState(&rs, &p.wire), "CreateWireframe");
    rs.CullMode = D3D11_CULL_BACK;
    hr(p.device->CreateRasterizerState(&rs, &p.model_wire), "CreateModelWireframe");
    rs.FillMode = D3D11_FILL_SOLID;
    hr(p.device->CreateRasterizerState(&rs, &p.model_solid), "CreateModelSolid");
    D3D11_BLEND_DESC blend{};
    auto& target = blend.RenderTarget[0];
    target.BlendEnable = TRUE;
    target.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    target.BlendOp = target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    target.SrcBlendAlpha = D3D11_BLEND_ONE;
    target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr(p.device->CreateBlendState(&blend, &p.alpha_blend), "CreateAlphaBlend");
    target.DestBlend = target.DestBlendAlpha = D3D11_BLEND_ONE;
    hr(p.device->CreateBlendState(&blend, &p.additive_blend), "CreateAdditiveBlend");
    target.SrcBlend = target.SrcBlendAlpha = D3D11_BLEND_ZERO;
    target.DestBlend = D3D11_BLEND_INV_SRC_COLOR;
    target.DestBlendAlpha = D3D11_BLEND_ONE;
    hr(p.device->CreateBlendState(&blend, &p.darken_blend), "CreateDarkenBlend");
    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    hr(p.device->CreateDepthStencilState(&ds, &p.depth_write), "CreateDepthWrite");
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    hr(p.device->CreateDepthStencilState(&ds, &p.depth_read), "CreateDepthRead");
    ds.DepthEnable = FALSE;
    hr(p.device->CreateDepthStencilState(&ds, &p.depth_off), "CreateDepthOff");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    p.ini_file = utf8(application_data() / "inspector.ini");
    ImGui::GetIO().IniFilename = p.ini_file.c_str();
    ImGui::StyleColorsDark();
    if (!ImGui_ImplWin32_Init(window) || !ImGui_ImplDX11_Init(p.device.Get(), p.context.Get()))
        throw Error("ImGui initialization failed");
    log("Direct3D 11 renderer initialized; original game DLLs are not used");
}
Renderer::~Renderer() {
    if (ImGui::GetCurrentContext()) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
}
void Renderer::set_mesh(const Mesh& mesh) {
    auto& p = *impl_;
    p.lod.reset();
    p.vertices.Reset();
    p.indices.Reset();
    p.index_count = 0;
    p.materials.clear();
    p.model_signatures.clear();
    p.model_indices.clear();
    p.model_triangle_materials.clear();
    p.vertex_count = 0;
    if (mesh.vertices.empty() || mesh.indices.empty())
        return;
    if (mesh.vertices.size() > UINT_MAX / sizeof(Vertex) ||
        mesh.indices.size() > UINT_MAX / sizeof(std::uint32_t))
        throw Error("Mesh exceeds GPU buffer limit");
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = static_cast<UINT>(mesh.vertices.size() * sizeof(Vertex));
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA data{mesh.vertices.data()};
    hr(p.device->CreateBuffer(&desc, &data, &p.vertices), "UploadVertices");
    p.vertex_count = static_cast<unsigned>(mesh.vertices.size());
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.CPUAccessFlags = 0;
    desc.ByteWidth = static_cast<UINT>(mesh.indices.size() * sizeof(std::uint32_t));
    desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    data.pSysMem = mesh.indices.data();
    hr(p.device->CreateBuffer(&desc, &data, &p.indices), "UploadIndices");
    p.index_count = static_cast<unsigned>(mesh.indices.size());
}
// Groups triangles by material in first-encounter order and uploads each material texture.
std::vector<Renderer::Impl::Material> Renderer::Impl::prepare_model(ID3D11Device* device,
                                                                    ID3D11DeviceContext* context,
                                                                    const ModelAsset& model, Mesh& grouped) {
    const auto ranges = renderer_shared::group_model(model, grouped);
    std::vector<Renderer::Impl::Material> materials;
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        Renderer::Impl::Material material;
        material.flags = ranges[i].flags;
        material.first = ranges[i].first;
        material.count = ranges[i].count;
        if (model.textures[i]) {
            const auto& texture = *model.textures[i];
            const auto& image = texture.image;
            if (!image.width || !image.height ||
                image.rgba.size() != static_cast<std::size_t>(image.width) * image.height)
                throw Error("Invalid model texture");
            const bool mipmaps = (texture.header.flags & 0x18) == 0;
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = image.width;
            desc.Height = image.height;
            desc.ArraySize = 1;
            desc.MipLevels = mipmaps ? 0 : 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (mipmaps ? D3D11_BIND_RENDER_TARGET : 0);
            desc.MiscFlags = mipmaps ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
            ComPtr<ID3D11Texture2D> gpu;
            hr(device->CreateTexture2D(&desc, nullptr, &gpu), "CreateModelTexture");
            context->UpdateSubresource(gpu.Get(), 0, nullptr, image.rgba.data(), image.width * 4, 0);
            hr(device->CreateShaderResourceView(gpu.Get(), nullptr, &material.texture),
               "CreateModelTextureView");
            if (mipmaps)
                context->GenerateMips(material.texture.Get());
        }
        materials.push_back(std::move(material));
    }
    if (grouped.indices.size() != model.mesh.indices.size())
        throw Error("Invalid triangle material index");
    return materials;
}
void Renderer::set_model(const ModelAsset& model) {
    Mesh grouped;
    auto materials = Impl::prepare_model(impl_->device.Get(), impl_->context.Get(), model, grouped);
    set_mesh(grouped);
    impl_->materials = std::move(materials);
    impl_->model_signatures = model.mesh.materials;
    impl_->model_indices = model.mesh.indices;
    impl_->model_triangle_materials = model.mesh.triangle_materials;
}
void Renderer::update_model_mesh(const Mesh& mesh) {
    auto& p = *impl_;
    if (mesh.materials != p.model_signatures || !p.vertices || mesh.vertices.empty() ||
        mesh.vertices.size() > UINT_MAX / sizeof(Vertex) || mesh.indices.size() % 3 ||
        mesh.triangle_materials.size() != mesh.indices.size() / 3)
        throw Error("Model pose does not match uploaded model");
    if (mesh.vertices.size() != p.vertex_count) {
        // Skeletal detail changes the generated vertex count; keep textures and materials.
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = static_cast<UINT>(mesh.vertices.size() * sizeof(Vertex));
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        ComPtr<ID3D11Buffer> buffer;
        hr(p.device->CreateBuffer(&desc, nullptr, &buffer), "ResizeModelVertices");
        p.vertices = std::move(buffer);
        p.vertex_count = static_cast<unsigned>(mesh.vertices.size());
        p.model_indices.clear();
    }
    if (mesh.indices != p.model_indices || mesh.triangle_materials != p.model_triangle_materials) {
        std::vector<std::uint32_t> indices;
        std::vector<std::pair<unsigned, unsigned>> ranges;
        indices.reserve(mesh.indices.size());
        for (std::size_t material = 0; material < p.materials.size(); ++material) {
            const auto first = static_cast<unsigned>(indices.size());
            for (std::size_t face = 0; face < mesh.triangle_materials.size(); ++face)
                if (mesh.triangle_materials[face] == material) {
                    for (unsigned corner = 0; corner < 3; ++corner) {
                        auto index = mesh.indices[face * 3 + corner];
                        if (index >= p.vertex_count)
                            throw Error("Model pose index outside vertices");
                        indices.push_back(index);
                    }
                }
            ranges.emplace_back(first, static_cast<unsigned>(indices.size()) - first);
        }
        if (indices.size() != mesh.indices.size() || indices.size() > UINT_MAX / sizeof(std::uint32_t))
            throw Error("Invalid model pose topology");
        ComPtr<ID3D11Buffer> buffer;
        if (!indices.empty()) {
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = static_cast<UINT>(indices.size() * sizeof(std::uint32_t));
            desc.Usage = D3D11_USAGE_IMMUTABLE;
            desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
            D3D11_SUBRESOURCE_DATA data{indices.data()};
            hr(p.device->CreateBuffer(&desc, &data, &buffer), "UploadModelDetail");
        }
        p.indices = std::move(buffer);
        p.index_count = static_cast<unsigned>(indices.size());
        for (std::size_t i = 0; i < ranges.size(); ++i) {
            p.materials[i].first = ranges[i].first;
            p.materials[i].count = ranges[i].second;
        }
        p.model_indices = mesh.indices;
        p.model_triangle_materials = mesh.triangle_materials;
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr(p.context->Map(p.vertices.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "MapModelPose");
    std::memcpy(mapped.pData, mesh.vertices.data(), mesh.vertices.size() * sizeof(Vertex));
    p.context->Unmap(p.vertices.Get(), 0);
}
void Renderer::set_image(const Image& image) {
    auto view = upload_ui_image(image);
    impl_->image = static_cast<ID3D11ShaderResourceView*>(view.get());
}
std::shared_ptr<void> Renderer::upload_ui_image(const Image& image) {
    if (!image.width || !image.height ||
        image.rgba.size() != static_cast<std::size_t>(image.width) * image.height)
        throw Error("Invalid image");
    auto& p = *impl_;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = image.width;
    d.Height = image.height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{image.rgba.data(), image.width * 4, 0};
    ComPtr<ID3D11Texture2D> texture;
    hr(p.device->CreateTexture2D(&d, &data, &texture), "UploadImage");
    ComPtr<ID3D11ShaderResourceView> view;
    hr(p.device->CreateShaderResourceView(texture.Get(), nullptr, &view), "CreateImageView");
    return {view.Detach(), [](void* resource) { static_cast<ID3D11ShaderResourceView*>(resource)->Release(); }};
}
void Renderer::clear_image() {
    impl_->image.Reset();
}
void Renderer::ui_additive(ImDrawList& list) {
    list.AddCallback([](const ImDrawList*, const ImDrawCmd* command) {
        auto& p = *static_cast<Impl*>(command->UserCallbackData);
        p.context->OMSetBlendState(p.additive_blend.Get(), nullptr, 0xffffffff);
    }, impl_.get());
}
void Renderer::Impl::upload_terrain_tiles(const TerrainMaterials& materials, const Terrain& terrain, bool full) {
    const auto layers = 256 + materials.scars.size();
    if (layers > D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION)
        throw Error("Too many scarred terrain tiles");
    const auto changed = update_tiles(materials, terrain, full);
    const auto source_of = [&](std::size_t tile) -> const TerrainTile* {
        if (tile >= layers)
            return nullptr;
        return tile < 256 ? (materials.tiles[tile] ? &*materials.tiles[tile] : nullptr) : &materials.scars[tile - 256];
    };
    std::array<std::vector<std::uint32_t>, 7> defaults;
    for (unsigned level = 0; level < 7; ++level)
        defaults[level].resize((64u >> level) * (64u >> level), 0xff000000u);
    const auto pixels = [&](const TerrainTile* source, unsigned level) -> const std::uint32_t* {
        const auto width = 64u >> level;
        if (!source)
            return defaults[level].data();
        const auto& image = source->mipmaps[level];
        if (image.width != width || image.height != width || image.rgba.size() != width * width)
            throw Error("Invalid terrain mip dimensions");
        return image.rgba.data();
    };
    if (!full && terrain_tile_array && layers <= terrain_tile_capacity) {
        // Scars redraw a few layers at a time; re-sending the whole array stalls the frame.
        for (const auto tile : changed)
            for (unsigned level = 0; level < 7; ++level)
                context->UpdateSubresource(terrain_tile_array.Get(),
                                           D3D11CalcSubresource(level, static_cast<UINT>(tile), 7), nullptr,
                                           pixels(source_of(tile), level), (64u >> level) * 4, 0);
        return;
    }
    // Room for the whole scar pool ('mscr', at most 256), so later scars fit without a new array.
    const auto capacity = static_cast<UINT>(std::min<std::size_t>(
        std::max<std::size_t>(layers, 256 + materials.scar_limit), D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION));
    std::vector<D3D11_SUBRESOURCE_DATA> data;
    data.reserve(capacity * 7);
    for (std::size_t tile = 0; tile < capacity; ++tile)
        for (unsigned level = 0; level < 7; ++level)
            data.push_back({pixels(source_of(tile), level), (64u >> level) * 4, 0});
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 64;
    desc.MipLevels = 7;
    desc.ArraySize = capacity;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    hr(device->CreateTexture2D(&desc, data.data(), &texture), "UploadTerrainTiles");
    ComPtr<ID3D11ShaderResourceView> view;
    hr(device->CreateShaderResourceView(texture.Get(), nullptr, &view), "CreateTerrainTileView");
    terrain_tile_array = std::move(texture);
    terrain_tile_capacity = capacity;
    terrain_tiles = std::move(view);
}
void Renderer::update_terrain_tiles(const TerrainMaterials& materials, const Terrain& terrain) {
    impl_->upload_terrain_tiles(materials, terrain, false);
}
void Renderer::set_terrain_materials(const TerrainMaterials& materials) {
    impl_->upload_terrain_tiles(materials, {}, true);
    auto& p = *impl_;
    D3D11_TEXTURE2D_DESC desc{};
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    // DT00..DT15 with box-filtered mips: alpha holds the intensity (missing slots 0, which leaves the frame
    // unchanged) and RGB the bump normal map. MipMapsFadeOut's per-mip fade (SetMipMapFade with mask 8 from
    // LAND::UpdateDetailTextureFade) only lowers alpha toward 0 by 25/256 a level, which neither the detail
    // blend nor its >= 16 alpha test can see on these alpha-less textures, so it is not applied.
    const auto levels = renderer_shared::detail_mips(materials);
    std::vector<D3D11_SUBRESOURCE_DATA> detail_data;
    for (std::size_t i = 0; i < levels.size(); ++i)
        detail_data.push_back({levels[i].data(), (256u >> (i % 9)) * 4, 0});
    desc.Width = desc.Height = 256;
    desc.MipLevels = 9;
    desc.ArraySize = static_cast<UINT>(materials.details.size());
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    ComPtr<ID3D11Texture2D> details;
    hr(p.device->CreateTexture2D(&desc, detail_data.data(), &details), "UploadTerrainDetails");
    ComPtr<ID3D11ShaderResourceView> detail_view;
    hr(p.device->CreateShaderResourceView(details.Get(), nullptr, &detail_view), "CreateTerrainDetailView");
    p.terrain_details = std::move(detail_view);
    std::array<std::uint32_t, 256> slots{};
    std::ranges::copy(materials.tile_details, slots.begin());
    D3D11_BUFFER_DESC buffer{sizeof(slots), D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA initial{slots.data(), 0, 0};
    p.tile_details.Reset();
    hr(p.device->CreateBuffer(&buffer, &initial, &p.tile_details), "CreateTileDetails");
}
void Renderer::set_terrain_edge(const ModelAsset::Texture* texture) {
    auto& p = *impl_;
    p.terrain_edge.Reset();
    if (!texture)
        return;
    const auto& image = texture->image;
    if (!image.width || !image.height ||
        image.rgba.size() != static_cast<std::size_t>(image.width) * image.height)
        throw Error("Invalid edge texture");
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = image.width;
    desc.Height = image.height;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    ComPtr<ID3D11Texture2D> gpu;
    hr(p.device->CreateTexture2D(&desc, nullptr, &gpu), "CreateEdgeTexture");
    p.context->UpdateSubresource(gpu.Get(), 0, nullptr, image.rgba.data(), image.width * 4, 0);
    hr(p.device->CreateShaderResourceView(gpu.Get(), nullptr, &p.terrain_edge), "CreateEdgeTextureView");
    p.context->GenerateMips(p.terrain_edge.Get());
}
void Renderer::set_terrain_lod(const Terrain& terrain) {
    auto& p = *impl_;
    p.widget_terrain = terrain;
    p.reset(terrain);
    if (!p.lod) return;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 256;
    desc.MipLevels = 1;
    desc.ArraySize = 6;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    p.window_texture.Reset();
    p.terrain_windows.Reset();
    hr(p.device->CreateTexture2D(&desc, nullptr, &p.window_texture), "CreateTerrainWindows");
    hr(p.device->CreateShaderResourceView(p.window_texture.Get(), nullptr, &p.terrain_windows),
       "CreateTerrainWindowView");

}
void Renderer::update_terrain(const Terrain& terrain) {
    auto& p = *impl_;
    // The tiled mesh only draws without a land view (Renderer::draw); with one, the view is patched in place.
    if (p.lod && p.terrain_tiles && p.widget_terrain && p.refresh(terrain)) {
        p.widget_terrain = terrain;
        return;
    }
    set_mesh(terrain.mesh(true));
    set_terrain_lod(terrain);
}
void Renderer::set_scene(const MapScene& scene) {
    clear_scene();
    append_scene(scene);
}
void Renderer::append_scene(const MapScene& scene) {
    auto& p = *impl_;
    p.set_clearings(scene.clearings);
    if (scene.models.size() < p.scene_batch.size())
        throw Error("Dynamic scene model list shrank without a scene reset");
    const auto first = p.scene_batch.size();
    p.scene_batch.reserve(scene.models.size());
    for (std::size_t m = first; m < scene.models.size(); ++m) {
        const auto& model = scene.models[m];
        if (model.placements.empty() || (model.asset.mesh.indices.empty() && !model.character && !model.summon)) {
            p.scene_batch.push_back(SIZE_MAX);
            continue;
        }
        Impl::SceneBatch batch;
        batch.visible = model.visible;
        if (!batch.visible.empty() && batch.visible.size() != model.placements.size())
            throw Error("Scene placement visibility size mismatch");
        batch.reveal = model.reveal;
        batch.reveal.resize(model.placements.size(), 1);
        batch.reveal_blend.resize(model.placements.size());
        batch.reveal_min = model.asset.geometry.bounds.minimum.z;
        batch.reveal_max = model.asset.geometry.bounds.maximum.z;
        Mesh grouped;
        batch.materials = Impl::prepare_model(p.device.Get(), p.context.Get(), model.asset, grouped);
        if (model.widget_field) { // Room for the Remastered Look's flora and drawing-only plant copies.
            const auto copies = static_cast<unsigned>(tuning::widget_copies);
            // Flora is instanced (FloraInstance), so only the original model's expanded vertices need room.
            grouped.vertices.resize(std::max(grouped.vertices.size(), model.widget_field->vertex_capacity(copies, false)));
            grouped.indices.resize(std::max(grouped.indices.size(), model.widget_field->index_capacity(copies, false)));
        }
        if (grouped.vertices.size() > UINT_MAX / sizeof(Vertex) ||
            grouped.indices.size() > UINT_MAX / sizeof(std::uint32_t))
            throw Error("Scene model exceeds GPU buffer limit");
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = static_cast<UINT>(grouped.vertices.size() * sizeof(Vertex));
        const bool dynamic =
            model.soul || model.character || model.summon || model.widget_field || model.spell_visual;
        desc.Usage = dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        desc.CPUAccessFlags = dynamic ? D3D11_CPU_ACCESS_WRITE : 0;
        D3D11_SUBRESOURCE_DATA data{grouped.vertices.data()};
        hr(p.device->CreateBuffer(&desc, &data, &batch.vertices), "UploadSceneVertices");
        batch.vertex_count = grouped.vertices.size();
        batch.dynamic_mesh =
            model.character.has_value() || model.summon.has_value() || model.spell_visual.has_value();
        batch.index_capacity =
            model.character ? model.character->topology->triangles.size() * 3 :
            model.summon ? model.summon->topology->triangles.size() * 3 : grouped.indices.size();
        if (batch.index_capacity > UINT_MAX / sizeof(std::uint32_t))
            throw Error("Dynamic scene index capacity exceeds GPU buffer limit");
        const bool dynamic_indices = model.character || model.summon || model.widget_field || model.spell_visual;
        desc.Usage = dynamic_indices ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_IMMUTABLE;
        desc.CPUAccessFlags = dynamic_indices ? D3D11_CPU_ACCESS_WRITE : 0;
        desc.ByteWidth =
            static_cast<UINT>(std::max(batch.index_capacity, std::size_t{1}) * sizeof(std::uint32_t));
        desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
        auto padded_indices = grouped.indices;
        padded_indices.resize(std::max(batch.index_capacity, std::size_t{1}));
        data.pSysMem = padded_indices.data();
        hr(p.device->CreateBuffer(&desc, &data, &batch.indices), "UploadSceneIndices");
        if (batch.dynamic_mesh)
            batch.dynamic_indices = grouped.indices;
        batch.screen_oriented = model.screen_oriented;
        batch.soul = static_cast<bool>(model.soul);
        batch.widget_range = model.widget_range;
        batch.widget_fade_flags = model.widget_fade_flags;
        batch.widget_field = model.widget_field;
        const auto& geometry = model.asset.geometry;
        if (!geometry.frames.empty() && !model.soul && !model.screen_oriented && model.widget_range == 0 &&
            geometry.minimum_vertices < geometry.frames[0].size()) {
            batch.mrm = std::make_shared<const MrmGeometry>(geometry);
            XMFLOAT3 low{FLT_MAX, FLT_MAX, FLT_MAX}, high{-FLT_MAX, -FLT_MAX, -FLT_MAX};
            for (const auto& v : geometry.frames[0]) {
                low = {std::min(low.x, v.position.x), std::min(low.y, v.position.y),
                       std::min(low.z, v.position.z)};
                high = {std::max(high.x, v.position.x), std::max(high.y, v.position.y),
                        std::max(high.z, v.position.z)};
            }
            batch.box_min = low;
            batch.box_max = high;
            // SetDetailLevel takes the serialized class box at MRMDATA+44, not the posed vertex box.
            const auto& native_box = geometry.bounds;
            batch.detail_radius =
                std::sqrt(std::min(0.4f * std::max({native_box.maximum.x - native_box.minimum.x,
                    native_box.maximum.y - native_box.minimum.y, native_box.maximum.z - native_box.minimum.z}), 10.0f));
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
            desc.ByteWidth = static_cast<UINT>(grouped.indices.size() * sizeof(std::uint32_t));
            batch.lods.resize(model.placements.size());
            for (auto& lod : batch.lods) {
                hr(p.device->CreateBuffer(&desc, nullptr, &lod.indices), "CreateSceneLodIndices");
                lod.materials = batch.materials;
            }
        }
        for (const auto& placement : model.placements) {
            // Original Z rotation (x' = cx - sy, y' = sx + cy) expressed in renderer X/Z.
            const float c = std::cos(placement.yaw) * placement.scale,
                        s = std::sin(placement.yaw) * placement.scale, k = placement.scale;
            XMFLOAT4X4 world{c,
                             0,
                             s,
                             0,
                             0,
                             k,
                             0,
                             0,
                             -s,
                             0,
                             c,
                             0,
                             placement.position.x,
                             placement.position.y,
                             placement.position.z,
                             1};
            batch.worlds.push_back(world);
        }
        batch.opacity.assign(batch.worlds.size(), 1);
        p.scene_batch.push_back(p.scene.size());
        p.scene.push_back(std::move(batch));
    }
}
void Renderer::clear_scene() {
    impl_->scene.clear();
    impl_->scene_batch.clear();
}
void Renderer::update_scene_texture(std::size_t model, void* texture, bool grayscale) {
    auto& p = *impl_;
    if (model >= p.scene_batch.size() || p.scene_batch[model] == SIZE_MAX)
        return;
    p.scene[p.scene_batch[model]].texture_override = static_cast<ID3D11ShaderResourceView*>(texture);
    p.scene[p.scene_batch[model]].grayscale = grayscale;
}
void Renderer::update_scene_visibility(std::size_t model, std::size_t placement, bool visible, float reveal,
                                       float opacity) {
    auto& p = *impl_;
    if (model >= p.scene_batch.size() || p.scene_batch[model] == SIZE_MAX)
        return;
    auto& batch = p.scene[p.scene_batch[model]];
    if (placement >= batch.worlds.size())
        throw Error("Scene visibility placement outside model");
    if (batch.visible.empty())
        batch.visible.resize(batch.worlds.size(), true);
    batch.visible[placement] = visible;
    batch.reveal[placement] = reveal;
    batch.opacity[placement] = opacity;
}
void Renderer::update_scene_transform(std::size_t model, const ObjectMotion& motion) {
    update_scene_transform(model, 0, motion);
}
void Renderer::update_scene_transform(std::size_t model, std::size_t placement, const ObjectMotion& motion,
                                      float scale) {
    auto& p = *impl_;
    if (model >= p.scene_batch.size() || p.scene_batch[model] == SIZE_MAX)
        return;
    auto& batch = p.scene[p.scene_batch[model]];
    if (placement >= batch.worlds.size())
        throw Error("Scene transform placement outside model");
    const auto& m = motion.orientation;
    const auto& v = motion.position;
    batch.worlds[placement] = {
        m[0] * scale, m[2] * scale, m[1] * scale, 0, m[6] * scale, m[8] * scale, m[7] * scale, 0,
        m[3] * scale, m[5] * scale, m[4] * scale, 0, v.x,          v.z,          v.y,          1};
}
void Renderer::update_scene_model(std::size_t model, const std::vector<Vertex>& vertices) {
    auto& p = *impl_;
    if (model >= p.scene_batch.size() || p.scene_batch[model] == SIZE_MAX)
        return;
    auto& batch = p.scene[p.scene_batch[model]];
    if (vertices.size() != batch.vertex_count)
        throw Error("Scene model vertex count changed");
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr(p.context->Map(batch.vertices.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "MapSceneModel");
    std::memcpy(mapped.pData, vertices.data(), vertices.size() * sizeof(Vertex));
    p.context->Unmap(batch.vertices.Get(), 0);
}
void Renderer::update_scene_model(std::size_t model, const Mesh& mesh) {
    auto& p = *impl_;
    if (model >= p.scene_batch.size() || p.scene_batch[model] == SIZE_MAX)
        return;
    auto& batch = p.scene[p.scene_batch[model]];
    if (!batch.dynamic_mesh || mesh.materials.size() != batch.materials.size() || mesh.indices.size() % 3 ||
        mesh.triangle_materials.size() != mesh.indices.size() / 3 ||
        mesh.indices.size() > batch.index_capacity)
        throw Error("Invalid dynamic scene topology");
    std::vector<std::uint32_t> indices;
    auto materials = batch.materials;
    for (std::size_t m = 0; m < materials.size(); ++m) {
        materials[m].first = static_cast<unsigned>(indices.size());
        for (std::size_t t = 0; t < mesh.triangle_materials.size(); ++t)
            if (mesh.triangle_materials[t] == m)
                indices.insert(indices.end(), mesh.indices.begin() + t * 3, mesh.indices.begin() + t * 3 + 3);
        materials[m].count = static_cast<unsigned>(indices.size()) - materials[m].first;
    }
    if (indices.size() != mesh.indices.size())
        throw Error("Invalid dynamic scene material index");
    update_scene_model(model, mesh.vertices);
    if (indices != batch.dynamic_indices) {
        if (!indices.empty()) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            hr(p.context->Map(batch.indices.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
               "MapSceneIndices");
            std::memcpy(mapped.pData, indices.data(), indices.size() * sizeof(std::uint32_t));
            p.context->Unmap(batch.indices.Get(), 0);
        }
        batch.dynamic_indices = std::move(indices);
        batch.materials = std::move(materials);
    }
}
void Renderer::update_environment(const Environment& environment) { impl_->environment = environment; }
void Renderer::set_environment(const Environment& environment, const ModelAsset& sky) {
    auto& p = *impl_;
    p.sky.reset();
    p.environment = environment;
    if (sky.mesh.indices.empty())
        return;
    Impl::SceneBatch batch;
    Mesh grouped;
    batch.materials = Impl::prepare_model(p.device.Get(), p.context.Get(), sky, grouped);
    p.sky_slots.clear();
    for (const auto& material : sky.mesh.materials)
        p.sky_slots.push_back(material.texture.value - 0x736b7930u);
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = static_cast<UINT>(grouped.vertices.size() * sizeof(Vertex));
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_SUBRESOURCE_DATA data{grouped.vertices.data()};
    hr(p.device->CreateBuffer(&desc, &data, &batch.vertices), "UploadSkyVertices");
    p.sky_vertex_count = grouped.vertices.size();
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.CPUAccessFlags = 0;
    desc.ByteWidth = static_cast<UINT>(grouped.indices.size() * sizeof(std::uint32_t));
    desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    data.pSysMem = grouped.indices.data();
    hr(p.device->CreateBuffer(&desc, &data, &batch.indices), "UploadSkyIndices");
    p.sky = std::move(batch);
}
void Renderer::update_sky(const std::vector<Vertex>& vertices) {
    auto& p = *impl_;
    if (!p.sky)
        return;
    if (vertices.size() != p.sky_vertex_count)
        throw Error("Sky vertex count changed");
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr(p.context->Map(p.sky->vertices.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "MapSky");
    std::memcpy(mapped.pData, vertices.data(), vertices.size() * sizeof(Vertex));
    p.context->Unmap(p.sky->vertices.Get(), 0);
}
void Renderer::update_sky_texture(unsigned slot, const Image& image) {
    auto& p = *impl_;
    if (!p.sky)
        return;
    for (std::size_t i = 0; i < p.sky->materials.size(); ++i) {
        const auto& view = p.sky->materials[i].texture;
        if (p.sky_slots[i] != slot || !view)
            continue;
        ComPtr<ID3D11Resource> resource;
        view->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;
        hr(resource.As(&texture), "SkyTexture");
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (desc.Width != image.width || desc.Height != image.height ||
            image.rgba.size() != static_cast<std::size_t>(image.width) * image.height)
            throw Error("Sky video frame size changed");
        p.context->UpdateSubresource(texture.Get(), 0, nullptr, image.rgba.data(), image.width * 4, 0);
        if (desc.MipLevels != 1)
            p.context->GenerateMips(view.Get());
    }
}
void Renderer::clear_environment() {
    impl_->sky.reset();
    impl_->environment.reset();
}
void* Renderer::image_handle() const {
    return impl_->image.Get();
}
const RenderDetail& Renderer::detail() const {
    return impl_->detail;
}
void Renderer::set_detail(RenderDetail detail, bool widgets, int texture_filter, int terrain_detail) {
    impl_->detail = detail;
    impl_->terrain_detail = terrain_detail;
    impl_->widgets = widgets;
    const auto filter = std::clamp(texture_filter, 0, 2);
    impl_->sampler = impl_->clamp_samplers[filter];
    impl_->model_sampler = impl_->wrap_samplers[filter];
}
void Renderer::begin(unsigned w, unsigned h) {
    auto& p = *impl_;
    p.resize(w, h);
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    if (!w || !h)
        return;
    constexpr float clear[] = {0.055f, 0.075f, 0.095f, 1};
    p.context->ClearRenderTargetView(p.target.Get(), clear);
    if (p.remastered)
        p.context->ClearRenderTargetView(p.hdr_target.Get(), clear);
    p.scene_drawn = false;
    p.gpu_timer.begin(p.device.Get(), p.context.Get());
    p.context->ClearDepthStencilView(p.depth.Get(), D3D11_CLEAR_DEPTH, 0, 0);
    auto* target = p.scene_target();
    p.context->OMSetRenderTargets(1, &target, p.depth.Get());
    D3D11_VIEWPORT vp{0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
    p.context->RSSetViewports(1, &vp);
}
void Renderer::draw(const Camera& camera, bool wireframe, bool model_materials, bool scene,
                    bool environment) {
    auto& p = *impl_;
    const CpuTimer draw_timer{p.cpu_draw};
    const auto* env = environment && p.environment ? &*p.environment : nullptr;
    if ((!p.index_count && (!scene || p.scene.empty())) || !p.width || !p.height)
        return;
    const float half_angle_width = 45.0f * std::min(1.0f, 0.75f * p.width / p.height),
                half_angle_height = 45.0f * std::min(static_cast<float>(p.height) / p.width, 0.75f);
    p.view_tan[0] = std::tan(XMConvertToRadians(half_angle_width));
    p.view_tan[1] = std::tan(XMConvertToRadians(half_angle_height));
    auto eye = XMVectorSet(camera.position.x, camera.position.y, camera.position.z, 1);
    auto direction = XMVectorSet(std::sin(camera.yaw) * std::cos(camera.pitch), std::sin(camera.pitch),
                                 std::cos(camera.yaw) * std::cos(camera.pitch), 0);
    auto view = XMMatrixLookToLH(eye, direction, XMVectorSet(0, 1, 0, 0));
    // Sacrifice.exe 0x47fb30 sets the RENDERWINDOW half-angles from 45 degrees: horizontal 45 x min(1,
    // 0.75 w / h), vertical 45 x min(h / w, 0.75), linear in degrees. The engine projects (GetRay 0x18006430)
    // and clips with their tangents independently, so pixels are not square.
    const float aspect = static_cast<float>(p.height) / p.width,
                half_width = 45.0f * std::min(1.0f, 0.75f / aspect),
                half_height = 45.0f * std::min(aspect, 0.75f);
    // Sacrifice.exe 0x40ad20 gives the game window near 0.1 (RENDERWINDOW +32; far 2500 at +36). The earlier
    // 0.01 (_3dngn's default window) cost the 24-bit depth buffer a factor of ten, and capes, statues and
    // effects just inside models flickered through at a distance.
    const float near_plane = 0.1f, tan_width = std::tan(XMConvertToRadians(half_width)) * near_plane,
                tan_height = std::tan(XMConvertToRadians(half_height)) * near_plane;
    const float shift_x = camera.centre_offset.x * 2 * tan_width / static_cast<float>(p.width),
                shift_y = camera.centre_offset.y * 2 * tan_height / static_cast<float>(p.height);
    auto projection = XMMatrixPerspectiveOffCenterLH(-tan_width - shift_x, tan_width - shift_x,
                                                     -tan_height + shift_y, tan_height + shift_y, near_plane,
                                                     30000.0f) *
                      // Reversed Z (z' = w - z): 1 at the near plane, 0 at the far plane.
                      XMMATRIX(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, 0, 0, 1, 1);
    Constants c;
    c.point_options.x = scene ? static_cast<UINT>(p.point_sources.size()) : 0;
    if (c.point_options.x) {
        ID3D11ShaderResourceView* empty = nullptr;
        p.context->VSSetShaderResources(5, 1, &empty);
        if (p.point_sources.size() > p.point_capacity) {
            p.point_capacity = std::max(p.point_sources.size(), p.point_capacity * 2);
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = static_cast<UINT>(p.point_capacity * sizeof(Impl::PointSource));
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            desc.StructureByteStride = sizeof(Impl::PointSource);
            p.point_view.Reset();
            p.point_buffer.Reset();
            hr(p.device->CreateBuffer(&desc, nullptr, &p.point_buffer), "CreatePointLights");
            D3D11_SHADER_RESOURCE_VIEW_DESC view_desc{};
            view_desc.Format = DXGI_FORMAT_UNKNOWN;
            view_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            view_desc.Buffer.NumElements = static_cast<UINT>(p.point_capacity);
            hr(p.device->CreateShaderResourceView(p.point_buffer.Get(), &view_desc, &p.point_view),
               "CreatePointLightView");
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr(p.context->Map(p.point_buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "MapPointLights");
        std::memcpy(mapped.pData, p.point_sources.data(), p.point_sources.size() * sizeof(Impl::PointSource));
        p.context->Unmap(p.point_buffer.Get(), 0);
    }
    auto* points = c.point_options.x ? p.point_view.Get() : nullptr;
    p.context->VSSetShaderResources(5, 1, &points);
    float detail_outer = detail_range;
    XMStoreFloat4x4(&c.view_projection, XMMatrixTranspose(view * projection));
    XMStoreFloat4x4(&c.world, XMMatrixIdentity());
    c.options = {p.image ? 1.0f : 0.0f, p.terrain_tiles ? 1.0f : 0.0f, 0,
                 p.terrain_details && p.terrain_detail ? detail_range : 0};
    c.eye = {camera.position.x, camera.position.y, camera.position.z, 1};
    if (env) {
        // 0x456b80/0x456e40: ambient color x intensity, a directional sun of the ENVI color x intensity
        // shining along the negated ENVI vector, and LAND::SetFogging distances with a D3DCOLOR fog color.
        // 3d_d3d.dll 0x18003e80 sets D3D table fog: LINEAR for mode 0 (every stock map), EXP with the density
        // otherwise, over eye depth (W fog) rather than range.
        const auto& e = *env;
        c.fog = {e.fog_start, e.fog_end, 1, e.fog_mode ? 1.0f : 0.0f};
        XMStoreFloat4(&c.view_dir, XMVector3Normalize(direction));
        c.view_dir.w = e.fog_density;
        c.fog_color = d3dcolor(e.fog_color);
        c.ambient = {e.ambient.x * e.ambient_intensity, e.ambient.y * e.ambient_intensity,
                     e.ambient.z * e.ambient_intensity, 1};
        XMStoreFloat4(&c.sun_direction, XMVector3Normalize(XMVectorSet(e.sun_direction.x, e.sun_direction.z,
                                                                       e.sun_direction.y, 0)));
        c.sun_color = {e.sun_color.x * e.sun_intensity, e.sun_color.y * e.sun_intensity,
                       e.sun_color.z * e.sun_intensity, 1};
        // ILANDVIEW hands the LAND sky color to the device with the view (0x1603b500); used as clear color.
        const auto clear = d3dcolor(e.sky_color);
        p.context->ClearRenderTargetView(p.scene_target(), &clear.x);
        if (p.remastered) {
            const auto fog = renderer_shared::remaster_constants(p.widget_terrain ? &*p.widget_terrain : nullptr);
            c.remaster = {fog.enabled, fog.sky, fog.floor, fog.falloff};
            c.weather = {p.weather.wet, p.weather.snow, p.weather.storm, p.weather.flash};
            c.wind = renderer_shared::wind_constants(p.weather);
        }
    }
    // Remastered sun shadows: last scene draw's casters, drawn from this frame's sun view.
    const bool shadows = scene && env && p.remastered && c.sun_color.w > 0 && p.shadow_target;
    c.shadow = {};
    if (shadows) {
        const float ground = p.widget_terrain ? terrain_height(*p.widget_terrain, camera.position.x, camera.position.z)
                                              : camera.position.y - 40;
        XMStoreFloat4x4(&c.shadow_matrix,
                        XMMatrixTranspose(renderer_shared::sun_shadow_matrix(c.sun_direction, camera, ground)));
        c.shadow = renderer_shared::shadow_constants();
        ID3D11ShaderResourceView* none = nullptr;
        p.context->PSSetShaderResources(6, 1, &none);
        ComPtr<ID3D11RenderTargetView> old_target;
        ComPtr<ID3D11DepthStencilView> old_depth;
        p.context->OMGetRenderTargets(1, &old_target, &old_depth);
        UINT viewports = 1;
        D3D11_VIEWPORT old_viewport{};
        p.context->RSGetViewports(&viewports, &old_viewport);
        p.gpu_timer.mark(GpuTimer::shadow);
        p.caster_count += static_cast<double>(p.casters.size());
        p.context->OMSetRenderTargets(0, nullptr, p.shadow_target.Get());
        p.context->ClearDepthStencilView(p.shadow_target.Get(), D3D11_CLEAR_DEPTH, 1, 0);
        const D3D11_VIEWPORT viewport{0, 0, tuning::shadow_map_size, tuning::shadow_map_size, 0, 1};
        p.context->RSSetViewports(1, &viewport);
        p.context->RSSetState(p.shadow_raster.Get());
        p.context->OMSetDepthStencilState(p.shadow_depth.Get(), 0);
        p.context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        p.context->IASetInputLayout(p.layout.Get());
        p.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        p.context->VSSetShader(p.shadow_vs.Get(), nullptr, 0);
        p.context->PSSetShader(p.shadow_ps.Get(), nullptr, 0);
        auto* constants = p.constants.Get();
        p.context->VSSetConstantBuffers(0, 1, &constants);
        p.context->PSSetConstantBuffers(0, 1, &constants);
        auto* wrap = p.model_sampler.Get();
        p.context->PSSetSamplers(0, 1, &wrap);
        UINT caster_stride = sizeof(Vertex), caster_offset = 0;
        bool flora_state = false;
        for (const auto& caster : p.casters) {
            if (flora_state != static_cast<bool>(caster.instances)) {
                flora_state = static_cast<bool>(caster.instances);
                p.context->IASetInputLayout(flora_state ? p.flora_layout.Get() : p.layout.Get());
                p.context->VSSetShader(flora_state ? p.flora_shadow_vs.Get() : p.shadow_vs.Get(), nullptr, 0);
            }
            auto* vertices = caster.vertices.Get();
            p.context->IASetVertexBuffers(0, 1, &vertices, &caster_stride, &caster_offset);
            if (caster.instances) {
                auto* instances = caster.instances.Get();
                UINT instance_stride = sizeof(FloraInstance), instance_offset = 0;
                p.context->IASetVertexBuffers(1, 1, &instances, &instance_stride, &instance_offset);
            }
            auto* texture = caster.texture.Get();
            p.context->PSSetShaderResources(0, 1, &texture);
            auto k = c;
            k.world = caster.world;
            k.options = {texture ? 1.0f : 0.0f, 0, 0, 1};
            k.material = {caster.flags, 1, 0, 0};
            p.context->UpdateSubresource(p.constants.Get(), 0, nullptr, &k, 0, 0);
            if (caster.instances) {
                p.context->IASetIndexBuffer(caster.indices.Get(), DXGI_FORMAT_R32_UINT, 0);
                p.context->DrawIndexedInstanced(caster.count, caster.instance_count, caster.first, caster.base_vertex,
                                                caster.instance_first);
            } else if (caster.indices) {
                p.context->IASetIndexBuffer(caster.indices.Get(), DXGI_FORMAT_R32_UINT, 0);
                p.context->DrawIndexed(caster.count, caster.first, 0);
            } else
                p.context->Draw(caster.count, caster.first);
        }
        if (flora_state) {
            p.context->IASetInputLayout(p.layout.Get());
            p.context->VSSetShader(p.shadow_vs.Get(), nullptr, 0);
        }
        auto* target = old_target.Get();
        p.context->OMSetRenderTargets(1, &target, old_depth.Get());
        p.context->RSSetViewports(1, &old_viewport);
        p.casters.clear(); // Other draws (previews, no environment) keep the scene's casters.
    }
    p.shadow_recording = shadows;
    p.scene_drawn = true;
    p.scene_environment = env != nullptr;
    UINT stride = sizeof(Vertex), offset = 0;
    p.context->IASetInputLayout(p.layout.Get());
    p.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    p.context->VSSetShader(p.vs.Get(), nullptr, 0);
    p.context->PSSetShader(p.ps.Get(), nullptr, 0);
    auto* cb = p.constants.Get();
    p.context->VSSetConstantBuffers(0, 1, &cb);
    ID3D11Buffer* buffers[] = {cb, p.tile_details.Get()};
    p.context->PSSetConstantBuffers(0, 2, buffers);
    if (shadows) {
        auto* view = p.shadow_view.Get();
        p.context->PSSetShaderResources(6, 1, &view);
        auto* compare = p.shadow_sampler.Get();
        p.context->PSSetSamplers(1, 1, &compare);
    }
    if (p.lod && p.terrain_tiles) {
        p.range = p.remastered ? tuning::terrain_detail_range : detail_range;
        const bool rebuilt = p.update(camera, half_width, half_height, static_cast<bool>(p.terrain_edge));
        for (unsigned i = 0; i < 6; ++i)
            if (p.window_dirty[i])
                p.context->UpdateSubresource(p.window_texture.Get(), i, nullptr, p.window_pixels[i].data(), 256 * 4, 0);
        if (rebuilt) {
            if (p.lod_faces.size() > p.lod_capacity) {
                p.lod_capacity = p.lod_faces.size() * 2;
                D3D11_BUFFER_DESC desc{};
                desc.ByteWidth = static_cast<UINT>(p.lod_capacity * sizeof(Vertex));
                desc.Usage = D3D11_USAGE_DYNAMIC;
                desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
                p.lod_vertices.Reset();
                hr(p.device->CreateBuffer(&desc, nullptr, &p.lod_vertices), "CreateLandVertices");
            }
            if (!p.lod_faces.empty()) {
                D3D11_MAPPED_SUBRESOURCE mapped{};
                hr(p.context->Map(p.lod_vertices.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
                   "MapLandVertices");
                std::memcpy(mapped.pData, p.lod_faces.data(), p.lod_faces.size() * sizeof(Vertex));
                p.context->Unmap(p.lod_vertices.Get(), 0);
            }
        }
        for (unsigned i = 0; i < 6; ++i) {
            const auto& w = p.composed[i];
            c.windows[i] = {w.x * 10.0f, w.y * 10.0f, 1.0f / (w.cells * 10.0f), 0};
        }
        c.windows[0].w = 1;
        c.windows[5].w = static_cast<float>(p.lod->size);
        // The detail light's outer range is 1/256 / view+100, i.e. the ramped range.
        // The Remastered Look's wider full-detail mesh keeps the original's detail texture fade, so the
        // overlay doesn't darken (and hide the tile colours of) ground further out than the original did.
        detail_outer = std::min(p.land.range, detail_range);
        c.options.w = p.terrain_details && p.terrain_detail ? detail_outer : 0;
    }
    {
        // LAND::EnableBumpMapping 0x16042450 (called by the detail pass with the camera rows): the horizontal
        // light 0.5 x right - forward, scaled to length 0.7071, goes to the texture factor as red and green
        // bytes (x 127.5 + 128) with blue 218, so it rises 45 degrees. The 'bmap' option enables the bump
        // path.
        const float sin_yaw = std::sin(camera.yaw), cos_yaw = std::cos(camera.yaw),
                    cos_pitch = std::cos(camera.pitch);
        float x = cos_yaw * 0.5f - sin_yaw * cos_pitch, y = -sin_yaw * 0.5f - cos_yaw * cos_pitch;
        if (const float length = std::sqrt(x * x + y * y); length != 0) {
            x *= 0.70709997f / length;
            y *= 0.70709997f / length;
        }
        const auto byte = [](float v) {
            return static_cast<float>(static_cast<int>(v * 127.5f + 128.0f)) / 255;
        };
        c.bump = {byte(x), byte(y), 218.0f / 255, p.terrain_detail == 2 ? 1.0f : 0.0f};
        if (c.remaster.x > 0 && c.sun_color.w > 0) // Remastered: the bump relief is lit from the sun.
            c.bump = renderer_shared::sun_bump(c.sun_direction, c.bump.w);
        // sub_1603AE60 places the light at the eye; sub_1603B080 raises it to at least the ground below plus
        // half the (unscaled) outer range.
        float ground = camera.position.y - detail_outer;
        if (p.lod && p.lod->size) {
            const auto n = static_cast<float>(p.lod->size);
            const auto pitch = p.lod->size + 1;
            const float gx = std::clamp(camera.position.x * 0.1f, 0.0f, n),
                        gy = std::clamp(camera.position.z * 0.1f, 0.0f, n);
            const auto x0 = std::min(static_cast<unsigned>(gx), p.lod->size - 1),
                       y0 = std::min(static_cast<unsigned>(gy), p.lod->size - 1);
            const float fx = gx - x0, fy = gy - y0;
            const auto h = [&](unsigned x, unsigned y) { return p.lod->heights[y * pitch + x]; };
            ground = (h(x0, y0) * (1 - fx) + h(x0 + 1, y0) * fx) * (1 - fy) +
                     (h(x0, y0 + 1) * (1 - fx) + h(x0 + 1, y0 + 1) * fx) * fy;
        }
        c.detail_light = {camera.position.x, std::max(camera.position.y, ground + detail_outer * 0.5f),
                          camera.position.z, 1};
    }
    ID3D11ShaderResourceView* images[] = {p.image.Get(), p.terrain_tiles.Get(), p.terrain_details.Get(),
                                          p.lod ? p.terrain_windows.Get() : nullptr, p.terrain_edge.Get()};
    p.context->PSSetShaderResources(0, 5, images);
    auto* sampler = p.sampler.Get();
    p.context->PSSetSamplers(0, 1, &sampler);
    p.context->RSSetState(wireframe ? p.wire.Get() : p.solid.Get());
    p.context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    p.context->OMSetDepthStencilState(p.depth_write.Get(), 0);
    // Same first-encounter material order as MRMCLASS::ConvertMaterials/MRMINSTANCE::Draw.
    // `widget_pass` 1/2 draws a widget field's near/fading list; the fading list uses `fade_flags`.
    // Remastered sun shadows: keep a drawn (or shadow-only) material as a caster for the next frame's map.
    const auto record_caster = [&](const Impl::Material& material, ID3D11ShaderResourceView* texture) {
        if (!p.shadow_recording || !renderer_shared::casts_shadow(material.flags))
            return;
        Impl::ShadowCaster caster;
        UINT bound_stride{}, bound_offset{}, index_offset{};
        DXGI_FORMAT format{};
        p.context->IAGetVertexBuffers(0, 1, caster.vertices.GetAddressOf(), &bound_stride, &bound_offset);
        p.context->IAGetIndexBuffer(caster.indices.GetAddressOf(), &format, &index_offset);
        caster.first = material.first;
        caster.count = material.count;
        caster.world = c.world;
        caster.texture = texture;
        caster.flags = material.flags;
        if (caster.vertices && caster.indices)
            p.casters.push_back(std::move(caster));
    };
    // `shadow_only` records the materials as sun shadow casters without drawing them (off-screen models).
    const auto draw_materials = [&](const std::vector<Impl::Material>& materials, unsigned widget_pass = 0,
                                    float widget_range = 0, std::uint32_t fade_flags = 0, float opacity = 1,
                                    ID3D11ShaderResourceView* texture_override = nullptr, bool grayscale = false,
                                    bool shadow_only = false) {
        auto* wrap = p.model_sampler.Get();
        p.context->PSSetSamplers(0, 1, &wrap);
        for (const auto& source : materials) {
            auto material = source;
            if (widget_pass == 2)
                material.flags = fade_flags;
            if (c.model_reveal.w > 0)
                material.flags |= (material.flags & 0x2000) ? 8 : 0x1008;
            if (opacity < 1)
                material.flags = (material.flags & ~0x7f00u) | 0x1008;
            auto* texture = texture_override ? texture_override : material.texture.Get();
            if (shadow_only) {
                record_caster(material, texture);
                continue;
            }
            p.context->PSSetShaderResources(0, 1, &texture);
            c.options = {texture ? 1.0f : 0.0f, grayscale ? 1.f : 0.f, widget_range * widget_range, opacity};
            c.material = {material.flags, 1, 0, widget_pass};
            p.context->UpdateSubresource(p.constants.Get(), 0, nullptr, &c, 0, 0);
            auto mode = material.flags & 0x7f00;
            if (mode && mode != 0x1000 && mode != 0x2000)
                throw Error("Unsupported model blend mode");
            p.context->OMSetBlendState(mode == 0x1000   ? p.alpha_blend.Get()
                                       : mode == 0x2000 ? p.additive_blend.Get()
                                                        : nullptr,
                                       nullptr, 0xffffffff);
            p.context->OMSetDepthStencilState(material.flags & 0x04000000   ? p.depth_off.Get()
                                              : material.flags & 0x02000000 ? p.depth_read.Get()
                                                                            : p.depth_write.Get(),
                                              0);
            const bool two_sided = (material.flags & 0x08000000) != 0;
            p.context->RSSetState(wireframe ? (two_sided ? p.wire.Get() : p.model_wire.Get())
                                            : (two_sided ? p.solid.Get() : p.model_solid.Get()));
            p.context->DrawIndexed(material.count, material.first, 0);
            record_caster(material, texture);
        }
    };
    // Remastered instanced flora (WidgetField::Frame::instances): per model, the near plants opaque, then the
    // fade band blended, as the expanded path's two widget passes; each range is kept as a sun shadow caster.
    const auto draw_flora = [&](Impl::SceneBatch& batch, const WidgetField::Frame& frame) {
        if (!batch.flora_vertices) {
            const auto mesh = batch.widget_field->flora_mesh();
            if (mesh.indices.empty())
                return;
            D3D11_BUFFER_DESC desc{static_cast<UINT>(mesh.vertices.size() * sizeof(Vertex)), D3D11_USAGE_IMMUTABLE,
                                   D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
            D3D11_SUBRESOURCE_DATA data{mesh.vertices.data()};
            hr(p.device->CreateBuffer(&desc, &data, &batch.flora_vertices), "UploadFloraVertices");
            desc.ByteWidth = static_cast<UINT>(mesh.indices.size() * sizeof(std::uint32_t));
            desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
            data.pSysMem = mesh.indices.data();
            hr(p.device->CreateBuffer(&desc, &data, &batch.flora_indices), "UploadFloraIndices");
            batch.flora_models = mesh.models;
        }
        if (frame.instances.size() > batch.flora_instance_capacity || !batch.flora_instances) {
            batch.flora_instance_capacity = std::max(frame.instances.size(), batch.flora_instance_capacity * 2);
            D3D11_BUFFER_DESC desc{static_cast<UINT>(batch.flora_instance_capacity * sizeof(FloraInstance)),
                                   D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
            batch.flora_instances.Reset();
            hr(p.device->CreateBuffer(&desc, nullptr, &batch.flora_instances), "CreateFloraInstances");
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr(p.context->Map(batch.flora_instances.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "MapFloraInstances");
        std::memcpy(mapped.pData, frame.instances.data(), frame.instances.size() * sizeof(FloraInstance));
        p.context->Unmap(batch.flora_instances.Get(), 0);
        ID3D11Buffer* streams[] = {batch.flora_vertices.Get(), batch.flora_instances.Get()};
        const UINT strides[] = {sizeof(Vertex), sizeof(FloraInstance)}, offsets[] = {0, 0};
        p.context->IASetVertexBuffers(0, 2, streams, strides, offsets);
        p.context->IASetIndexBuffer(batch.flora_indices.Get(), DXGI_FORMAT_R32_UINT, 0);
        p.context->IASetInputLayout(p.flora_layout.Get());
        p.context->VSSetShader(p.flora_vs.Get(), nullptr, 0);
        ID3D11ShaderResourceView* no_texture = nullptr;
        p.context->PSSetShaderResources(0, 1, &no_texture);
        c.model_reveal = {0, 0, 1, 0};
        c.options = {0, 0, 0, 1};
        p.context->RSSetState(wireframe ? p.wire.Get() : p.solid.Get()); // Two-sided (0x08000000).
        for (unsigned pass = 1; pass <= 2; ++pass) {
            const std::uint32_t flags = pass == 1 ? 0x08000001u : 0x08001009u;
            c.material = {flags, 1, 0, pass};
            p.context->UpdateSubresource(p.constants.Get(), 0, nullptr, &c, 0, 0);
            p.context->OMSetBlendState(pass == 1 ? nullptr : p.alpha_blend.Get(), nullptr, 0xffffffff);
            p.context->OMSetDepthStencilState(p.depth_write.Get(), 0);
            for (std::size_t m = 0; m < frame.models.size() && m < batch.flora_models.size(); ++m) {
                const auto& group = frame.models[m];
                const auto& range = batch.flora_models[m];
                const UINT first = pass == 1 ? group.first : group.first + group.near_count,
                           count = pass == 1 ? group.near_count : group.fading_count;
                if (!count || !range.index_count)
                    continue;
                p.context->DrawIndexedInstanced(range.index_count, count, range.first_index, range.base_vertex, first);
                if (p.shadow_recording && tuning::flora_shadows > 0 && renderer_shared::casts_shadow(flags)) {
                    Impl::ShadowCaster caster;
                    caster.vertices = batch.flora_vertices;
                    caster.indices = batch.flora_indices;
                    caster.instances = batch.flora_instances;
                    caster.first = range.first_index;
                    caster.count = range.index_count;
                    caster.base_vertex = range.base_vertex;
                    caster.instance_first = first;
                    caster.instance_count = count;
                    caster.world = c.world;
                    caster.flags = flags;
                    p.casters.push_back(std::move(caster));
                }
            }
        }
        p.context->IASetInputLayout(p.layout.Get());
        p.context->VSSetShader(p.vs.Get(), nullptr, 0);
        ID3D11Buffer* none[] = {nullptr};
        const UINT zero = 0;
        p.context->IASetVertexBuffers(1, 1, none, &zero, &zero);
    };
    if (env && p.sky) {
        // LAND::DrawSky runs before the terrain without lighting or fog; all its materials disable depth.
        auto* vb = p.sky->vertices.Get();
        p.context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        p.context->IASetIndexBuffer(p.sky->indices.Get(), DXGI_FORMAT_R32_UINT, 0);
        auto* wrap = p.model_sampler.Get();
        p.context->PSSetSamplers(0, 1, &wrap);
        for (const auto& material : p.sky->materials) {
            auto* texture = material.texture.Get();
            p.context->PSSetShaderResources(0, 1, &texture);
            c.options = {texture ? 1.0f : 0.0f, 0, 0, 1};
            c.material = {material.flags, 1, 1, 0};
            c.remaster.y = c.remaster.x; // Sky layers take the remastered haze and sun glow.
            p.context->UpdateSubresource(p.constants.Get(), 0, nullptr, &c, 0, 0);
            const auto mode = material.flags & 0x7f00;
            p.context->OMSetBlendState(mode == 0x1000   ? p.alpha_blend.Get()
                                       : mode == 0x2000 ? p.additive_blend.Get()
                                                        : nullptr,
                                       nullptr, 0xffffffff);
            p.context->OMSetDepthStencilState(p.depth_off.Get(), 0);
            // 0x1800a210 sets D3DCULL_NONE for 0x08000000 (UNDR) and CCW culling otherwise.
            const bool two_sided = (material.flags & 0x08000000) != 0;
            p.context->RSSetState(wireframe ? (two_sided ? p.wire.Get() : p.model_wire.Get())
                                            : (two_sided ? p.solid.Get() : p.model_solid.Get()));
            p.context->DrawIndexed(material.count, material.first, 0);
        }
        c.options = {p.image ? 1.0f : 0.0f, p.terrain_tiles ? 1.0f : 0.0f, 0,
                     p.terrain_details && p.terrain_detail ? detail_outer : 0};
        c.material = {};
        c.remaster.y = 0;
        p.context->PSSetShaderResources(0, 5, images);
        p.context->PSSetSamplers(0, 1, &sampler);
        p.context->RSSetState(wireframe ? p.wire.Get() : p.solid.Get());
        p.context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        p.context->OMSetDepthStencilState(p.depth_write.Get(), 0);
    }
    if (p.index_count) {
        auto* vb = p.vertices.Get();
        p.context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        p.context->IASetIndexBuffer(p.indices.Get(), DXGI_FORMAT_R32_UINT, 0);
        if (model_materials && !p.materials.empty())
            draw_materials(p.materials);
        else {
            // The neutral comparison view must not accidentally sample a previously selected preview image.
            if (!p.materials.empty()) {
                c.options = {};
                ID3D11ShaderResourceView* empty = nullptr;
                p.context->PSSetShaderResources(0, 1, &empty);
            }
            p.context->UpdateSubresource(p.constants.Get(), 0, nullptr, &c, 0, 0);
            p.gpu_timer.mark(GpuTimer::land);
            if (p.lod && p.terrain_tiles) {
                auto* land = p.lod_vertices.Get();
                p.context->IASetVertexBuffers(0, 1, &land, &stride, &offset);
                p.context->Draw(static_cast<UINT>(p.lod_faces.size()), 0);
                if (p.shadow_recording && tuning::terrain_shadows > 0)
                    p.casters.push_back({p.lod_vertices, nullptr, 0, static_cast<UINT>(p.lod_faces.size()), c.world,
                                         nullptr, 0});
            } else {
                p.context->DrawIndexed(p.index_count, 0, 0);
                if (p.shadow_recording && tuning::terrain_shadows > 0)
                    p.casters.push_back({p.vertices, p.indices, 0, p.index_count, c.world, nullptr, 0});
            }
        }
    }
    p.gpu_timer.mark(GpuTimer::other);
    if (!scene)
        return;
    p.gpu_timer.mark(GpuTimer::models);
    // Camera basis as XMMatrixLookToLH derives it, for screen-oriented models (FLAG::Draw copies the view
    // rotation and keeps the model position).
    const auto right = XMVector3Normalize(XMVector3Cross(XMVectorSet(0, 1, 0, 0), direction));
    const auto up = XMVector3Cross(direction, right);
    const auto view_projection = view * projection;
    // The view's side planes (left, right, bottom, top) in renderer space, normals inwards and unit length,
    // for culling foliage (Gribb and Hartmann, row-vector form: column 4 plus or minus column 1 or 2).
    std::array<std::array<float, 4>, 4> view_planes{};
    {
        XMFLOAT4X4 m;
        XMStoreFloat4x4(&m, view_projection);
        for (int i = 0; i < 4; ++i) {
            const int column = i / 2;
            const float sign = i % 2 ? -1.0f : 1.0f;
            auto& plane = view_planes[i];
            for (int k = 0; k < 4; ++k)
                plane[k] = m.m[k][3] + sign * m.m[k][column];
            const float length = std::sqrt(plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2]);
            for (auto& value : plane)
                value /= std::max(length, 1e-6f);
        }
    }
    // Souls blend without writing depth, so they draw after every model that does; otherwise vegetation
    // or buildings behind a soul, drawn later, would cover it.
    // Remastered grass layer: two-sided untextured foliage triangles in the widget pass, drawn with the
    // scene models before the souls. It casts no shadow.
    bool grass_drawn = !p.remastered || !p.widgets || !p.widget_terrain || tuning::grass_density <= 0;
    const auto draw_grass = [&] {
        grass_drawn = true;
        const CpuTimer grass_timer{p.cpu_grass};
        p.gpu_timer.mark(GpuTimer::foliage);
        struct Back {
            GpuTimer& timer;
            ~Back() { timer.mark(GpuTimer::models); }
        } back{p.gpu_timer};
        if (!p.grass_ready())
            return;
        if (p.grass_chunk_generation != p.grass_generation()) {
            p.grass_chunks.clear();
            p.grass_chunk_generation = p.grass_generation();
        }
        for (const auto key : p.take_dirty_grass_chunks()) // Land edits (Bore, scars) rebuild only their chunks.
            p.grass_chunks.erase(key);
        constexpr int cells = renderer_shared::TerrainView::grass_chunk_cells;
        const float reach = tuning::grass_range, span = cells * 10.0f;
        const float ex = camera.position.x, ez = camera.position.z;
        const float fx = std::sin(camera.yaw), fz = std::cos(camera.yaw);
        const int chunks = (p.grass_size() + cells - 1) / cells;
        // Forget chunks well out of reach, so the cache stays the size of the view.
        for (auto it = p.grass_chunks.begin(); it != p.grass_chunks.end();) {
            const float x = (static_cast<float>(it->first % 65536) + .5f) * span - ex,
                        z = (static_cast<float>(it->first / 65536) + .5f) * span - ez;
            const float keep = reach * 2 + span;
            it = x * x + z * z > keep * keep ? p.grass_chunks.erase(it) : std::next(it);
        }
        const int x0 = std::max(0, static_cast<int>(std::floor((ex - reach) / span))),
                  x1 = std::min(chunks - 1, static_cast<int>(std::floor((ex + reach) / span))),
                  y0 = std::max(0, static_cast<int>(std::floor((ez - reach) / span))),
                  y1 = std::min(chunks - 1, static_cast<int>(std::floor((ez + reach) / span)));
        XMStoreFloat4x4(&c.world, XMMatrixIdentity());
        c.model_reveal = {0, 0, 1, 0};
        c.grass = {ex, ez, reach, 1};
        const bool recording = p.shadow_recording;
        p.shadow_recording = false;
        for (int cy = y0; cy <= y1; ++cy)
            for (int cx = x0; cx <= x1; ++cx) {
                // Nearest point of the chunk to the camera; beyond the reach nothing in it shows.
                const float lx = cx * span, lz = cy * span;
                const float nx = std::clamp(ex, lx, lx + span) - ex, nz = std::clamp(ez, lz, lz + span) - ez;
                if (nx * nx + nz * nz >= reach * reach)
                    continue;
                bool ahead = nx * nx + nz * nz < 400;
                for (int k = 0; k < 4 && !ahead; ++k)
                    ahead = (lx + (k & 1) * span - ex) * fx + (lz + (k >> 1) * span - ez) * fz >= -10;
                if (!ahead)
                    continue; // Behind the camera.
                const auto key = static_cast<std::int64_t>(cy) * 65536 + cx;
                auto found = p.grass_chunks.find(key);
                if (found == p.grass_chunks.end()) {
                    Impl::GrassChunk chunk;
                    p.grass_chunk(*p.widget_terrain, cx, cy, p.grass_vertex_data, p.grass_index_data,
                                  chunk.rank_indices);
                    if (!p.grass_index_data.empty()) {
                        D3D11_BUFFER_DESC desc{static_cast<UINT>(p.grass_vertex_data.size() * sizeof(Vertex)),
                                               D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
                        D3D11_SUBRESOURCE_DATA data{p.grass_vertex_data.data()};
                        hr(p.device->CreateBuffer(&desc, &data, &chunk.vertices), "UploadGrassChunk");
                        desc.ByteWidth = static_cast<UINT>(p.grass_index_data.size() * sizeof(std::uint32_t));
                        desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
                        data.pSysMem = p.grass_index_data.data();
                        hr(p.device->CreateBuffer(&desc, &data, &chunk.indices), "UploadGrassChunkIndices");
                        chunk.index_count = static_cast<UINT>(p.grass_index_data.size());
                        chunk.low = FLT_MAX;
                        chunk.high = -FLT_MAX;
                        for (const auto& vertex : p.grass_vertex_data) {
                            chunk.low = std::min(chunk.low, vertex.position.y);
                            chunk.high = std::max(chunk.high, vertex.position.y);
                        }
                    }
                    found = p.grass_chunks.emplace(key, std::move(chunk)).first;
                }
                const auto& chunk = found->second;
                // Frustum culling: skipped when the box (widened for wind and fill) is outside a side plane.
                bool outside = false;
                for (const auto& plane : view_planes) {
                    const float x = plane[0] >= 0 ? lx + span : lx, y = plane[1] >= 0 ? chunk.high : chunk.low,
                                z = plane[2] >= 0 ? lz + span : lz;
                    outside = outside || plane[0] * x + plane[1] * y + plane[2] * z + plane[3] < -1.5f;
                }
                if (outside)
                    continue;
                // Only tufts ranked below the thinning line at the chunk's nearest point can show.
                const float keep = renderer_shared::TerrainView::grass_keep(std::sqrt(nx * nx + nz * nz) / reach);
                const UINT count = keep >= 1 ? chunk.index_count
                    : chunk.rank_indices[std::clamp(static_cast<int>(std::ceil(keep * 16)), 0, 16)];
                if (!count)
                    continue;
                p.stat_grass_triangles += count / 3.0;
                auto* chunk_vertices = chunk.vertices.Get();
                p.context->IASetVertexBuffers(0, 1, &chunk_vertices, &stride, &offset);
                p.context->IASetIndexBuffer(chunk.indices.Get(), DXGI_FORMAT_R32_UINT, 0);
                const std::vector<Impl::Material> grass{{0x08000000u | 1, 0, count, nullptr}};
                draw_materials(grass, 1);
            }
        c.grass = {};
        p.shadow_recording = recording;
    };
    for (const bool souls : {false, true})
    for (auto& batch : p.scene) {
        if (souls && !grass_drawn)
            draw_grass();
        if (batch.soul != souls)
            continue;
        auto* vb = batch.vertices.Get();
        p.context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        for (std::size_t i = 0; i < batch.worlds.size(); ++i) {
            if (!batch.visible.empty() && !batch.visible[i])
                continue;
            auto matrix = XMLoadFloat4x4(&batch.worlds[i]);
            if (batch.screen_oriented)
                matrix = XMMATRIX(right, up, XMVector3Normalize(direction), matrix.r[3]);
            XMStoreFloat4x4(&c.world, XMMatrixTranspose(matrix));
            const auto reveal_material = [&] {
                if (batch.reveal[i] != 1)
                    batch.reveal_blend[i] = true; // FillVertexBuffer leaves material +204 set after reveal.
                c.model_reveal = {batch.reveal_min, batch.reveal_max, batch.reveal[i],
                                  batch.reveal_blend[i] ? 1.0f : 0.0f};
            };
            if (batch.widget_field) {
                if (!p.widgets || !p.widget_terrain)
                    continue;
                const CpuTimer widget_timer{p.cpu_widgets};
                p.gpu_timer.mark(GpuTimer::foliage);
                struct Back {
                    GpuTimer& timer;
                    ~Back() { timer.mark(GpuTimer::models); }
                } back{p.gpu_timer};
                const Vec3 at{camera.position.x, camera.position.z, camera.position.y},
                           forward{std::sin(camera.yaw) * std::cos(camera.pitch),
                                   std::cos(camera.yaw) * std::cos(camera.pitch), std::sin(camera.pitch)};
                WidgetField::Extras extras{tuning::widget_range_scale,
                                                 static_cast<unsigned>(tuning::widget_copies),
                                                 tuning::widget_copy_max_height, tuning::widget_copy_spread, true,
                                                 true, tuning::widget_min_range, true, true};
                // Map space swaps the renderer's Y (height) and Z (map Y).
                for (int i = 0; i < 4; ++i)
                    extras.frustum[i] = {view_planes[i][0], view_planes[i][2], view_planes[i][1], view_planes[i][3]};
                const auto frame = batch.widget_field->draw(*p.widget_terrain, at, forward, p.detail.minimum,
                                                            p.widget_lights, p.remastered ? &extras : nullptr);
                p.stat_widget_vertices += static_cast<double>(frame.vertices.size());
                p.stat_widget_triangles += static_cast<double>(frame.triangles);
                if (!frame.instances.empty()) {
                    draw_flora(batch, frame);
                    continue;
                }
                if (frame.indices.empty())
                    continue;
                if (frame.vertices.size() > batch.vertex_count || frame.indices.size() > batch.index_capacity)
                    throw Error("Widget frame exceeds its placement capacity");
                D3D11_MAPPED_SUBRESOURCE mapped{};
                hr(p.context->Map(batch.vertices.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "MapWidgetVertices");
                std::memcpy(mapped.pData, frame.vertices.data(), frame.vertices.size() * sizeof(Vertex));
                p.context->Unmap(batch.vertices.Get(), 0);
                hr(p.context->Map(batch.indices.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "MapWidgetIndices");
                std::memcpy(mapped.pData, frame.indices.data(), frame.indices.size() * sizeof(std::uint32_t));
                p.context->Unmap(batch.indices.Get(), 0);
                p.context->IASetIndexBuffer(batch.indices.Get(), DXGI_FORMAT_R32_UINT, 0);
                c.model_reveal = {0, 0, 1, 0};
                auto lists = batch.materials;
                // Flora models are untextured and opaque, fading out blended.
                if (frame.flora)
                    lists[0].flags = 0x08000001;
                lists[0].first = 0;
                lists[0].count = frame.near_indices;
                if (lists[0].count)
                    draw_materials(lists, 1);
                lists[0].first = frame.near_indices;
                lists[0].count = static_cast<unsigned>(frame.indices.size()) - frame.near_indices;
                if (lists[0].count)
                    draw_materials(lists, 2, 0, frame.flora ? 0x08001009u : batch.widget_fade_flags);
                continue;
            }
            if (batch.mrm) {
                // MODEL::IsInView 0x1601af40: the placed box's corners in view space (_3dngn::IsInView
                // 0x18005e70) give the nearest depth and, from those in front of the near plane, a screen
                // rectangle; under 3 pixels either way the model is not drawn. The main camera passes
                // its adaptive GFXDETAILINFO (0x452c60), not MODEL's DefaultGFXDetail fallback.
                float nearest = FLT_MAX, left = FLT_MAX, top = FLT_MAX, right_edge = -FLT_MAX,
                      bottom = -FLT_MAX;
                for (unsigned corner = 0; corner < 8; ++corner) {
                    const auto local = XMVectorSet(corner & 1 ? batch.box_max.x : batch.box_min.x,
                                                   corner & 2 ? batch.box_max.y : batch.box_min.y,
                                                   corner & 4 ? batch.box_max.z : batch.box_min.z, 1);
                    const auto placed = XMVector3TransformCoord(local, matrix);
                    const float depth = XMVectorGetZ(XMVector3TransformCoord(placed, view));
                    nearest = std::min(nearest, depth);
                    if (depth < near_plane)
                        continue;
                    const auto screen = XMVector3TransformCoord(placed, view_projection);
                    const float x = (XMVectorGetX(screen) * 0.5f + 0.5f) * p.width,
                                y = (0.5f - XMVectorGetY(screen) * 0.5f) * p.height;
                    left = std::min(left, x);
                    right_edge = std::max(right_edge, x);
                    top = std::min(top, y);
                    bottom = std::max(bottom, y);
                }
                // Off screen (behind the camera, say), a model near the sun's shadowed square still casts.
                const bool on_screen = right_edge - left >= 3 && bottom - top >= 3;
                float lod_depth = nearest;
                if (!on_screen) {
                    const float dx = XMVectorGetX(matrix.r[3]) - camera.position.x,
                                dz = XMVectorGetZ(matrix.r[3]) - camera.position.z,
                                reach = tuning::shadow_range * 1.5f;
                    if (!p.shadow_recording || dx * dx + dz * dz > reach * reach)
                        continue;
                    lod_depth = std::sqrt(dx * dx + dz * dz);
                }
                const float quality = p.detail.quality(lod_depth, batch.detail_radius);
                // MRMINSTANCE::Draw 0x1601d7f0 truncates min + (max - min) x quality; UpdateLod adds each
                // frame's difference from the current count and rebuilds only when the sum reaches 16.
                const auto& geometry = *batch.mrm;
                const auto maximum = static_cast<unsigned>(geometry.frames[0].size());
                const auto target = geometry.minimum_vertices +
                                    static_cast<unsigned>((maximum - geometry.minimum_vertices) * quality);
                auto& lod = batch.lods[i];
                bool rebuild = lod.vertices == 0;
                if (!rebuild) {
                    lod.pending += static_cast<int>(target) - static_cast<int>(lod.vertices);
                    rebuild = std::abs(lod.pending) >= 16;
                }
                if (rebuild) {
                    lod.pending = 0;
                    lod.vertices = target;
                    std::vector<std::vector<std::uint32_t>> groups(lod.materials.size());
                    for (const auto face : geometry.active_faces(target)) {
                        const auto& f = geometry.faces[face];
                        groups.at(f.material)
                            .insert(groups[f.material].end(), {f.vertices[0], f.vertices[2], f.vertices[1]});
                    }
                    D3D11_MAPPED_SUBRESOURCE mapped{};
                    hr(p.context->Map(lod.indices.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
                       "MapSceneLod");
                    auto* out = static_cast<std::uint32_t*>(mapped.pData);
                    unsigned first = 0;
                    for (std::size_t m = 0; m < groups.size(); ++m) {
                        std::ranges::copy(groups[m], out + first);
                        lod.materials[m].first = first;
                        lod.materials[m].count = static_cast<unsigned>(groups[m].size());
                        first += lod.materials[m].count;
                    }
                    p.context->Unmap(lod.indices.Get(), 0);
                }
                p.context->IASetIndexBuffer(lod.indices.Get(), DXGI_FORMAT_R32_UINT, 0);
                reveal_material();
                draw_materials(lod.materials, 0, 0, 0, batch.opacity[i], batch.texture_override.Get(), batch.grayscale,
                               !on_screen);
                continue;
            }
            p.context->IASetIndexBuffer(batch.indices.Get(), DXGI_FORMAT_R32_UINT, 0);
            reveal_material();
            draw_materials(batch.materials, 0, 0, 0, batch.opacity[i], batch.texture_override.Get(), batch.grayscale);
        }
    }
    // PARTICLESYSTEM::Draw 0x16020f30: camera-aligned quads, or generator-aligned
    // quads for flag 0x40000 (0x160215c0). Keep the existing screen-particle depth gates.
    p.gpu_timer.mark(GpuTimer::effects);
    std::size_t vertices = 0, indices = 0;
    for (const auto& batch : p.particles) {
        vertices += batch.quads.size() * 4 + batch.mesh.vertices.size();
        indices += batch.quads.size() * 6 + batch.mesh.indices.size();
    }
    if (!indices)
        return;
    const auto total = std::max(vertices, indices);
    if (total > p.particle_capacity) {
        p.particle_capacity = std::max<std::size_t>(total, p.particle_capacity * 2);
        D3D11_BUFFER_DESC vd{static_cast<UINT>(p.particle_capacity * sizeof(Vertex)), D3D11_USAGE_DYNAMIC,
                             D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE};
        hr(p.device->CreateBuffer(&vd, nullptr, &p.particle_vertices), "CreateParticleVertices");
        D3D11_BUFFER_DESC id{static_cast<UINT>(p.particle_capacity * sizeof(std::uint32_t)), D3D11_USAGE_DYNAMIC,
                             D3D11_BIND_INDEX_BUFFER, D3D11_CPU_ACCESS_WRITE};
        hr(p.device->CreateBuffer(&id, nullptr, &p.particle_indices), "CreateParticleIndices");
    }
    XMFLOAT3 r{}, u{}, f{};
    XMStoreFloat3(&r, right);
    XMStoreFloat3(&u, up);
    XMStoreFloat3(&f, XMVector3Normalize(direction));
    const auto geometry = renderer_shared::expand_particles(p.particles, camera,
        {r.x, r.y, r.z}, {u.x, u.y, u.z}, {f.x, f.y, f.z}, near_plane);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr(p.context->Map(p.particle_vertices.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "MapParticles");
    std::memcpy(mapped.pData, geometry.vertices.data(), geometry.vertices.size()*sizeof(Vertex));
    p.context->Unmap(p.particle_vertices.Get(), 0);
    const auto& ordered_indices = geometry.indices;
    const auto& draws = geometry.ranges;
    hr(p.context->Map(p.particle_indices.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "MapEffectIndices");
    std::memcpy(mapped.pData, ordered_indices.data(), ordered_indices.size() * sizeof(std::uint32_t));
    p.context->Unmap(p.particle_indices.Get(), 0);
    auto* vb = p.particle_vertices.Get();
    p.context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    p.context->IASetIndexBuffer(p.particle_indices.Get(), DXGI_FORMAT_R32_UINT, 0);
    XMStoreFloat4x4(&c.world, XMMatrixIdentity());
    c.model_reveal = {};
    auto* wrap = p.model_sampler.Get();
    p.context->PSSetSamplers(0, 1, &wrap);
    // Remastered soft particles read a copy of the depth so far; the depth buffer itself stays bound.
    const bool soft = p.remastered && p.depth_copy;
    if (soft) {
        p.context->CopyResource(p.depth_copy.Get(), p.depth_texture.Get());
        auto* view = p.depth_copy_view.Get();
        p.context->PSSetShaderResources(5, 1, &view);
    }
    for (const auto& range : draws) {
        const auto flags = renderer_shared::effect_flags(range.batch->flags);
        auto* texture = static_cast<ID3D11ShaderResourceView*>(range.batch->texture);
        p.context->PSSetShaderResources(0, 1, &texture);
        c.options = {texture ? 1.0f : 0.0f, 0,
                     soft && renderer_shared::soft_particles(*range.batch, flags) ? 1.0f : 0.0f, 1};
        c.material = {flags, 1, 1, 0}; // Unlit and unfogged like the sky: particle colours are final.
        p.context->UpdateSubresource(p.constants.Get(), 0, nullptr, &c, 0, 0);
        const auto mode = flags & 0x7f00;
        p.context->OMSetBlendState(mode == 0x400    ? p.darken_blend.Get()
                                   : mode == 0x1000   ? p.alpha_blend.Get()
                                   : mode == 0x2000 ? p.additive_blend.Get()
                                                    : nullptr,
                                   nullptr, 0xffffffff);
        p.context->OMSetDepthStencilState(flags & 0x04000000   ? p.depth_off.Get()
                                          : flags & 0x02000000 ? p.depth_read.Get()
                                                               : p.depth_write.Get(),
                                          0);
        p.context->RSSetState(wireframe ? p.wire.Get() : p.solid.Get());
        p.context->DrawIndexed(range.count, range.first, 0);
    }
    if (soft) {
        ID3D11ShaderResourceView* none = nullptr;
        p.context->PSSetShaderResources(5, 1, &none);
    }
}
unsigned Renderer::scene_detail_vertices(std::size_t model, std::size_t placement) const {
    if (model < impl_->scene_batch.size()) {
        const auto batch = impl_->scene_batch[model];
        if (batch < impl_->scene.size() && placement < impl_->scene[batch].lods.size())
            return impl_->scene[batch].lods[placement].vertices;
    }
    return 0; // No previous drawn detail yet; CheckIntercept starts with full geometry.
}
void Renderer::set_particles(std::vector<ParticleBatch> batches) {
    impl_->particles = std::move(batches);
}
void Renderer::set_point_lights(std::span<const PointLight> lights, const Terrain& terrain) {
    auto& sources = impl_->point_sources;
    sources.clear();
    impl_->widget_lights.clear();
    const auto position = [](const PointLight& light) {
        return XMFLOAT4{light.position.x, light.position.z, light.position.y, light.outer};
    };
    const auto color = [](const PointLight& light) {
        return XMFLOAT4{light.color.x * light.intensity, light.color.y * light.intensity,
                        light.color.z * light.intensity, 0};
    };
    for (const auto& light : lights) {
        if (!light.enabled || !(light.outer > 0) || !(light.intensity > 0))
            continue;
        impl_->widget_lights.push_back(light);
        const auto ground = static_cast<float>(terrain_true_height(terrain, light.position.x, light.position.y));
        const auto land = light.for_terrain(ground);
        sources.push_back({position(light), color(light), position(land), color(land)});
    }
}
void Renderer::Impl::post_process() {
    if (!remastered || !scene_drawn || bloom.empty())
        return;
    ID3D11ShaderResourceView* none[4]{};
    context->PSSetShaderResources(0, 4, none);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(post_vs.Get(), nullptr, 0);
    context->RSSetState(solid.Get());
    context->OMSetDepthStencilState(depth_off.Get(), 0);
    auto* sampler_state = post_sampler.Get();
    context->PSSetSamplers(0, 1, &sampler_state);
    auto* constants_buffer = post_constants.Get();
    context->PSSetConstantBuffers(0, 1, &constants_buffer);
    const auto pass = [&](ID3D11RenderTargetView* out, unsigned w, unsigned h, ID3D11PixelShader* shader,
                          ID3D11ShaderResourceView* a, ID3D11ShaderResourceView* b, const post::Constants& k,
                          ID3D11BlendState* blend) {
        context->PSSetShaderResources(0, 4, none);
        context->OMSetRenderTargets(1, &out, nullptr);
        D3D11_VIEWPORT vp{0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
        context->RSSetViewports(1, &vp);
        context->UpdateSubresource(post_constants.Get(), 0, nullptr, &k, 0, 0);
        context->PSSetShader(shader, nullptr, 0);
        ID3D11ShaderResourceView* views[]{a, b, depth_view.Get(), shader == post_composite.Get() ? ao_view.Get() : nullptr};
        context->PSSetShaderResources(0, 4, views);
        context->OMSetBlendState(blend, nullptr, 0xffffffff);
        context->Draw(3, 0);
    };
    pass(bloom[0].target.Get(), bloom[0].width, bloom[0].height, post_down.Get(), hdr_view.Get(), nullptr,
         post::downsample(width, height, true), nullptr);
    for (std::size_t i = 1; i < bloom.size(); ++i)
        pass(bloom[i].target.Get(), bloom[i].width, bloom[i].height, post_down.Get(), bloom[i - 1].view.Get(),
             nullptr, post::downsample(bloom[i - 1].width, bloom[i - 1].height, false), nullptr);
    for (std::size_t i = bloom.size() - 1; i > 0; --i)
        pass(bloom[i - 1].target.Get(), bloom[i - 1].width, bloom[i - 1].height, post_up.Get(),
             bloom[i].view.Get(), nullptr, post::upsample(bloom[i].width, bloom[i].height), post_add.Get());
    pass(ao_target.Get(), width, height, post_ao.Get(), nullptr, nullptr,
         post::occlusion(width, height, view_tan[0], view_tan[1]), nullptr);
    pass(target.Get(), width, height, post_composite.Get(), hdr_view.Get(), bloom[0].view.Get(),
         post::look(scene_environment && environment ? &*environment : nullptr, static_cast<unsigned>(bloom.size()),
                    width, height, weather),
         nullptr);
    context->PSSetShaderResources(0, 4, none);
}
void Renderer::set_remastered(bool remastered) { impl_->remastered = remastered; }
void Renderer::set_weather(const WeatherLook& weather) { impl_->weather = weather; }
void Renderer::end(bool vsync, const std::filesystem::path& capture_path) {
    auto& p = *impl_;
    std::optional<CpuTimer> end_timer{std::in_place, p.cpu_end};
    ++p.timed_frames;
    if (p.width && p.height) {
        p.gpu_timer.mark(GpuTimer::post);
        p.post_process();
        auto* target = p.target.Get();
        p.context->OMSetRenderTargets(1, &target, p.depth.Get());
        D3D11_VIEWPORT vp{0, 0, static_cast<float>(p.width), static_cast<float>(p.height), 0, 1};
        p.context->RSSetViewports(1, &vp);
    }
    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    if (!capture_path.empty())
        capture(capture_path);
    p.gpu_timer.end();
    hr(impl_->swap->Present(vsync ? 1 : 0, 0), "Present");
    end_timer.reset();
}
RenderTimings Renderer::timings(bool reset) {
    auto& p = *impl_;
    RenderTimings t;
    t.frames = p.timed_frames;
    const double n = std::max(p.timed_frames, 1u);
    t.cpu_draw = static_cast<float>(p.cpu_draw / n);
    t.cpu_widgets = static_cast<float>(p.cpu_widgets / n);
    t.cpu_grass = static_cast<float>(p.cpu_grass / n);
    t.cpu_end = static_cast<float>(p.cpu_end / n);
    t.widget_vertices = static_cast<float>(p.stat_widget_vertices / n);
    t.widget_triangles = static_cast<float>(p.stat_widget_triangles / n);
    t.grass_vertices = static_cast<float>(p.stat_grass_vertices / n);
    t.grass_triangles = static_cast<float>(p.stat_grass_triangles / n);
    t.casters = static_cast<float>(p.caster_count / n);
    auto& g = p.gpu_timer;
    t.gpu = g.resolved > 0;
    const double m = std::max(g.resolved, 1u);
    t.gpu_other = static_cast<float>(g.sums[GpuTimer::other] / m);
    t.gpu_shadow = static_cast<float>(g.sums[GpuTimer::shadow] / m);
    t.gpu_land = static_cast<float>(g.sums[GpuTimer::land] / m);
    t.gpu_models = static_cast<float>(g.sums[GpuTimer::models] / m);
    t.gpu_foliage = static_cast<float>(g.sums[GpuTimer::foliage] / m);
    t.gpu_effects = static_cast<float>(g.sums[GpuTimer::effects] / m);
    t.gpu_post = static_cast<float>(g.sums[GpuTimer::post] / m);
    t.gpu_total = static_cast<float>(g.total / m);
    if (reset) {
        p.cpu_draw = p.cpu_widgets = p.cpu_grass = p.cpu_end = 0;
        p.stat_widget_vertices = p.stat_widget_triangles = p.stat_grass_vertices = p.stat_grass_triangles = p.caster_count = 0;
        p.timed_frames = 0;
        g.sums = {};
        g.total = 0;
        g.resolved = 0;
    }
    return t;
}
Image Renderer::frame_image() {
    auto& p = *impl_;
    ComPtr<ID3D11Texture2D> back;
    hr(p.swap->GetBuffer(0, IID_PPV_ARGS(&back)), "FrameBackbuffer");
    D3D11_TEXTURE2D_DESC d{};
    back->GetDesc(&d);
    d.BindFlags = 0;
    d.MiscFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.Usage = D3D11_USAGE_STAGING;
    ComPtr<ID3D11Texture2D> staging;
    hr(p.device->CreateTexture2D(&d, nullptr, &staging), "FrameStaging");
    p.context->CopyResource(staging.Get(), back.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr(p.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "FrameMap");
    Image image{d.Width, d.Height, std::vector<std::uint32_t>(std::size_t{d.Width} * d.Height)};
    const bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    for (unsigned y = 0; y < d.Height; ++y) {
        const auto* row = reinterpret_cast<const std::uint32_t*>(static_cast<const char*>(mapped.pData) + y * mapped.RowPitch);
        for (unsigned x = 0; x < d.Width; ++x) {
            auto v = row[x];
            if (bgra)
                v = (v & 0xff00ff00u) | ((v >> 16) & 0xff) | ((v & 0xff) << 16);
            image.rgba[std::size_t{y} * d.Width + x] = v | 0xff000000u;
        }
    }
    p.context->Unmap(staging.Get(), 0);
    return image;
}
void Renderer::capture(const std::filesystem::path& path) {
    auto& p = *impl_;
    ComPtr<ID3D11Texture2D> back;
    hr(p.swap->GetBuffer(0, IID_PPV_ARGS(&back)), "CaptureBackbuffer");
    D3D11_TEXTURE2D_DESC d{};
    back->GetDesc(&d);
    d.BindFlags = 0;
    d.MiscFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.Usage = D3D11_USAGE_STAGING;
    ComPtr<ID3D11Texture2D> staging;
    hr(p.device->CreateTexture2D(&d, nullptr, &staging), "CaptureStaging");
    p.context->CopyResource(staging.Get(), back.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr(p.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "CaptureMap");
    if (path.extension() == ".tga" || path.extension() == ".TGA") {
        // 0x40aa50 names shotNNNN.tga: uncompressed 32-bit BGRA, top-left origin.
        std::array<std::uint8_t, 18> header{};
        header[2] = 2;
        header[12] = static_cast<std::uint8_t>(d.Width), header[13] = static_cast<std::uint8_t>(d.Width >> 8);
        header[14] = static_cast<std::uint8_t>(d.Height), header[15] = static_cast<std::uint8_t>(d.Height >> 8);
        header[16] = 32;
        header[17] = 0x28;
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(header.data()), header.size());
        for (unsigned y = 0; y < d.Height; ++y)
            out.write(static_cast<const char*>(mapped.pData) + y * mapped.RowPitch, d.Width * 4);
        p.context->Unmap(staging.Get(), 0);
        if (!out)
            throw Error("Cannot write capture " + utf8(path));
        return;
    }
    BITMAPFILEHEADER file{};
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
    file.bfSize = file.bfOffBits + d.Width * d.Height * 4;
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = d.Width;
    info.biHeight = -static_cast<LONG>(d.Height);
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&file), sizeof(file));
    out.write(reinterpret_cast<const char*>(&info), sizeof(info));
    for (unsigned y = 0; y < d.Height; ++y)
        out.write(static_cast<const char*>(mapped.pData) + y * mapped.RowPitch, d.Width * 4);
    p.context->Unmap(staging.Get(), 0);
    if (!out)
        throw Error("Cannot write capture " + utf8(path));
}
} // namespace sacrifice
