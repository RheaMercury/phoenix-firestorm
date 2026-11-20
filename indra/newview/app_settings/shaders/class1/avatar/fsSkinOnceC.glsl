/**
 * @file fsSkinOnceC.glsl
 * @brief FSRenderSkinOnce: skins the vertices of one rigged vertex buffer once a frame (fsskinonce.cpp)
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

// The C++ side puts "#version 430 core", MAX_JOINTS_PER_MESH_OBJECT and SKIN_PRECISE (as the global defines
// have them) in front of this.
//
// One invocation per vertex of the buffer. The blend is getSkinBlend() of avatar/objectSkinV.glsl, the same
// operations in the same order on the same floats, and the outputs are what its skinPoint() and
// skinDirection() give: the position before skin_origin is added, the normal and the tangent's direction
// unnormalised. The vertex shaders then take an identity blend (fs_skin_once) and add skin_origin and the view
// transform themselves, so the result is the vertex they would have skinned.

layout(local_size_x = 64) in;

// the vertex buffer's own GL buffer, whole: its regions are arrays of 16 byte values
layout(std430, binding = 0) readonly buffer FSSkinOnceSource
{
    vec4 fs_src[];
};

// three regions of fs_job.w vertices each: positions (w 1), normals, tangents (w the source tangent's)
layout(std430, binding = 1) writeonly buffer FSSkinOnceSkinned
{
    vec4 fs_dst[];
};

// palettes as the FS_GDD_RIGGED skinning reads them: a header (skin_origin, joint count), then three vec4
// columns a joint
layout(std430, binding = 2) readonly buffer FSSkinOncePalettes
{
    vec4 fs_pal[];
};

// where the source regions start, in vec4s: x positions, y normals, z tangents, w weights (FS_NONE: absent)
uniform uvec4 fs_src_regions;
// x: first output vertex, y: vertices, z: the palette's header (vec4 index), w: vertices per output region
uniform uvec4 fs_job;

#define FS_NONE 0xFFFFFFFFu

mat3x4 fsJoint(uint base, int i)
{
    uint at = base + 1u + 3u * uint(i);
    return mat3x4(fs_pal[at], fs_pal[at + 1u], fs_pal[at + 2u]);
}

mat3x4 fsSkinBlend(vec4 weight4, uint base)
{
    int matrixPaletteSize = int(fs_pal[base].w);

    SKIN_PRECISE vec4 w = fract(weight4);
    vec4 index = floor(weight4);

    int last_palette_index = min(max(matrixPaletteSize - 1, 0), MAX_JOINTS_PER_MESH_OBJECT - 1);
    index = min(index, vec4(float(last_palette_index)));
    index = max(index, vec4( 0.0));

    SKIN_PRECISE float weight_sum = w.x + w.y + w.z + w.w;
    if (weight_sum <= 0.00001)
    {
        // as the vertex shader: broken or partly loaded weights stay on the root joint, never NaN
        w = vec4(1.0, 0.0, 0.0, 0.0);
        index = vec4(0.0);
        weight_sum = 1.0;
    }
    w *= 1.0 / weight_sum;

    mat3x4 j1 = fsJoint(base, int(index.x));
    mat3x4 j2 = fsJoint(base, int(index.y));
    mat3x4 j3 = fsJoint(base, int(index.z));
    mat3x4 j4 = fsJoint(base, int(index.w));

    SKIN_PRECISE mat3 mat = mat3(j1)*w.x;
         mat += mat3(j2)*w.y;
         mat += mat3(j3)*w.z;
         mat += mat3(j4)*w.w;

    SKIN_PRECISE vec3 trans = vec3(j1[0].w,j1[1].w,j1[2].w)*w.x;
         trans += vec3(j2[0].w,j2[1].w,j2[2].w)*w.y;
         trans += vec3(j3[0].w,j3[1].w,j3[2].w)*w.z;
         trans += vec3(j4[0].w,j4[1].w,j4[2].w)*w.w;

    SKIN_PRECISE mat3x4 ret;
    ret[0] = vec4(mat[0], trans.x);
    ret[1] = vec4(mat[1], trans.y);
    ret[2] = vec4(mat[2], trans.z);
    return ret;
}

void main()
{
    uint v = gl_GlobalInvocationID.x;
    if (v >= fs_job.y)
    {
        return;
    }

    mat3x4 skin = fsSkinBlend(fs_src[fs_src_regions.w + v], fs_job.z);
    uint dst = fs_job.x + v;

    // skinPoint()
    vec3 position = fs_src[fs_src_regions.x + v].xyz;
    SKIN_PRECISE vec3 pos = mat3(skin) * position + vec3(skin[0].w, skin[1].w, skin[2].w);
    fs_dst[dst] = vec4(pos, 1.0);

    // skinDirection(); a buffer without normals or tangents is never drawn by a program that reads them, the
    // zeros only keep the region defined
    vec3 normal = vec3(0.0);
    if (fs_src_regions.y != FS_NONE)
    {
        SKIN_PRECISE vec3 n = mat3(skin) * fs_src[fs_src_regions.y + v].xyz;
        normal = n;
    }
    fs_dst[fs_job.w + dst] = vec4(normal, 0.0);

    vec4 tangent = vec4(0.0);
    if (fs_src_regions.z != FS_NONE)
    {
        vec4 t = fs_src[fs_src_regions.z + v];
        SKIN_PRECISE vec3 tdir = mat3(skin) * t.xyz;
        tangent = vec4(tdir, t.w);
    }
    fs_dst[2u * fs_job.w + dst] = tangent;
}
