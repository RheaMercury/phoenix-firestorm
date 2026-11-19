/**
 * @file fsOcclusionCompactC.glsl
 * @brief FSRenderGPUOcclusionCulling: packs the commands a cull kept to the front of each multi-draw's range (fsgpuocclusion.cpp)
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

// Compiled by fsgpuocclusion.cpp with "#version 430 core" in front. One work group per range: a run of
// commands the pass draws with one multi-draw call. The commands of the range that draw (instance count not
// 0) are copied, in their order, to the front of the same range in fs_dst, and how many there are goes into
// fs_counts[fs_count_base + range], which glMultiDrawElementsIndirectCount reads as the draw count. Keeping the
// order keeps which of two surfaces at exactly the same depth wins the same from frame to frame.

layout(local_size_x = 256) in;

// the cull's commands, 5 uints each: count, instance count, first index, base vertex, base instance
layout(std430, binding = 0) readonly buffer FSOccCompactSrc
{
    uint fs_src[];
};

layout(std430, binding = 1) writeonly buffer FSOccCompactDst
{
    uint fs_dst[];
};

// per range: its first command, its command count
layout(std430, binding = 2) readonly buffer FSOccCompactRanges
{
    uint fs_range[];
};

layout(std430, binding = 3) writeonly buffer FSOccCompactCounts
{
    uint fs_counts[];
};

uniform uint fs_count_base;     // where this list's counts start in fs_counts

shared uint fs_scan[256];

void main()
{
    uint range = gl_WorkGroupID.x;
    uint lid = gl_LocalInvocationID.x;
    uint first = fs_range[range * 2u];
    uint count = fs_range[range * 2u + 1u];

    // the same count for the whole group, so every invocation takes every barrier
    uint total = 0u;
    for (uint base = 0u; base < count; base += 256u)
    {
        uint i = base + lid;
        uint src = (first + i) * 5u;
        uint keep = 0u;
        if (i < count && fs_src[src + 1u] != 0u)
        {
            keep = 1u;
        }

        // inclusive prefix sum of keep over the group
        fs_scan[lid] = keep;
        memoryBarrierShared();
        barrier();
        for (uint offset = 1u; offset < 256u; offset <<= 1u)
        {
            uint add = lid >= offset ? fs_scan[lid - offset] : 0u;
            barrier();
            fs_scan[lid] += add;
            memoryBarrierShared();
            barrier();
        }
        uint kept = fs_scan[255];

        if (keep != 0u)
        {
            uint dst = (first + total + fs_scan[lid] - 1u) * 5u;
            fs_dst[dst] = fs_src[src];
            fs_dst[dst + 1u] = 1u;
            fs_dst[dst + 2u] = fs_src[src + 2u];
            fs_dst[dst + 3u] = fs_src[src + 3u];
            fs_dst[dst + 4u] = fs_src[src + 4u];
        }
        total += kept;
        barrier();      // every invocation read fs_scan before the next chunk writes it
    }
    if (lid == 0u)
    {
        fs_counts[fs_count_base + range] = total;
    }
}
