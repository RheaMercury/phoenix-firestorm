/**
 * @file fsTAASharpenF.glsl
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
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
 * $/LicenseInfo$
 */

/*[EXTRA_CODE_HERE]*/

// Temporal anti-aliasing (FSRenderTAA, fstaa.h): copies the resolved image back into the HDR
// screen target, sharpened by fs_taa_sharpness to win back the softness the history blending
// leaves. A plus-shaped unsharp mask on tone-compressed colour, held within the five taps' range
// so it can't ring or halo. The history keeps the unsharpened image, so the sharpening never
// builds up from frame to frame. At 0 it is a plain copy.

out vec4 frag_color;

uniform sampler2D diffuseRect;  // the resolved image (rgb)
uniform sampler2D exposureMap;  // last frame's auto exposure, 1x1

uniform vec2 screen_res;
uniform float fs_taa_sharpness; // 0 to 1

const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);

vec3 compress(vec3 c, float e)
{
    return c / (1.0 + dot(c, LUMA) * e);
}

vec3 uncompress(vec3 c, float e)
{
    return c / max(1.0 - dot(c, LUMA) * e, 1e-3);
}

void main()
{
    ivec2 size = ivec2(screen_res);
    ivec2 pix = clamp(ivec2(gl_FragCoord.xy), ivec2(0), size - 1);
    vec3 c = texelFetch(diffuseRect, pix, 0).rgb;
    if (fs_taa_sharpness <= 0.0)
    {
        frag_color = vec4(c, 1.0);
        return;
    }

    float e = texture(exposureMap, vec2(0.5)).r;
    if (isnan(e) || isinf(e) || e <= 0.0)
    {
        e = 1.0;
    }
    e = clamp(e, 1e-3, 1e3);

    vec3 cc = compress(c, e);
    vec3 n = compress(texelFetch(diffuseRect, clamp(pix + ivec2(0, 1), ivec2(0), size - 1), 0).rgb, e);
    vec3 s = compress(texelFetch(diffuseRect, clamp(pix - ivec2(0, 1), ivec2(0), size - 1), 0).rgb, e);
    vec3 w = compress(texelFetch(diffuseRect, clamp(pix - ivec2(1, 0), ivec2(0), size - 1), 0).rgb, e);
    vec3 east = compress(texelFetch(diffuseRect, clamp(pix + ivec2(1, 0), ivec2(0), size - 1), 0).rgb, e);

    vec3 lo = min(cc, min(min(n, s), min(w, east)));
    vec3 hi = max(cc, max(max(n, s), max(w, east)));
    vec3 blurred = (n + s + w + east) * 0.25;
    vec3 sharp = clamp(cc + (cc - blurred) * (fs_taa_sharpness * 1.5), lo, hi);

    frag_color = vec4(uncompress(sharp, e), 1.0);
}
