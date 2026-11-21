/**
 * @file class3\deferred\fsBatchedSpotLightF.glsl
 *
 * $LicenseInfo:firstyear=2022&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2022, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

// Batched spotlights without a shadow (FSPerfBatchedSpotLights): the fragment half of
// gFSBatchedSpotLightProgram, with fsBatchedSpotLightV.glsl.
//
// A copy of spotLightF.glsl's lighting for a light with no shadow slot: proj_shadow_idx is -1,
// so shadow is 1 and shadow_fade never read, and the extra-slot and coloured shadow terms
// (FS_SPOT_SHADOW_EXTRA, FS_SPOT_COLORED_SHADOW) are 1 as well, so they are left out. Every
// other line follows spotLightF; keep the two in step.
//
// The per-light values come from the FSSpotLights block at the vec4 index the vertex shader
// passes (fs_spot_index), not from uniforms. deferredUtil.glsl's projector helpers read the
// projector uniforms (proj_mat, proj_lod, color, size, ...), and it is a shared object compiled
// once, so this file has its own copies (fsSpot*) that take the values as arguments. None of its
// globals reuse those uniforms' names: on AMD a global of the same name as another object's
// uniform fails to link.

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

uniform sampler2D projectionMap; // rgba, the group's projection texture (deferredUtil's uniform)
uniform int classic_mode;

#define FS_SPOT_BLOCK_VEC4 1024

layout(std140) uniform FSSpotLights
{
    vec4 fs_spot_data[FS_SPOT_BLOCK_VEC4]; // layout in fsBatchedSpotLightV.glsl
};

in vec4 vary_fragcoord;
flat in vec3 trans_center;
flat in int fs_spot_index;

void calcHalfVectors(vec3 lv, vec3 n, vec3 v, out vec3 h, out vec3 l, out float nh, out float nl, out float nv, out float vh, out float lightDist);
float blinnPhongLobe(float nh, float glossiness);
void calcDiffuseSpecular(vec3 baseColor, float metallic, inout vec3 diffuseColor, inout vec3 specularColor);
vec3 clampRadiance(vec3 c);
float fsMicroShadow(float nl, float ao); // deferredUtil.glsl: normal-map micro-shadows, 1 when off
float unpackRoughness(vec2 p);
float calcLegacyDistanceAttenuation(float distance, float falloff);
vec2 getScreenCoord(vec4 clip);
vec3 srgb_to_linear(vec3 cs);

vec4 getPosition(vec2 pos_screen);

void pbrPunctual(vec3 diffuseColor, vec3 specularColor,
                    float perceptualRoughness,
                    float metallic,
                    vec3 n, // normal
                    vec3 v, // surface point to camera
                    vec3 l, // surface point to light
                    out float nl,
                    out vec3 diff,
                    out vec3 spec);

GBufferInfo getGBuffer(vec2 screenpos);
// Avatar skin lighting (fsSkinUtil.glsl)
#ifdef FS_SKIN_LIGHTING
float fsSkinWeightGB(GBufferInfo gb, vec2 tc);
#endif
#ifdef FS_SKIN_DUAL_SPEC
vec3 fsSkinDualLobe(vec3 n, vec3 v, vec3 l, float perceptualRoughness);
vec3 fsSkinLegacySpecular(vec3 n, vec3 v, vec3 l, vec4 spec);
#endif
#ifdef FS_SKIN_TRANS_LOCAL
float fsSkinThickness(vec2 tc, vec3 pos, vec3 n);
vec3 fsSkinLocalTranslucency(vec3 albedo, vec3 n, vec3 l, float thickness_mm, float w, vec3 pos);
vec3 fsSkinAlbedo(GBufferInfo gb);
#endif

// deferredUtil.glsl's clipProjectedLightVars, with the light's size and projection as arguments
bool fsSpotClipProjectedLightVars(vec3 light_center, vec3 pos, float light_size, mat4 light_proj_mat, vec3 light_proj_n,
                                  out float dist, out float l_dist, out vec3 lv, out vec4 proj_tc)
{
    lv = light_center - pos.xyz;
    dist = length(lv);
    bool clipped = (dist >= light_size);
    if ( !clipped )
    {
        dist /= light_size;

        l_dist = -dot(lv, light_proj_n);
        vec4 projected_point = (light_proj_mat * vec4(pos.xyz, 1.0));
        clipped = (projected_point.z < 0.0);
        projected_point.xyz /= projected_point.w;
        proj_tc = projected_point;
    }

    return clipped;
}

// deferredUtil.glsl's getTexture2DLodAmbient
vec4 fsSpotTexture2DLodAmbient(vec2 tc, float lod)
{
#ifndef FXAA_GLSL_120
    vec4 ret = textureLod(projectionMap, tc, lod);
#else
    vec4 ret = texture(projectionMap, tc);
#endif
    // projectionMap is an sRGB texture bound without hardware decode; convert it to linear.
    ret.rgb = srgb_to_linear(ret.rgb);

    vec2 dist = tc-vec2(0.5);
    float d = dot(dist,dist);
    ret *= min(clamp((0.25-d)/0.25, 0.0, 1.0), 1.0);

    return ret;
}

// deferredUtil.glsl's getTexture2DLodDiffuse, proj_lod as an argument
vec4 fsSpotTexture2DLodDiffuse(vec2 tc, float lod, float light_proj_lod)
{
#ifndef FXAA_GLSL_120
    vec4 ret = textureLod(projectionMap, tc, lod);
#else
    vec4 ret = texture(projectionMap, tc);
#endif
    // projectionMap is an sRGB texture bound without hardware decode; convert it to linear.
    ret.rgb = srgb_to_linear(ret.rgb);

    vec2 dist = vec2(0.5) - abs(tc-vec2(0.5));
    float det = min(lod/(light_proj_lod*0.5), 1.0);
    float d = min(dist.x, dist.y);
    float edge = 0.25*det;
    ret *= clamp(d/edge, 0.0, 1.0);

    return ret;
}

// deferredUtil.glsl's getProjectedLightAmbiance, the light's colour, proj_lod and proj_ambiance
// as arguments
vec3 fsSpotProjectedLightAmbiance(float amb_da, float attenuation, float lit, float nl, float noise, vec2 projected_uv,
                                  vec3 light_color, float light_proj_lod, float light_proj_ambiance)
{
    vec4 amb_plcol = fsSpotTexture2DLodAmbient(projected_uv, light_proj_lod);
    vec3 amb_rgb   = amb_plcol.rgb * amb_plcol.a;

    amb_da += light_proj_ambiance;
    amb_da += (nl*nl*0.5+0.5) * light_proj_ambiance;
    amb_da *= attenuation * noise;
    amb_da = min(amb_da, 1.0-lit);

    return (amb_da * light_color.rgb * amb_rgb);
}

// deferredUtil.glsl's getProjectedLightDiffuseColor, the light's colour, proj_focus, proj_range
// and proj_lod as arguments
vec3 fsSpotProjectedLightDiffuseColor(float light_distance, vec2 projected_uv,
                                      vec3 light_color, float light_proj_focus, float light_proj_range, float light_proj_lod)
{
    float diff = clamp((light_distance - light_proj_focus)/light_proj_range, 0.0, 1.0);
    float lod = diff * light_proj_lod;
    vec4 plcol = fsSpotTexture2DLodDiffuse(projected_uv.xy, lod, light_proj_lod);

    return light_color.rgb * plcol.rgb * plcol.a;
}

// deferredUtil.glsl's texture2DLodSpecular, proj_lod as an argument
vec4 fsSpotTexture2DLodSpecular(vec2 tc, float lod, float light_proj_lod)
{
#ifndef FXAA_GLSL_120
    vec4 ret = textureLod(projectionMap, tc, lod);
#else
    vec4 ret = texture(projectionMap, tc);
#endif
    // projectionMap is an sRGB texture bound without hardware decode; convert it to linear.
    ret.rgb = srgb_to_linear(ret.rgb);

    vec2 dist = vec2(0.5) - abs(tc-vec2(0.5));
    float det = min(lod/(light_proj_lod*0.5), 1.0);
    float d = min(dist.x, dist.y);
    d *= min(1, d * (light_proj_lod - lod)); // BUG? extra factor compared to diffuse causes N repeats
    float edge = 0.25*det;
    ret *= clamp(d/edge, 0.0, 1.0);

    return ret;
}

void main()
{
    // This light's values, in place of spotLightF's uniforms
    int k = fs_spot_index;
    vec4 fs_center_size   = fs_spot_data[k];
    vec4 fs_color_falloff = fs_spot_data[k + 1];
    mat4 fs_proj_mat      = mat4(fs_spot_data[k + 2], fs_spot_data[k + 3], fs_spot_data[k + 4], fs_spot_data[k + 5]);
    vec4 fs_proj_p_near   = fs_spot_data[k + 6];
    vec4 fs_proj_n_range  = fs_spot_data[k + 7];
    vec4 fs_origin_amb    = fs_spot_data[k + 8];
    vec4 fs_focus_lod     = fs_spot_data[k + 9];

    float fs_size          = fs_center_size.w;
    vec3  fs_color         = fs_color_falloff.rgb;
    float fs_falloff       = fs_color_falloff.a;
    vec3  fs_proj_p        = fs_proj_p_near.xyz;
    vec3  fs_proj_n        = fs_proj_n_range.xyz;
    float fs_proj_range    = fs_proj_n_range.w;
    vec3  fs_proj_origin   = fs_origin_amb.xyz;
    float fs_proj_ambiance = fs_origin_amb.w;
    float fs_proj_focus    = fs_focus_lod.x;
    float fs_proj_lod      = fs_focus_lod.y;
    // proj_near (fs_proj_p_near.w) and proj_ambient_lod (fs_focus_lod.z) are uploaded as the stock
    // path sets them, but spotLightF reads neither.

    vec3 final_color = vec3(0,0,0);
    vec2 tc          = getScreenCoord(vary_fragcoord);
    vec3 pos         = getPosition(tc).xyz;

    vec3 lv;
    vec4 proj_tc;
    float dist, l_dist;
    vec3 c = trans_center;

    if (fsSpotClipProjectedLightVars(c, pos, fs_size, fs_proj_mat, fs_proj_n, dist, l_dist, lv, proj_tc))
    {
        discard;
    }

    // No shadow slot: spotLightF's shadow is 1 (proj_shadow_idx -1), the factor is left out below.

    GBufferInfo gb = getGBuffer(tc);
#ifdef FS_SKIN_LIGHTING
    float fs_skin = fsSkinWeightGB(gb, tc); // how much of this pixel is avatar skin, 0 elsewhere
#endif

    vec3 n = gb.normal;

    float dist_atten = calcLegacyDistanceAttenuation(dist, fs_falloff);
    if (dist_atten <= 0.0)
    {
        discard;
    }

    lv = fs_proj_origin-pos.xyz;
    vec3 v = -normalize(pos);

    vec3 diffuse = gb.albedo.rgb;
    vec4 spec    = gb.specular;
    vec3 dlit    = vec3(0, 0, 0);

    vec3 amb_rgb = vec3(0);

    if (GET_GBUFFER_FLAG(gb.gbufferFlag, GBUFFER_FLAG_HAS_PBR))
    {
        vec3 orm = spec.rgb;
        float perceptualRoughness = unpackRoughness(spec.ga);
        float metallic = orm.b;
        vec3 baseColor = diffuse.rgb;

        vec3 diffuseColor;
        vec3 specularColor;
        calcDiffuseSpecular(baseColor, metallic, diffuseColor, specularColor);
        vec3 diffPunc = vec3(0);
        vec3 specPunc = vec3(0);
        float nl;

        // We need this additional test inside a light's frustum since a spotlight's ambiance can be applied
        if (proj_tc.x > 0.0 && proj_tc.x < 1.0
        &&  proj_tc.y > 0.0 && proj_tc.y < 1.0)
        {
            float lit = 0.0;
            float amb_da = 0.0;

            lv = normalize(lv);

            // pbrPunctual's own NdotL, so the ambiance below sees the same value either way. Only
            // a front-facing fragment uses the lobes, so the BRDF is evaluated inside the test.
            nl = clamp(dot(n.xyz, lv), 0.0, 1.0);

            if (dot(n.xyz, lv) > 0.0)
            {
                pbrPunctual(diffuseColor, specularColor, perceptualRoughness, metallic, n.xyz, v, lv, nl, diffPunc, specPunc);
#ifdef FS_SKIN_DUAL_SPEC
                if (fs_skin > 0.0)
                {
                    specPunc = mix(specPunc, fsSkinDualLobe(n, v, lv, perceptualRoughness), fs_skin); // skin: two lobes
                }
#endif

                amb_da += (nl*0.5 + 0.5) * fs_proj_ambiance;

                dlit = fsSpotProjectedLightDiffuseColor( l_dist, proj_tc.xy, fs_color, fs_proj_focus, fs_proj_range, fs_proj_lod );

                vec3 intensity = dist_atten * dlit * PUNCTUAL_LIGHT_SCALE;
                intensity *= fsMicroShadow(nl, orm.r);

                final_color += intensity * clampRadiance(nl * (diffPunc + specPunc));
                lit = clamp(nl * dist_atten, 0.0, 1.0);
            }
#ifdef FS_SKIN_TRANS_LOCAL
            else if (fs_skin > 0.0) // skin: light through it from behind
            {
                final_color += fsSkinLocalTranslucency(baseColor, n, lv, fsSkinThickness(tc, pos, n), fs_skin, pos) *
                               fsSpotProjectedLightDiffuseColor(l_dist, proj_tc.xy, fs_color, fs_proj_focus, fs_proj_range, fs_proj_lod) * dist_atten;
            }
#endif

            // Ambiance is scaled by proj_ambiance throughout, and most projectors leave it at zero:
            // skip the mipmapped cookie fetch then. The condition is uniform per light.
            if (fs_proj_ambiance != 0.0)
            {
                amb_rgb = fsSpotProjectedLightAmbiance( amb_da, dist_atten, lit, nl, 1.0, proj_tc.xy, fs_color, fs_proj_lod, fs_proj_ambiance ) * PUNCTUAL_LIGHT_SCALE;

                final_color += diffuseColor.rgb * amb_rgb;
            }
        }
    }
    else
    {
        // These legacy angular terms are independent of the PBR lighting above.
        vec3 h, l;
        float nh, nl, nv, vh, lightDist;
        calcHalfVectors(lv, n, v, h, l, nh, nl, nv, vh, lightDist);

        float envIntensity = gb.envIntensity;

        diffuse = srgb_to_linear(diffuse);
        spec.rgb = srgb_to_linear(spec.rgb);

        if (proj_tc.z > 0.0 &&
            proj_tc.x < 1.0 &&
            proj_tc.y < 1.0 &&
            proj_tc.x > 0.0 &&
            proj_tc.y > 0.0)
        {
            float amb_da = 0;
            float lit = 0.0;

            if (nl > 0.0)
            {
                lit = nl * dist_atten;

                dlit = fsSpotProjectedLightDiffuseColor( l_dist, proj_tc.xy, fs_color, fs_proj_focus, fs_proj_range, fs_proj_lod );

                final_color = dlit*lit*diffuse;
#ifdef FS_SKIN_DUAL_SPEC
                if (fs_skin > 0.0) // skin: the two-lobe specular, also where legacy has none
                {
                    final_color += fsSkinLegacySpecular(n, v, l, spec) * dlit * (dist_atten * fs_skin);
                }
#endif

                amb_da += (nl*0.5+0.5) * fs_proj_ambiance;
            }

#ifdef FS_SKIN_TRANS_LOCAL
            // Skin: light through it from behind. nl is floored above zero, so the facing test is
            // on the raw dot product.
            if (fs_skin > 0.0 && dot(n, l) <= 0.0)
            {
                final_color += fsSkinLocalTranslucency(diffuse, n, l, fsSkinThickness(tc, pos, n), fs_skin, pos) *
                               fsSpotProjectedLightDiffuseColor(l_dist, proj_tc.xy, fs_color, fs_proj_focus, fs_proj_range, fs_proj_lod) * dist_atten;
            }
#endif

            // See the PBR branch: zero ambiance contributes exactly nothing here either (where lit
            // exceeds 1 and the clamp goes negative, nl > 0 and the facing factor is zero).
            if (fs_proj_ambiance != 0.0)
            {
                amb_rgb = fsSpotProjectedLightAmbiance( amb_da, dist_atten, lit, nl, 1.0, proj_tc.xy, fs_color, fs_proj_lod, fs_proj_ambiance );
                final_color += diffuse.rgb * amb_rgb * max(dot(-normalize(lv), n), 0.0);
            }
        }

        if (spec.a > 0.0)
        {
            dlit *= min(nl*6.0, 1.0) * dist_atten;

            float f = 1 - vh;
            float f2 = f * f;
            float fres = (f2 * f2 * f) * 0.4 + 0.5;

            float gtdenom = 2 * nh;
            float gt = max(0, min(gtdenom * nv / vh, gtdenom * nl / vh));

            if (nh > 0.0)
            {
                float scol = fres*blinnPhongLobe(nh, spec.a)*gt/(nh*max(nl, 1e-6));
#ifdef FS_SKIN_DUAL_SPEC
                vec3 speccol = dlit*scol*spec.rgb * (1.0 - fs_skin); // skin takes the two-lobe specular instead
#else
                vec3 speccol = dlit*scol*spec.rgb;
#endif
                speccol = clamp(speccol, vec3(0), vec3(1));
                final_color += speccol;
            }
        }

        if (envIntensity > 0.0)
        {
            vec3 ref = reflect(normalize(pos), n);

            //project from point pos in direction ref to plane proj_p, proj_n
            vec3 pdelta = fs_proj_p-pos;
            float ds = dot(ref, fs_proj_n);

            if (ds < 0.0)
            {
                vec3 pfinal = pos + ref * dot(pdelta, fs_proj_n)/ds;

                vec4 stc = (fs_proj_mat * vec4(pfinal.xyz, 1.0));

                if (stc.z > 0.0)
                {
                    stc /= stc.w;

                    if (stc.x < 1.0 &&
                        stc.y < 1.0 &&
                        stc.x > 0.0 &&
                        stc.y > 0.0)
                    {
                        final_color += fs_color.rgb * fsSpotTexture2DLodSpecular(stc.xy, (1 - spec.a) * (fs_proj_lod * 0.6), fs_proj_lod).rgb * envIntensity;
                    }
                }
            }
        }
    }

    //not sure why, but this line prevents MATBUG-194
    final_color = max(final_color, vec3(0.0));
    float final_scale = 1.0;
    if (classic_mode > 0)
        final_scale = 0.9;
    //output linear
    frag_color.rgb = final_color * final_scale;
    frag_color.a = 0.0;
}
