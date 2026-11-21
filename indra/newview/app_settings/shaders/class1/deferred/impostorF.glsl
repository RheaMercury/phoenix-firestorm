/**
 * @file impostorF.glsl
 *
 * $LicenseInfo:firstyear=2011&license=viewerlgpl$
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

uniform float minimum_alpha;


uniform sampler2D diffuseMap;
uniform sampler2D normalMap;
uniform sampler2D specularMap;

in vec2 vary_texcoord0;

// <FS:Trish> FSRenderFadeLOD: above 0, the part of an impostor still shown while it dissolves away over
// its avatar, now drawn in full (LLDrawPoolAvatar::renderAvatars); 0 shows it whole, as before
uniform float fs_impostor_fade;
// </FS:Trish>

vec3 linear_to_srgb(vec3 c);

vec4 encodeNormal(vec3 n, float env, float gbuffer_flag);

void main()
{
    vec4 col = texture(diffuseMap, vary_texcoord0.xy);

    if (col.a < minimum_alpha)
    {
        discard;
    }

    // <FS:Trish> FSRenderFadeLOD: the same screen-door pattern as the fade-in (fsFadeInF.glsl)
    if (fs_impostor_fade > 0.0)
    {
        float threshold = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
        if (threshold >= fs_impostor_fade)
        {
            discard;
        }
    }
    // </FS:Trish>

    vec4 norm = texture(normalMap,   vary_texcoord0.xy);
    vec4 spec = texture(specularMap, vary_texcoord0.xy);

    frag_data[0] = vec4(col.rgb, 0.0);
    frag_data[1] = spec;
    frag_data[2] = vec4(norm.xyz, GBUFFER_FLAG_HAS_ATMOS);

#if defined(HAS_EMISSIVE)
    frag_data[3] = vec4(0);
#endif
}
