/**
 * @file fsOcclusionCullC.glsl
 * @brief FSRenderGPUOcclusionCulling: which indirect draw commands of a GPU-driven pass draw (fsgpuocclusion.cpp)
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

// Compiled by fsgpuocclusion.cpp with "#version 430 core" in front. One invocation per draw command.
//
// Phase 1 (no FS_OCC_PHASE2), while the pass draws: the commands come in through fs_cmd2 (the pass's own
// commands, 5 uints each: count, instance count, first index, base vertex, base instance). A command whose
// batch was visible last frame and is in the view frustum goes into fs_cmd1 with instance count 1, any other
// with 0; fs_cmd2 keeps the command with instance count 0. The pass draws fs_cmd1.
//
// Phase 2 (FS_OCC_PHASE2), after every opaque pool has drawn and the depth pyramid was built from that
// depth: every command with a box is tested against the frustum and the pyramid. Visible and not drawn in
// phase 1: fs_cmd2's instance count becomes 1, and the pass draws fs_cmd2 again. The result is the batch's
// visibility for the next frame.
//
// A command without a box (slot 0) is always drawn in phase 1 and never tested.
// Boxes are in the space the batch's vertices are in; the modelview projection matrix the batch is drawn
// with comes from the pass's matrix table, named by its draw record.

layout(local_size_x = 64) in;

layout(std430, binding = 0) buffer FSOccCommands1
{
    uint fs_cmd1[];
};

layout(std430, binding = 1) buffer FSOccCommands2
{
    uint fs_cmd2[];
};

// per command 8 uints: box min xyz (float bits), slot, box max xyz (float bits), triangles
layout(std430, binding = 2) readonly buffer FSOccEntries
{
    uint fs_entry[];
};

layout(std430, binding = 3) readonly buffer FSOccRecords
{
    uint fs_record[];
};

layout(std430, binding = 4) readonly buffer FSOccMatrices
{
    mat4 fs_matrix[];
};

layout(std430, binding = 5) buffer FSOccVisibility
{
    uint fs_visible[];
};

layout(std430, binding = 6) buffer FSOccStats
{
    uint fs_stats[];
};

uniform uint fs_count;          // commands
uniform uint fs_record_stride;  // uints per draw record
uniform uint fs_matrix_field;   // the record's uint naming the batch's first matrix
uniform uint fs_mvp_offset;     // the modelview projection matrix, from that first matrix
uniform uint fs_collect;        // 1: count into fs_stats
uniform uint fs_stats_base;     // where this kind of pass's counters start in fs_stats

#ifdef FS_OCC_PHASE2
layout(binding = 0) uniform sampler2D fs_hiz;
uniform ivec2 fs_screen;        // the depth buffer's size in pixels
uniform int fs_levels;          // pyramid levels
#endif

// 0 visible, 1 outside the frustum, 2 hidden behind the depth pyramid
int fsTest(uint c, bool use_hiz)
{
    uint e = c * 8u;
    vec3 bmin = vec3(uintBitsToFloat(fs_entry[e]), uintBitsToFloat(fs_entry[e + 1u]), uintBitsToFloat(fs_entry[e + 2u]));
    vec3 bmax = vec3(uintBitsToFloat(fs_entry[e + 4u]), uintBitsToFloat(fs_entry[e + 5u]), uintBitsToFloat(fs_entry[e + 6u]));

    uint record = fs_cmd2[c * 5u + 4u];
    mat4 mvp = fs_matrix[fs_record[record * fs_record_stride + fs_matrix_field] + fs_mvp_offset];

    vec4 base = mvp * vec4(bmin, 1.0);
    vec4 dx = mvp[0] * (bmax.x - bmin.x);
    vec4 dy = mvp[1] * (bmax.y - bmin.y);
    vec4 dz = mvp[2] * (bmax.z - bmin.z);

    // per plane: how many corners are outside it (x > w, x < -w, y > w, y < -w, beyond the far plane)
    int out_px = 0;
    int out_nx = 0;
    int out_py = 0;
    int out_ny = 0;
    int out_far = 0;
    bool behind = false;        // a corner at or behind the camera plane: no screen rectangle
    bool near_clip = false;     // a corner in front of the near plane
    vec2 ndc_min = vec2(1.0e30);
    vec2 ndc_max = vec2(-1.0e30);
#ifdef REVERSE_Z
    float nearest = 0.0;
#else
    float nearest = 1.0;
#endif
    for (int i = 0; i < 8; ++i)
    {
        vec4 p = base;
        if ((i & 1) != 0)
        {
            p += dx;
        }
        if ((i & 2) != 0)
        {
            p += dy;
        }
        if ((i & 4) != 0)
        {
            p += dz;
        }
        out_px += p.x > p.w ? 1 : 0;
        out_nx += p.x < -p.w ? 1 : 0;
        out_py += p.y > p.w ? 1 : 0;
        out_ny += p.y < -p.w ? 1 : 0;
#ifdef REVERSE_Z
        out_far += p.z < 0.0 ? 1 : 0;
        near_clip = near_clip || p.z > p.w;
#else
        out_far += p.z > p.w ? 1 : 0;
        near_clip = near_clip || p.z < -p.w;
#endif
        if (p.w <= 1.0e-5)
        {
            behind = true;
        }
        else
        {
            vec3 ndc = p.xyz / p.w;
            ndc_min = min(ndc_min, ndc.xy);
            ndc_max = max(ndc_max, ndc.xy);
#ifdef REVERSE_Z
            nearest = max(nearest, ndc.z);
#else
            nearest = min(nearest, ndc.z * 0.5 + 0.5);
#endif
        }
    }
    if (out_px == 8 || out_nx == 8 || out_py == 8 || out_ny == 8 || out_far == 8)
    {
        return 1;
    }
#ifdef FS_OCC_PHASE2
    if (!use_hiz || behind || near_clip || fs_levels <= 0)
    {
        return 0;
    }

    // the pixels the box covers, clamped to the screen
    vec2 screen = vec2(fs_screen);
    vec2 lo = clamp((ndc_min * 0.5 + 0.5) * screen, vec2(0.0), screen - vec2(1.0));
    vec2 hi = clamp((ndc_max * 0.5 + 0.5) * screen, vec2(0.0), screen - vec2(1.0));
    ivec2 p0 = ivec2(lo);
    ivec2 p1 = ivec2(hi);

    // the finest level where they span at most 2x2 texels
    int level = 0;
    for (; level < fs_levels - 1; ++level)
    {
        int shift = level + 1;
        int span = max((p1.x >> shift) - (p0.x >> shift), (p1.y >> shift) - (p0.y >> shift));
        if (span <= 1)
        {
            break;
        }
    }
    int shift = level + 1;
    ivec2 last = textureSize(fs_hiz, level) - ivec2(1, 1);
    ivec2 t0 = min(ivec2(p0.x >> shift, p0.y >> shift), last);
    ivec2 t1 = min(ivec2(p1.x >> shift, p1.y >> shift), last);
    float a = texelFetch(fs_hiz, t0, level).r;
    float b = texelFetch(fs_hiz, ivec2(t1.x, t0.y), level).r;
    float c2 = texelFetch(fs_hiz, ivec2(t0.x, t1.y), level).r;
    float d = texelFetch(fs_hiz, t1, level).r;
#ifdef REVERSE_Z
    float far_depth = min(min(a, b), min(c2, d));
    return nearest < far_depth ? 2 : 0;
#else
    float far_depth = max(max(a, b), max(c2, d));
    return nearest > far_depth ? 2 : 0;
#endif
#else
    return 0;
#endif
}

void main()
{
    uint c = gl_GlobalInvocationID.x;
    if (c >= fs_count)
    {
        return;
    }
    uint cmd = c * 5u;
    uint slot = fs_entry[c * 8u + 3u];

#ifndef FS_OCC_PHASE2
    uint draw = 1u;
    if (slot != 0u && (fs_visible[slot] == 0u || fsTest(c, false) != 0))
    {
        draw = 0u;
    }
    fs_cmd1[cmd] = fs_cmd2[cmd];
    fs_cmd1[cmd + 1u] = draw;
    fs_cmd1[cmd + 2u] = fs_cmd2[cmd + 2u];
    fs_cmd1[cmd + 3u] = fs_cmd2[cmd + 3u];
    fs_cmd1[cmd + 4u] = fs_cmd2[cmd + 4u];
    fs_cmd2[cmd + 1u] = 0u;
#else
    // stats: 0 tested, 1 drawn in phase 1, 2 drawn in phase 2, 3 outside the frustum, 4 hidden,
    // 5 triangles outside the frustum, 6 triangles hidden (4 and 6: not drawn at all), 7 no box,
    // 8 drawn in phase 1 but hidden now
    if (slot == 0u)
    {
        if (fs_collect != 0u)
        {
            atomicAdd(fs_stats[fs_stats_base + 7u], 1u);
        }
        return;
    }
    int result = fsTest(c, true);
    bool visible = result == 0;
    bool drawn = fs_cmd1[cmd + 1u] != 0u;
    fs_visible[slot] = visible ? 1u : 0u;
    fs_cmd2[cmd + 1u] = (visible && !drawn) ? 1u : 0u;
    if (fs_collect != 0u)
    {
        uint triangles = fs_entry[c * 8u + 7u];
        atomicAdd(fs_stats[fs_stats_base + 0u], 1u);
        if (drawn)
        {
            atomicAdd(fs_stats[fs_stats_base + 1u], 1u);
            if (!visible)
            {
                atomicAdd(fs_stats[fs_stats_base + 8u], 1u);
            }
        }
        else if (visible)
        {
            atomicAdd(fs_stats[fs_stats_base + 2u], 1u);
        }
        if (result == 1)
        {
            atomicAdd(fs_stats[fs_stats_base + 3u], 1u);
            atomicAdd(fs_stats[fs_stats_base + 5u], triangles);
        }
        else if (result == 2 && !drawn)
        {
            atomicAdd(fs_stats[fs_stats_base + 4u], 1u);
            atomicAdd(fs_stats[fs_stats_base + 6u], triangles);
        }
    }
#endif
}
