/**
 * @file fsgpuworkdiag.h
 * @brief FSDiagnosticsGPUWork: repeated geometry, hidden geometry and texture memory by visibility, in the log
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

#ifndef FS_GPUWORKDIAG_H
#define FS_GPUWORKDIAG_H

class LLCamera;
class LLCullResult;

// Numbers for sizing three possible projects (instancing repeated meshes, GPU occlusion culling of the
// multi-draws, texture streaming from GPU mip demand), logged under FSGPUWork every 10 seconds while
// FSDiagnosticsGPUWork is on:
// - Repeats: how many in-world volume objects share one LLVolume (the same mesh asset or prim shape at the
//   same LOD), and the vertices and bytes their per-object copies take.
// - Occlusion: of the groups in view, how many the occlusion queries hide and how much geometry they hold;
//   and of what is drawn, how many objects (and triangles) are behind the finished G-buffer depth anyway,
//   tested with one box query per object on one frame in ten seconds.
// - Textures: texture memory split by where the faces using it are: visible, in view but only in occluded
//   groups, or off screen.
// With the setting off nothing runs beyond a cached bool read.
namespace FSGPUWorkDiag
{
    // Once per frame, main thread, outside rendering: every 10 s walks the objects and textures, logs, and
    // asks for the next sample frame.
    void frame();
    // From LLPipeline::renderGeomDeferred after the G-buffer pools, while the G-buffer depth is bound: reads
    // the box queries that are in, without waiting, and issues the next sample when asked for.
    void afterGBuffer(LLCamera& camera, LLCullResult* cull);
    // From LLOcclusionCullingGroup::checkOcclusion: a group query result for the main camera.
    void noteGroupQuery(bool occluded);
}

#endif // FS_GPUWORKDIAG_H
