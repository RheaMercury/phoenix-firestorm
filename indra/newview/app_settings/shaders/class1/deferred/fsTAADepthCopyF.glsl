/**
 * @file fsTAADepthCopyF.glsl
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

// Temporal anti-aliasing (FSRenderTAA, fstaa.h, PresentedDepthScope): writes a depth texture into
// the bound framebuffer's depth over the viewport (fs_taa_viewport: origin, size), each pixel
// taking the texel it falls in. The texture is either the conservative depth (render resolution,
// stretched over the window like the present stretches the scene's) or a copy of the
// framebuffer's own depth at the viewport's size, put back texel for texel.

out vec4 frag_color;

uniform sampler2D depthMap;
uniform vec4 fs_taa_viewport;

void main()
{
    ivec2 size = textureSize(depthMap, 0);
    vec2 rel = gl_FragCoord.xy - fs_taa_viewport.xy;
    ivec2 texel = ivec2(floor(rel * vec2(size) / max(fs_taa_viewport.zw, vec2(1.0))));
    gl_FragDepth = texelFetch(depthMap, clamp(texel, ivec2(0), max(size - 1, ivec2(0))), 0).r;
    frag_color = vec4(0.0);     // colour writes are off
}
