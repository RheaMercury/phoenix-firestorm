/**
 * @file fsTAAOverlayDepthF.glsl
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

// Temporal anti-aliasing (FSRenderTAA, fstaa.h): the depth the selection highlights and debug
// displays are tested against, now that they are drawn after the resolve with the plain
// projection. The scene's depth was rasterised with this frame's jitter, so each texel shows the
// scene up to half a pixel away from where the plain projection puts that pixel. For every pixel
// this writes the nearest of the four texels around the pixel's own centre (where the jitter drew
// it): whatever covers that centre is in front of the overlay, whichever way the jitter fell.

out vec4 frag_color;

uniform sampler2D depthMap;         // the scene's depth, rasterised with the jitter
uniform vec2 screen_res;            // the size of the target written
uniform vec2 fs_taa_jitter_ndc;     // this frame's jitter, NDC

float nearer(float a, float b)
{
#ifdef REVERSE_Z
    return max(a, b);   // depth 1 at the near plane
#else
    return min(a, b);
#endif
}

void main()
{
    ivec2 size = textureSize(depthMap, 0);
    ivec2 last = max(size - 1, ivec2(0));
    // This pixel's centre in the depth texture's texels (it may be sized differently), moved to
    // where the jittered projection drew that point
    vec2 pos = gl_FragCoord.xy * (vec2(size) / max(screen_res, vec2(1.0))) + fs_taa_jitter_ndc * 0.5 * vec2(size);
    ivec2 base = ivec2(floor(pos - 0.5));   // the 2x2 texels around it

    float d0 = texelFetch(depthMap, clamp(base, ivec2(0), last), 0).r;
    float d1 = texelFetch(depthMap, clamp(base + ivec2(1, 0), ivec2(0), last), 0).r;
    float d2 = texelFetch(depthMap, clamp(base + ivec2(0, 1), ivec2(0), last), 0).r;
    float d3 = texelFetch(depthMap, clamp(base + ivec2(1, 1), ivec2(0), last), 0).r;

    gl_FragDepth = nearer(nearer(d0, d1), nearer(d2, d3));
    frag_color = vec4(0.0);     // the target has no colour attachment
}
