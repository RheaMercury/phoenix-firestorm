/**
 * @file fsSkinSSSF.glsl
 *
 * Screen-space subsurface scattering for avatar skin (FSRenderSkinSSS).
 *
 * A separable, depth-aware blur in the style of Jimenez's separable SSS, run on avatar pixels
 * only, after the deferred sun and local lights and before alpha and tone mapping.
 *
 * What is blurred is the light, not the skin: each tap's lit colour is divided by its own
 * g-buffer albedo (linear, as the lighting used it), the resulting irradiance is blurred, and
 * the centre's albedo multiplies it back in (post-scatter texturing). Pores, freckles, makeup
 * and tattoos stay sharp; only the lighting diffuses.
 *
 * Specular highlights: the screen colour already includes them and there is no separate
 * specular buffer, so irradiance is clamped, by luminance, to a ceiling no diffuse surface can
 * exceed under the current sky (sun plus ambient) or exposure. A highlight's excess over that
 * ceiling never enters the blur, and the centre pixel keeps its own excess untouched: the blur
 * only replaces the clamped (diffuse) share of the centre, so highlights stay crisp and do not
 * smear into halos.
 *
 * Kernel: the d'Eon/Jimenez six-Gaussian skin diffusion profile, integrated per channel over
 * 15 importance-spaced taps (quadratic spacing out to 6 mm). Red reaches several millimetres,
 * green and blue stay under half a millimetre, which gives the soft red terminator and the
 * red-tinted shadow edges without softening the image. Widths are in millimetres on the
 * surface, scaled by sss_radius (1 = measured human skin), converted to pixels by distance and
 * projection.
 *
 * Which pixels: with the feature on, rigged (HAS_SKIN) and system avatar g-buffer writers bias
 * their g-buffer flag by FS_SKIN_FLAG_BIAS (see LLShaderMgr::loadShaderFile), which marks
 * "drawn as part of an avatar" without disturbing GET_GBUFFER_FLAG's 0.1 tolerance. Inside
 * that, a skin weight (fsSkinUtil.glsl: a tone test on the albedo, with
 * FSRenderSkinDetectMaterial also a material test) keeps the blur off most clothing.
 *
 * Debug view (sss_debug, FSRenderSkinDebug): pass 1 alone tints avatar pixels by that weight.
 *
 * Translucency (sss_translucency > 0, vertical pass only): sunlight transmitted through thin
 * skin lit from behind (ears, fingers, nostrils, rims), after Jimenez et al. 2010, "Real-Time
 * Realistic Skin Translucency". The thickness is the distance along the light between the
 * receiver (pushed slightly inside the surface) and the nearest caster in the sun's shadow map,
 * read as raw depth through the PCSS depth-fetch sampler (cascades 0 and 1, where close-ups
 * are); it goes through the skin profile's transmittance (red survives furthest) times the sun
 * colour, the albedo and max(0.3 + dot(-N, L), 0), times a forward scattering lobe (strongest
 * looking into the light through the skin, gathered on rims), with the thickness scaled down
 * toward silhouettes (fsSkinUtil.glsl).
 *
 * Skin polish (GPU Gems 3 ch. 14), each its own permutation:
 * FS_SKIN_ALBEDO_SPLIT (FSRenderSkinAlbedoSplit): part of the albedo goes in before the blur
 *   ("pre- and post-scatter texturing"): the light is divided by albedo^(1 - split / 2) and the
 *   centre multiplies the same power back, so at split 1 the blur carries irradiance times
 *   sqrt(albedo) and the colour of freckles, lips and makeup bleeds a little into the skin
 *   around them, as it does under real skin. Detail stays as sharp as the other half allows.
 * FS_SKIN_ENERGY (FSRenderSkinEnergy): light the specular surface film reflects never enters
 *   the skin (the chapter's rho_dt term). The light leaving the skin toward the eye is scaled by
 *   1 - the directional albedo of skin's specular (F0 0.028, Karis' analytic fit), which grows
 *   toward grazing angles, so rims don't glow brighter than the light allows. Applied to the
 *   scattered light of legacy-shaded skin (PBR lighting already takes 1 - Fresnel off each
 *   light's diffuse) and to the backlit translucency of all skin.
 * FS_SKIN_TAP_LOD (FSRenderSkinTapLOD): where the kernel reaches only a few pixels (faces a
 *   metre or two away and further), 3 taps per side stand in for the 7, their weights merged
 *   (sums per channel, red weighted offsets), so the profile keeps its energy: about half the
 *   cost in crowds for no visible change at that size.
 *
 * Pass 0 (horizontal): screen -> scratch. rgb = horizontally blurred clamped irradiance,
 *                      a = the centre's clamp factor (1 when nothing was clamped).
 * Pass 1 (vertical):   scratch -> screen, premultiplied blend (ONE, ONE_MINUS_SRC_ALPHA):
 *                      src = a * albedo * blurred, alpha = a * clamp factor, a = weight * strength,
 *                      which is lit + a * albedo * (blurred - clamped centre); the
 *                      translucency is added to src.
 */

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform sampler2D diffuseRect;      // g-buffer albedo
#ifdef FS_SKIN_DETECT_MATERIAL
uniform sampler2D specularRect;     // g-buffer specular / ORM, for the material score
#endif
uniform sampler2D altDiffuseMap;    // the lit screen (pass 0) or the scratch (pass 1)
uniform sampler2D exposureMap;

uniform vec2 screen_res;
uniform mat4 inv_proj;

uniform int sss_pass;               // 0 horizontal, 1 vertical
uniform float sss_strength;         // [0,1] how much of the scattered light replaces the lit skin
uniform float sss_radius;           // profile scale, 1 = measured human skin
uniform float sss_tint;             // 0: every channel scatters like red, 1: skin profile
uniform float sss_tone_match;       // 0: every avatar surface, 1: skin coloured albedo only
uniform float sss_max_px;           // cap on the kernel reach in pixels
uniform float exposure;             // RenderExposure, as the tone mapper uses it
uniform float sss_translucency;     // strength of the light transmitted through thin skin, 0 off
uniform int sss_debug;              // 1: pass 1 only tints avatar pixels by their skin weight
#ifdef FS_SKIN_ALBEDO_SPLIT
uniform float fs_skin_albedo_split; // 0: albedo after the blur only, 1: half before (sqrt), half after
#endif

// Sun shadow (cascades 0 and 1 as raw depth), for the translucency thickness
uniform sampler2D shadowDepth0;
uniform sampler2D shadowDepth1;
uniform mat4 shadow_matrix[6];
uniform vec4 shadow_clip;
uniform vec3 sun_dir;
uniform vec3 moon_dir;

// Sky (LLEnvironment), for the diffuse ceiling
uniform vec3  sunlight_color;
uniform vec3  moonlight_color;
uniform int   sun_up_factor;
uniform vec3  ambient_color;
uniform vec3  blue_density;
uniform float haze_density;
uniform float cloud_shadow;
uniform float density_multiplier;
uniform float max_y;
uniform vec3  lightnorm;
uniform float sky_sunlight_scale;
uniform float sky_ambient_scale;
uniform int   classic_mode;

vec4 getNormRaw(vec2 screenpos);
vec4 getNorm(vec2 screenpos);
vec4 getPosition(vec2 pos_screen);
vec3 linear_to_srgb(vec3 cl);
vec3 srgb_to_linear(vec3 cs);

// fsSkinUtil.glsl
int fsSkinSurface(float flag);
float fsSkinWeightRaw(int surface, vec4 base, vec4 spec, float env, out vec3 albedo);
float fsSkinWeightParts(int surface, vec4 base, vec4 spec, float env, out vec3 albedo, out float material, out float tone);
vec3 fsSkinTransmittance(float s);
float fsSkinForwardScatter(vec3 n, vec3 v, vec3 l);
float fsSkinRimThinning(vec3 n, vec3 v);
#ifdef FS_SKIN_ENERGY
float unpackRoughness(vec2 p);
#endif

#define FS_SKIN_TAPS 7              // per side
#define FS_SKIN_RANGE_MM 6.0        // reach of the outermost tap at sss_radius 1
#define FS_SKIN_TRANS_SHRINK 0.002  // m: the receiver is pushed this far inside the surface
#define FS_SKIN_TRANS_MM 6.0        // measured thickness (mm) per profile mm at sss_radius 1

// d'Eon/Jimenez skin profile, integrated over each tap's span: rgb = per channel weight
// (centre + 2 x sides sums to 1 per channel), a = tap offset in mm at sss_radius 1
const vec4 fs_skin_kernel[FS_SKIN_TAPS + 1] = vec4[FS_SKIN_TAPS + 1](
    vec4(0.1859, 0.3495, 0.4366, 0.0000),
    vec4(0.1482, 0.2462, 0.2510, 0.1224),
    vec4(0.1061, 0.0704, 0.0296, 0.4898),
    vec4(0.0783, 0.0080, 0.0009, 1.1020),
    vec4(0.0471, 0.0006, 0.0001, 1.9592),
    vec4(0.0193, 0.0001, 0.0000, 3.0612),
    vec4(0.0059, 0.0000, 0.0000, 4.4082),
    vec4(0.0020, 0.0000, 0.0000, 6.0000));

#ifdef FS_SKIN_TAP_LOD
// The kernel above merged into 3 taps per side, for reaches under FS_SKIN_LOD_PX pixels: taps 1-2,
// 3-4 and 5-7 summed per channel, at their red weighted mean offset. Centre unchanged.
#define FS_SKIN_LOD_TAPS 3
#define FS_SKIN_LOD_PX 6.0
const vec4 fs_skin_kernel_lod[FS_SKIN_LOD_TAPS + 1] = vec4[FS_SKIN_LOD_TAPS + 1](
    vec4(0.1859, 0.3495, 0.4366, 0.0000),
    vec4(0.2543, 0.3166, 0.2806, 0.2757),
    vec4(0.1254, 0.0086, 0.0010, 1.4240),
    vec4(0.0272, 0.0001, 0.0000, 3.5695));
#endif

float fsLum(vec3 c)
{
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

// A NaN or Inf anywhere in the lit screen (an overflowed half float highlight, a bad light) must
// never enter the blur: Inf / albedo times a clamp factor of 0 is NaN, and min/max/clamp with a
// NaN operand are undefined (NVIDIA tends to return the other operand, AMD the NaN), so a single
// bad pixel would spread over the whole kernel and blend black into the skin around it.
bool fsSkinBad(vec3 v)
{
    return any(isnan(v)) || any(isinf(v));
}

bool fsSkinBad(vec4 v)
{
    return any(isnan(v)) || any(isinf(v));
}

bool fsSkinBad(float v)
{
    return isnan(v) || isinf(v);
}

// Skin weight of a pixel in [0,1], or -1 when it is not an avatar surface (fsSkinUtil.glsl).
// albedo: the linear diffuse albedo the lighting multiplied by (legacy albedo is stored sRGB, PBR
// linear), floored.
float fsSkinSample(vec2 tc, out vec3 albedo)
{
    albedo = vec3(1.0);
    vec4 norm_raw = getNormRaw(tc);
    int surface = fsSkinSurface(norm_raw.a);
    if (surface == 0)
    {
        return -1.0;
    }
#ifdef FS_SKIN_DETECT_MATERIAL
    vec4 spec = texture(specularRect, tc);
#else
    vec4 spec = vec4(0.0); // only the material score reads it
#endif
    return fsSkinWeightRaw(surface, texture(diffuseRect, tc), spec, norm_raw.b, albedo);
}

// The part of the albedo the light is divided by before the blur and multiplied by after it:
// all of it (post-scatter texturing), or with FS_SKIN_ALBEDO_SPLIT down to its square root
vec3 fsSkinScatterAlbedo(vec3 albedo)
{
#ifdef FS_SKIN_ALBEDO_SPLIT
    return pow(albedo, vec3(1.0 - 0.5 * clamp(fs_skin_albedo_split, 0.0, 1.0)));
#else
    return albedo;
#endif
}

#ifdef FS_SKIN_ENERGY
// Share of the light that leaves the skin toward the eye rather than being turned back by its
// specular film: 1 - the directional albedo of a GGX lobe at skin's F0 0.028 for this view
// angle and roughness (Karis' analytic fit of the split-sum environment BRDF). About 0.96 seen
// head on, falling toward the silhouette.
float fsSkinEnergyKeep(vec3 n, vec3 pos, float roughness)
{
    vec3 v = -pos * inversesqrt(max(dot(pos, pos), 1e-12));
    float nv = clamp(dot(n, v), 0.0, 1.0);
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = clamp(roughness, 0.0, 1.0) * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * nv)) * r.x + r.y;
    vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;
    return 1.0 - clamp(0.028 * ab.x + ab.y, 0.0, 1.0);
}

// The skin's roughness where the g-buffer's material is read (FS_SKIN_DETECT_MATERIAL), else a
// typical value; as the skin specular takes it (fsSkinLegacyRoughness, fsSkinUtil.glsl)
float fsSkinEnergyRoughness(vec2 tc, int surface)
{
#ifdef FS_SKIN_DETECT_MATERIAL
    vec4 spec = texture(specularRect, tc);
    if (surface == 2)
    {
        return clamp(unpackRoughness(spec.ga), 0.045, 1.0);
    }
    return spec.a > 0.0 ? clamp(1.0 - spec.a, 0.3, 0.9) : 0.55;
#else
    return 0.55;
#endif
}
#endif

// Linear sun (or moon) colour at the ground, as calcAtmosphericVarsLinear gives the lighting
vec3 fsSkinSunlight()
{
    vec3 sun = (sun_up_factor == 1) ? sunlight_color : moonlight_color;
    vec3 light_atten = (blue_density + vec3(haze_density * 0.25)) * (density_multiplier * max_y);
    sun *= exp(-light_atten / max(1e-6, lightnorm.y));
    if (classic_mode < 1)
    {
        sun = srgb_to_linear(sun);
    }
    return sun * sky_sunlight_scale;
}

// Brightest irradiance (luminance) a diffuse surface can plausibly receive right now: full sun
// plus generous ambient, or what exposes to white, whichever is higher (the second covers
// scenes lit by local lights). Anything above it on skin is specular.
float fsSkinDiffuseCeiling()
{
    vec3 amb = ambient_color + (vec3(1.0) - ambient_color) * cloud_shadow * 0.5;
    amb = pow(max(amb, vec3(0.0)), vec3(0.9)) * 0.57;
    if (classic_mode < 1)
    {
        amb = srgb_to_linear(amb);
    }
    float sun_k = classic_mode < 1 ? 1.0 : 3.14159265; // classic PBR scales the sun by pi

    float env = fsLum(fsSkinSunlight()) * sun_k + 2.0 * fsLum(amb) * sky_ambient_scale;
    float map_expo = texture(exposureMap, vec2(0.5)).r;
    map_expo = (fsSkinBad(map_expo) || map_expo <= 0.0) ? 1.0 : map_expo;
    float expo = max(exposure * map_expo, 1e-4);
    float ceiling = 1.25 * max(env, 1.0 / expo);
    return (fsSkinBad(ceiling) || ceiling <= 0.0) ? 1.0 : min(ceiling, 60000.0);
}

// Irradiance clamped to the ceiling: luminance first (keeps the hue of the light), then a per
// channel cap so a floored albedo channel cannot blow up. e must be finite (see fsSkinBad).
vec3 fsSkinClamp(vec3 e, float ceiling, out float f)
{
    e = max(e, vec3(0.0));
    f = min(1.0, ceiling / max(fsLum(e), 1e-6));
    return min(e * f, vec3(3.0 * ceiling));
}


// Transmittance through the skin between p (view space) and the sun along l, from one shadow
// cascade read as raw depth: the depth difference to the nearest caster, over the map's depth
// per metre along the light at p (right for warped maps too). A 3 x 3 tent of taps a texel apart
// smooths the map's stair steps: a texel is a centimetre or two, about the size of an ear, so
// fewer taps lit whole parts at once with hard edges. Zero when p is outside the cascade.
vec3 fsSkinCascadeTransmittance(sampler2D depth_map, mat4 m, vec3 p, vec3 l, float scale)
{
    vec4 sp = m * vec4(p, 1.0);
    vec3 c = sp.xyz / sp.w;
    if (any(lessThan(c.xy, vec2(0.0))) || any(greaterThan(c.xy, vec2(1.0))) || c.z > 1.0)
    {
        return vec3(0.0);
    }
    vec4 sl = m * vec4(p + l * 0.1, 1.0);
    float z_per_m = max((c.z - sl.z / sl.w) / 0.1, 1e-6);

    vec2 texel = 1.0 / vec2(textureSize(depth_map, 0));
    vec3 t = vec3(0.0);
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            float k = (x == 0 ? 2.0 : 1.0) * (y == 0 ? 2.0 : 1.0); // tent: 4 centre, 2 edges, 1 corners
            float z_caster = textureLod(depth_map, c.xy + vec2(x, y) * texel, 0.0).r;
            float thickness_mm = max(c.z - z_caster, 0.0) / z_per_m * 1000.0;
            t += k * fsSkinTransmittance(thickness_mm * scale);
        }
    }
    return t / 16.0;
}

// Sunlight transmitted through thin skin at this pixel, before albedo
vec3 fsSkinTranslucency(vec3 pos, vec3 n)
{
    vec3 l = (sun_up_factor == 1) ? sun_dir : moon_dir;
    float l_len2 = dot(l, l);
    if (!(l_len2 > 1e-12))
    {
        return vec3(0.0); // no light direction (or not a number): normalize would be undefined
    }
    l *= inversesqrt(l_len2);
    float wrap = max(0.3 + dot(-n, l), 0.0);
    if (wrap <= 0.0)
    {
        return vec3(0.0);
    }

    vec3 v = -pos * inversesqrt(max(dot(pos, pos), 1e-12));
    wrap *= fsSkinForwardScatter(n, v, l);

    vec3 p = pos - n * FS_SKIN_TRANS_SHRINK;
    float z = -p.z;
    float scale = fsSkinRimThinning(n, v) / (FS_SKIN_TRANS_MM * max(sss_radius, 0.05));
    vec3 t = vec3(0.0);
    if (z < shadow_clip.x)
    {
        t = fsSkinCascadeTransmittance(shadowDepth0, shadow_matrix[0], p, l, scale);
    }
    else if (z < shadow_clip.y)
    {
        t = fsSkinCascadeTransmittance(shadowDepth1, shadow_matrix[1], p, l, scale);
        t *= 1.0 - smoothstep(0.8 * shadow_clip.y, shadow_clip.y, z);
    }
    t = mix(vec3(t.r), t, clamp(sss_tint, 0.0, 1.0));
    return t * fsSkinSunlight() * wrap;
}

void main()
{
    vec2 tc = vary_fragcoord.xy;

    if (sss_debug != 0)
    {
        // Debug view (FSRenderSkinDebug), pass 1 only: avatar pixels tinted green as far as they
        // count as skin, the rest red where the colour test turned them down and blue where the
        // material test did. Everything else is left as it is.
        vec4 norm_raw = getNormRaw(tc);
        int surface = fsSkinSurface(norm_raw.a);
        if (surface == 0)
        {
            discard;
        }
#ifdef FS_SKIN_DETECT_MATERIAL
        vec4 spec = texture(specularRect, tc);
#else
        vec4 spec = vec4(0.0);
#endif
        vec3 d_albedo;
        float material;
        float tone;
        float w = clamp(fsSkinWeightParts(surface, texture(diffuseRect, tc), spec, norm_raw.b, d_albedo, material, tone), 0.0, 1.0);
        float tone_w = mix(1.0, tone, clamp(sss_tone_match, 0.0, 1.0));
        vec3 rejected = material < tone_w ? vec3(0.1, 0.25, 1.0) : vec3(1.0, 0.1, 0.05);
        vec3 c = mix(rejected, vec3(0.1, 1.0, 0.2), w) * (0.25 * fsSkinDiffuseCeiling());
        const float a = 0.8;
        frag_color = vec4(c * a, a);
        return;
    }

    vec3 albedo;
    float center_w = fsSkinSample(tc, albedo);
    if (center_w <= 0.0)
    {
        // Not avatar (-1) or not skin (0): nothing reads this pixel's blur
        if (sss_pass == 1)
        {
            discard;
        }
        frag_color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec3 pos = getPosition(tc).xyz;
    if (fsSkinBad(pos))
    {
        // No usable depth: leave the pixel as lit. Alpha 0 in the scratch marks it for pass 1.
        if (sss_pass == 1)
        {
            discard;
        }
        frag_color = vec4(0.0);
        return;
    }
    float dist = max(-pos.z, 1e-3);

    vec3 trans = vec3(0.0);
#ifdef FS_SKIN_ENERGY
    float energy_keep = 1.0;    // translucency (all skin)
    float energy_scatter = 1.0; // scattered light (legacy-shaded skin)
    if (sss_pass == 1)
    {
        int surface = fsSkinSurface(getNormRaw(tc).a);
        energy_keep = fsSkinEnergyKeep(getNorm(tc).xyz, pos, fsSkinEnergyRoughness(tc, surface));
        energy_keep = fsSkinBad(energy_keep) ? 1.0 : energy_keep;
        energy_scatter = surface == 1 ? energy_keep : 1.0;
    }
#endif
    if (sss_pass == 1)
    {
        if (sss_translucency > 0.0)
        {
            trans = fsSkinTranslucency(pos, getNorm(tc).xyz) * albedo * (sss_translucency * center_w);
            trans = fsSkinBad(trans) ? vec3(0.0) : max(trans, vec3(0.0));
#ifdef FS_SKIN_ENERGY
            trans *= energy_keep;
#endif
        }
        if (sss_strength <= 0.0)
        {
            // Translucency alone; the scratch was not written this frame
            frag_color = vec4(trans, 0.0);
            return;
        }
    }

    float ceiling = 0.0;
    float f_c = 1.0;
    vec3 center;
    if (sss_pass == 0)
    {
        ceiling = fsSkinDiffuseCeiling();
        vec3 lit = texture(altDiffuseMap, tc).rgb;
        if (fsSkinBad(lit))
        {
            // Already broken upstream: keep it out of every neighbour's blur (they stand in their
            // own centre for it, see below) and leave the pixel itself as it is, alpha 0 telling
            // pass 1 so. Blending over it could only turn an Inf into a NaN (Inf * 0).
            frag_color = vec4(0.0);
            return;
        }
        center = fsSkinClamp(lit / fsSkinScatterAlbedo(albedo), ceiling, f_c);
    }
    else
    {
        vec4 c = texture(altDiffuseMap, tc);
        if (fsSkinBad(c) || c.a <= 0.0)
        {
            discard; // pass 0 left this pixel alone
        }
        center = max(c.rgb, vec3(0.0));
        f_c = clamp(c.a, 1e-3, 1.0);
    }

    // Kernel reach in pixels: mm -> pixels at this distance (inv_proj[1][1] is 1 / proj[1][1])
    float px_per_mm = 0.5 * screen_res.y / (inv_proj[1][1] * dist) * 0.001;
    float reach_px = min(sss_radius * FS_SKIN_RANGE_MM * px_per_mm, sss_max_px);

    vec3 result = center;
    if (reach_px >= 1.0)
    {
        vec2 dir = (sss_pass == 0 ? vec2(1.0, 0.0) : vec2(0.0, 1.0)) / screen_res;
        float px_per_unit = reach_px / FS_SKIN_RANGE_MM;   // pixels per kernel mm
        // A tap more than about twice the reach away in depth is another surface
        float depth_reject = 1.0 / max(2.0 * (reach_px / px_per_mm) * 0.001, 1e-4);
        float tint = clamp(sss_tint, 0.0, 1.0);
#ifdef FS_SKIN_TAP_LOD
        // Small on screen: the merged kernel. Both passes decide alike, from the same distance.
        bool lod = reach_px < FS_SKIN_LOD_PX;
        int taps = lod ? FS_SKIN_LOD_TAPS : FS_SKIN_TAPS;
#else
        int taps = FS_SKIN_TAPS;
#endif

        result = center * mix(vec3(fs_skin_kernel[0].r), fs_skin_kernel[0].rgb, tint);
        for (int i = 1; i <= taps; ++i)
        {
#ifdef FS_SKIN_TAP_LOD
            vec4 kernel = lod ? fs_skin_kernel_lod[min(i, FS_SKIN_LOD_TAPS)] : fs_skin_kernel[i];
#else
            vec4 kernel = fs_skin_kernel[i];
#endif
            vec3 k = mix(vec3(kernel.r), kernel.rgb, tint);
            float x = kernel.a * px_per_unit;
            for (int side = -1; side <= 1; side += 2)
            {
                vec2 stc = tc + dir * (x * float(side));
                vec3 s_albedo;
                float keep = clamp(fsSkinSample(stc, s_albedo), 0.0, 1.0);
                vec3 s = center;
                if (keep > 0.0)
                {
                    float dz = abs(max(-getPosition(stc).z, 1e-3) - dist);
                    keep *= 1.0 - clamp(dz * depth_reject, 0.0, 1.0);
                    vec4 c = texture(altDiffuseMap, stc);
                    // A tap pass 0 skipped (alpha 0), or one that is not a finite number, counts
                    // as off the skin: a single bad pixel must not spread through the kernel
                    bool bad = fsSkinBad(c) || (sss_pass == 1 && c.a <= 0.0);
                    if (!bad && sss_pass == 0)
                    {
                        float f;
                        c.rgb = fsSkinClamp(c.rgb / fsSkinScatterAlbedo(s_albedo), ceiling, f);
                        bad = fsSkinBad(c.rgb);
                    }
                    if (bad || fsSkinBad(keep))
                    {
                        keep = 0.0;
                    }
                    // Off the skin or across a depth step: stand in the centre, so nothing
                    // bleeds in from clothing or the background (Jimenez's follow-surface rule)
                    s = keep > 0.0 ? mix(center, max(c.rgb, vec3(0.0)), keep) : center;
                }
                result += s * k;
            }
        }
    }

    // Last line of defence: whatever slipped through, the output is finite
    result = fsSkinBad(result) ? center : result;

    if (sss_pass == 0)
    {
        frag_color = vec4(result, f_c);
    }
    else
    {
        float a = clamp(center_w * sss_strength, 0.0, 1.0);
#ifdef FS_SKIN_ENERGY
        result *= energy_scatter;
#endif
        frag_color = vec4(a * fsSkinScatterAlbedo(albedo) * result + trans, a * f_c);
    }
}
