/**
 * @file class1\deferred\terrainF.glsl
 *
 * $LicenseInfo:firstyear=2007&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2007, Linden Research, Inc.
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

/*[EXTRA_CODE_HERE]*/

out vec4 frag_data[4];

uniform sampler2D detail_0;
uniform sampler2D detail_1;
uniform sampler2D detail_2;
uniform sampler2D detail_3;
uniform sampler2D alpha_ramp;

in vec3 pos;
in vec3 vary_normal;
in vec4 vary_texcoord0;
in vec4 vary_texcoord1;

void mirrorClip(vec3 position);
vec4 encodeNormal(vec3 n, float env, float gbuffer_flag);

// <FS:Trish> Terrain anti-tiling (fsTerrainAntiTilingF.glsl, FSRenderTerrainAntiTiling)
#ifdef FS_TERRAIN_ANTI_TILING
vec4 fsAntiTileSampleColor(sampler2D tex, vec2 uv, vec2 ddx, vec2 ddy, out vec3 hex_w);
#endif
// </FS:Trish>

void main()
{
    mirrorClip(pos);
    /// Note: This should duplicate the blending functionality currently used for the terrain rendering.

    // <FS:Trish> The ramps first, then only the layers they give any weight: away from the
    // transitions the ramps sit at exactly 0 or 1 and two or three of the four layers were fetched
    // (three textureGrad each plus a macro one with hex anti-tiling) and multiplied by 0. mix() with
    // a weight of exactly 0 or 1 returns one side unchanged, so the result is the same. The
    // derivatives are taken first, in uniform control flow, and the fetches use them.
    float alpha1 = texture(alpha_ramp, vary_texcoord0.zw).a;
    float alpha2 = texture(alpha_ramp,vary_texcoord1.xy).a;
    float alphaFinal = texture(alpha_ramp, vary_texcoord1.zw).a;
    bool fs_use0 = alphaFinal > 0.0 && alpha1 > 0.0;
    bool fs_use1 = alphaFinal > 0.0 && alpha1 < 1.0;
    bool fs_use2 = alphaFinal < 1.0 && alpha2 > 0.0;
    bool fs_use3 = alphaFinal < 1.0 && alpha2 < 1.0;
    vec2 fs_uv = vary_texcoord0.xy;
    vec2 fs_ddx = dFdx(fs_uv);
    vec2 fs_ddy = dFdy(fs_uv);
    vec4 color0 = vec4(0.0);
    vec4 color1 = vec4(0.0);
    vec4 color2 = vec4(0.0);
    vec4 color3 = vec4(0.0);
    // Terrain anti-tiling: the four layers share one coordinate and its cells
#ifdef FS_TERRAIN_ANTI_TILING
    vec3 fs_hex_w;
    if (fs_use0) color0 = fsAntiTileSampleColor(detail_0, fs_uv, fs_ddx, fs_ddy, fs_hex_w);
    if (fs_use1) color1 = fsAntiTileSampleColor(detail_1, fs_uv, fs_ddx, fs_ddy, fs_hex_w);
    if (fs_use2) color2 = fsAntiTileSampleColor(detail_2, fs_uv, fs_ddx, fs_ddy, fs_hex_w);
    if (fs_use3) color3 = fsAntiTileSampleColor(detail_3, fs_uv, fs_ddx, fs_ddy, fs_hex_w);
#else
    //vec4 color0 = texture(detail_0, vary_texcoord0.xy);
    //vec4 color1 = texture(detail_1, vary_texcoord0.xy);
    //vec4 color2 = texture(detail_2, vary_texcoord0.xy);
    //vec4 color3 = texture(detail_3, vary_texcoord0.xy);
    if (fs_use0) color0 = textureGrad(detail_0, fs_uv, fs_ddx, fs_ddy);
    if (fs_use1) color1 = textureGrad(detail_1, fs_uv, fs_ddx, fs_ddy);
    if (fs_use2) color2 = textureGrad(detail_2, fs_uv, fs_ddx, fs_ddy);
    if (fs_use3) color3 = textureGrad(detail_3, fs_uv, fs_ddx, fs_ddy);
#endif
    // </FS:Trish>
    vec4 outColor = mix( mix(color3, color2, alpha2), mix(color1, color0, alpha1), alphaFinal );

    outColor.a = 0.0; // yes, downstream atmospherics

    frag_data[0] = max(outColor, vec4(0));
    frag_data[1] = vec4(0.0,0.0,0.0,-1.0);
    vec3 nvn = normalize(vary_normal);
    frag_data[2] = encodeNormal(nvn.xyz, 0, GBUFFER_FLAG_HAS_ATMOS);

#if defined(HAS_EMISSIVE)
    frag_data[3] = vec4(0);
#endif
}

