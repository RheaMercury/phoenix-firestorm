/**
 * @file fsOcclusionHiZC.glsl
 * @brief FSRenderGPUOcclusionCulling: one level of the farthest depth pyramid (fsgpuocclusion.cpp)
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
 *
 * The Phoenix Firestorm Project, Inc., 1831 Oakwood Drive, Fairmont, Minnesota 56031-3225 USA
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */

// Compiled by fsgpuocclusion.cpp with "#version 430 core" in front (compute shaders and image load/store).
// Every texel of a level holds the farthest depth of the texels it covers one level down: the 2x2 below it,
// and for the last column or row of a level whose source is odd also the source's last column or row, so a
// level never loses an edge. A full resolution pixel p is then covered at level k by texel
// min(p >> (k + 1), size_k - 1). Level 0 reads the depth buffer, the others the level before.
// Farthest is the largest depth, or with REVERSE_Z (depth 1 at the near plane) the smallest.

layout(local_size_x = 8, local_size_y = 8) in;

#ifdef FS_HIZ_FROM_DEPTH
layout(binding = 0) uniform sampler2D fs_src_depth;
#else
layout(r32f, binding = 1) readonly uniform image2D fs_src_level;
#endif
layout(r32f, binding = 0) writeonly uniform image2D fs_dst_level;

uniform ivec2 fs_src_size;
uniform ivec2 fs_dst_size;

float fsLoad(ivec2 p)
{
#ifdef FS_HIZ_FROM_DEPTH
    return texelFetch(fs_src_depth, p, 0).r;
#else
    return imageLoad(fs_src_level, p).r;
#endif
}

void main()
{
    ivec2 d = ivec2(gl_GlobalInvocationID.xy);
    if (d.x >= fs_dst_size.x || d.y >= fs_dst_size.y)
    {
        return;
    }
    ivec2 s0 = d * 2;
    ivec2 s1 = s0 + ivec2(1, 1);
    if (d.x == fs_dst_size.x - 1)
    {
        s1.x = fs_src_size.x - 1;
    }
    if (d.y == fs_dst_size.y - 1)
    {
        s1.y = fs_src_size.y - 1;
    }
    s1 = min(s1, fs_src_size - ivec2(1, 1));

#ifdef REVERSE_Z
    float far_depth = 1.0;
#else
    float far_depth = 0.0;
#endif
    for (int y = s0.y; y <= s1.y; ++y)
    {
        for (int x = s0.x; x <= s1.x; ++x)
        {
#ifdef REVERSE_Z
            far_depth = min(far_depth, fsLoad(ivec2(x, y)));
#else
            far_depth = max(far_depth, fsLoad(ivec2(x, y)));
#endif
        }
    }
    imageStore(fs_dst_level, d, vec4(far_depth, 0.0, 0.0, 0.0));
}
