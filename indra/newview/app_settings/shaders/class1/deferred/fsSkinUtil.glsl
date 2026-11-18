/**
 * @file fsSkinUtil.glsl
 *
 * Which avatar pixels are skin, shared by the skin scattering pass (fsSkinSSSF.glsl) and, with
 * the skin lighting options on, the deferred sun and local light passes.
 *
 * Avatar pixels: with FSRenderSkinSSS on, the rigged (HAS_SKIN) and system avatar (AVATAR_CLOTH)
 * g-buffer writers bias their flag by FS_SKIN_FLAG_BIAS (LLShaderMgr::loadShaderFile). Inside
 * that, a weight in [0, 1] says how much of the pixel is skin.
 *
 * What the g-buffer holds for an avatar pixel (mRT->deferredScreen):
 *  0 albedo    rgb: sRGB from legacy writers (avatarF, diffuseF, bumpF, materialF), linear from
 *              PBR (pbropaqueF). a: legacy fullbright share; 0 for PBR and the system avatar.
 *  1 specular  legacy: rgb specular colour (sRGB), a glossiness. materialF writes its specular
 *              map times colour and its glossiness; the shiny legacy writers (diffuseF, bumpF)
 *              their shininess level in all four; the system avatar (avatarF) zero.
 *              PBR: packORM, r occlusion, g + a roughness, b metallic.
 *  2 normal    xy octahedral normal. b: legacy environment intensity (materialF's environment
 *              factor, or the shininess level), PBR the geometric normal. a: the flag.
 *  3 emissive  not used here.
 * So the material is authored, and worth trusting, for PBR (metallic and roughness) and for
 * legacy materials (glossiness, environment, specular colour). The system avatar's bakes and
 * legacy mesh without a material carry none of it: there only the albedo tone tells skin.
 *
 * Material score (FSRenderSkinDetectMaterial, FS_SKIN_DETECT_MATERIAL): skin is a dielectric of
 * mid roughness. PBR: metallic near 0 and roughness roughly 0.25 to 0.8; most cloth is rougher,
 * jewellery and buckles metallic. Legacy: not mirror glossy, no strong environment reflection,
 * and no strongly tinted (metal coloured) specular. Pixels with no material information score
 * 1, so the tone alone decides there, as without the option.
 * It multiplies a relaxed tone score (fs_skin_tone_range): with the material doing the work of
 * keeping cloth out, the tone window can open up to pale, very dark and fantasy skins.
 */

/*[EXTRA_CODE_HERE]*/

uniform float sss_tone_match;       // 0: every avatar surface, 1: skin coloured albedo only
#ifdef FS_SKIN_DETECT_MATERIAL
uniform float fs_skin_tone_range;   // 0: the human tone window, 1: any hue, pale and dark
#endif

vec3 linear_to_srgb(vec3 cl);
vec3 srgb_to_linear(vec3 cs);
float unpackRoughness(vec2 p);

#define FS_SKIN_FLAG_BIAS 0.05
#define FS_SKIN_MIN_ALBEDO 0.02     // linear; darker albedo is floored before dividing by it

// 0: not avatar, 1: legacy avatar surface, 2: PBR avatar surface
int fsSkinSurface(float flag)
{
    if (abs(flag - (GBUFFER_FLAG_HAS_ATMOS + FS_SKIN_FLAG_BIAS)) < 0.02)
    {
        return 1;
    }
    if (abs(flag - (GBUFFER_FLAG_HAS_PBR + FS_SKIN_FLAG_BIAS)) < 0.02)
    {
        return 2;
    }
    return 0;
}

// Hue in degrees (-180..180, red at 0), saturation and value of an sRGB colour
vec3 fsSkinHSV(vec3 c)
{
    float mx = max(c.r, max(c.g, c.b));
    float mn = min(c.r, min(c.g, c.b));
    float d = mx - mn;
    float s = d / max(mx, 1e-4);

    float h;
    if (d < 1e-4)
    {
        h = 180.0;
    }
    else if (mx == c.r)
    {
        h = 60.0 * (c.g - c.b) / d;
    }
    else if (mx == c.g)
    {
        h = 60.0 * ((c.b - c.r) / d + 2.0);
    }
    else
    {
        h = 60.0 * ((c.r - c.g) / d + 4.0);
        h = h > 180.0 ? h - 360.0 : h;
    }
    return vec3(h, s, mx);
}

// How much an albedo looks like skin, across the range of human tones: red leading, hue between
// red and yellow-orange, moderate saturation, not black. Smooth edges so the mask has no seams.
float fsSkinTone(vec3 c)
{
    vec3 hsv = fsSkinHSV(c);
    float wh = smoothstep(-25.0, -5.0, hsv.x) * (1.0 - smoothstep(45.0, 65.0, hsv.x));
    float ws = smoothstep(0.05, 0.14, hsv.y) * (1.0 - smoothstep(0.72, 0.88, hsv.y));
    float wv = smoothstep(0.04, 0.12, hsv.z);
    return wh * ws * wv;
}

#ifdef FS_SKIN_DETECT_MATERIAL
// fsSkinTone with its window opened by range: at 0 the same, at 1 any hue, any paleness, and
// down to near black; only neon saturation keeps some penalty
float fsSkinToneRelaxed(vec3 c, float range)
{
    vec3 hsv = fsSkinHSV(c);
    float r = clamp(range, 0.0, 1.0);
    float wh = smoothstep(-25.0, -5.0, hsv.x) * (1.0 - smoothstep(45.0, 65.0, hsv.x));
    float ws_pale = smoothstep(0.05, 0.14, hsv.y);
    float ws_neon = 1.0 - smoothstep(0.72, 0.88, hsv.y);
    float dark = 1.0 - 0.75 * r;
    float wv = smoothstep(0.04 * dark, 0.12 * dark, hsv.z);
    return mix(wh, 1.0, r) * mix(ws_pale, 1.0, r) * mix(ws_neon, 1.0, 0.5 * r) * wv;
}

// How much the stored material looks like skin (1 when the pixel carries no material)
float fsSkinMaterial(int surface, vec4 spec, float env)
{
    if (surface == 2)
    {
        float metallic = spec.b;
        float rough = unpackRoughness(spec.ga);
        return (1.0 - smoothstep(0.2, 0.5, metallic)) *
               smoothstep(0.12, 0.28, rough) * (1.0 - smoothstep(0.78, 0.92, rough));
    }

    // Legacy: mirror gloss or a strong environment reflection is not skin (the "low" shininess
    // some skins use stays in), nor a strongly tinted specular colour (gold, copper)
    float gloss = spec.a;
    float mx = max(spec.r, max(spec.g, spec.b));
    float mn = min(spec.r, min(spec.g, spec.b));
    float tint = (mx - mn) / max(mx, 1e-3);
    return (1.0 - smoothstep(0.7, 0.9, gloss)) *
           (1.0 - smoothstep(0.3, 0.6, env)) *
           (1.0 - smoothstep(0.35, 0.6, tint) * smoothstep(0.05, 0.2, mx));
}
#endif

// Skin weight of an avatar surface (surface from fsSkinSurface, not 0) in [0, 1], from its raw
// albedo, specular and environment texels. albedo: the linear diffuse albedo the lighting
// multiplied by, floored. material, tone: the two scores, for the debug view.
float fsSkinWeightParts(int surface, vec4 base, vec4 spec, float env, out vec3 albedo, out float material, out float tone)
{
    vec3 srgb;
    float w = 1.0;
    if (surface == 2)
    {
        albedo = base.rgb;
        srgb = linear_to_srgb(base.rgb);
    }
    else
    {
        albedo = srgb_to_linear(base.rgb);
        srgb = base.rgb;
        w = 1.0 - clamp(base.a, 0.0, 1.0); // legacy alpha is the fullbright share
    }
    albedo = max(albedo, vec3(FS_SKIN_MIN_ALBEDO));

#ifdef FS_SKIN_DETECT_MATERIAL
    material = fsSkinMaterial(surface, spec, env);
    tone = fsSkinToneRelaxed(srgb, fs_skin_tone_range);
#else
    material = 1.0;
    tone = fsSkinTone(srgb);
#endif
    return w * material * mix(1.0, tone, clamp(sss_tone_match, 0.0, 1.0));
}

float fsSkinWeightRaw(int surface, vec4 base, vec4 spec, float env, out vec3 albedo)
{
    float material;
    float tone;
    return fsSkinWeightParts(surface, base, spec, env, albedo, material, tone);
}

// Skin weight of a g-buffer pixel for the lighting passes: 0 when not avatar or not skin
float fsSkinWeightGB(GBufferInfo gb)
{
    int surface = fsSkinSurface(gb.gbufferFlag);
    if (surface == 0)
    {
        return 0.0;
    }
    vec3 albedo;
    return clamp(fsSkinWeightRaw(surface, gb.albedo, gb.specular, gb.envIntensity, albedo), 0.0, 1.0);
}
