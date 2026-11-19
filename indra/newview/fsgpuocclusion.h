/**
 * @file fsgpuocclusion.h
 * @brief FSRenderGPUOcclusionCulling: two-phase occlusion culling of the GPU-driven G-buffer draws
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

#ifndef FS_GPUOCCLUSION_H
#define FS_GPUOCCLUSION_H

#include "llgl.h"
#include "stdtypes.h"

#include <vector>

class LLDrawInfo;
class LLFace;
class LLSpatialGroup;

// Two-phase occlusion culling (as in Haar and Aaltonen, "GPU-Driven Rendering Pipelines", 2015) of the
// batches a GPU-driven G-buffer pass draws with glMultiDrawElementsIndirect, main camera only:
//  - Phase 1, in the pass's usual place: a compute pass copies the pass's commands and keeps only the
//    batches that were visible last frame and are in the view frustum (instance count 1, the others 0);
//    the pass draws those.
//  - After every opaque pool has drawn (flushPhaseTwo): a pyramid of the farthest depth is built from the
//    depth buffer, and a second compute pass tests every batch's box against the frustum and the pyramid.
//    A visible batch not drawn in phase 1 is drawn now, by the pass's replay function, from a copy of its
//    inputs. The result is each batch's visibility for the next frame.
// With glMultiDrawElementsIndirectCount (OpenGL 4.6), each phase's commands that draw are packed to the front of
// their multi-draw's range (the pass's ranges, in their order) and the multi-draws read their count from the GPU
// (drawRange), so a phase draws no empty commands; without it they draw with instance count 0.
// Nothing is read back on the CPU (statistics aside: every 10 s, a few frames late, on the render thread).
// What is drawn is what the depth test would leave anyway: a batch is only left out when its box lies behind
// depth already in the buffer. Boxes are in the space a batch's vertices are in (the face extents its batch
// was built from), transformed by the batch's own modelview projection matrix on the GPU, so moving objects
// need no update. Rigged batches get their avatar's extents, padded, each frame (not the own avatar's). Batches
// without a reliable box (flexible, animated children, vertices rewritten in place since, the own avatar's rigged
// meshes) are always drawn in phase 1.
namespace FSGPUOcclusion
{
    // per command, std430: the box (model space) and the batch's visibility slot (0: no box, always drawn),
    // the triangles it draws (statistics)
    struct CullEntry
    {
        F32 mMin[3];
        U32 mSlot;
        F32 mMax[3];
        U32 mTriangles;
    };

    // Which kind of pass culls (the statistics are kept per kind)
    enum EKind
    {
        KIND_PBR = 0,
        KIND_MATERIALS,
        KIND_LEGACY,
        KIND_RIGGED,        // the rigged passes of all three
        KIND_COUNT
    };

    // A multi-draw of a pass: its commands [mFirst, mFirst + mCount)
    struct Range
    {
        U32 mFirst;
        U32 mCount;
    };

    // what a pass's draw records and matrix table look like, and where its inputs are on the GPU now
    struct PassInputs
    {
        U32 mKind = KIND_PBR;
        const Range* mRanges = nullptr; // the pass's multi-draws, in the order it draws them (drawRange's range)
        U32 mRangeCount = 0;
        GLuint mCommands = 0;           // the indirect commands (FSGDDDrawCommand), as uploaded
        size_t mCommandsOffset = 0;
        U32 mCommandCount = 0;
        GLuint mRecords = 0;            // the per command draw records
        size_t mRecordsOffset = 0;
        size_t mRecordBytes = 0;
        U32 mRecordStride = 4;          // uints per record
        U32 mMatrixField = 0;           // the record's uint naming the batch's first matrix
        U32 mMVPOffset = 1;             // the modelview projection matrix, from that first matrix
        GLuint mMatrices = 0;           // the matrix table (mat4s)
        size_t mMatricesOffset = 0;
        size_t mMatrixBytes = 0;
        GLuint mMaterials = 0;          // the material table (any layout; copied for phase 2), 0 for none
        size_t mMaterialsOffset = 0;
        size_t mMaterialBytes = 0;
    };

    // Phase 2 of a pass: draws what replayCommands(pass) holds with the pass's programs, from
    // replayRecords/Matrices/Materials(pass). user: what the pass gave addReplay.
    typedef void (*replay_fn)(S32 pass, U32 user);

    // FSRenderGPUOcclusionCulling (cached)
    bool enabled();
    // The setting changed (llviewercontrol.cpp): turned on, the scene's batches are rebuilt to get their boxes
    void onSettingChanged(bool on);

    // LLVolumeGeometryManager::registerFace: a new batch from facep, or facep added to batch info
    void initBatchBounds(LLDrawInfo& info, LLFace* facep);
    void extendBatchBounds(LLDrawInfo& info, LLFace* facep);
    // LLVolumeGeometryManager::rebuildMesh: the group's vertices are about to be rewritten in place (its batches
    // lose their boxes), and have been (they get them again from their faces' extents, where every face has one)
    void invalidateGroupBounds(LLSpatialGroup* group);
    void refreshGroupBounds(LLSpatialGroup* group);
    // ~LLDrawInfo
    void releaseSlot(U32 slot);

    // Around the main camera's G-buffer (llviewerdisplay.cpp)
    void beginFrame(bool for_snapshot);
    void endFrame();
    // After the last opaque pool (LLPipeline::renderGeomDeferred): the pyramid, phase 2, the replays
    void flushPhaseTwo();

    // A GPU-driven pass may cull now (main camera G-buffer, before flushPhaseTwo)
    bool passActive();
    // The cull entry of a batch the pass draws as a command (rigged ones: from their avatar, in agent space)
    void makeEntry(LLDrawInfo& info, CullEntry& entry);
    // Phase 1 for a pass whose inputs are uploaded: >= 0, the pass then draws with phaseOneCommands(pass)
    // as its indirect buffer (offset 0, same layout); -1: draw as usual.
    S32 cullPhaseOne(const PassInputs& inputs, const std::vector<CullEntry>& entries);
    GLuint phaseOneCommands(S32 pass);
    // The pass drew with phaseOneCommands: phase 2 will run for it and call replay (else it is left out,
    // and its batches' visibility stays as it was)
    void addReplay(S32 pass, replay_fn replay, U32 user);
    // The multi-draw of range (of PassInputs::mRanges) with the commands bound as the indirect buffer
    // (phaseOneCommands in phase 1, replayCommands in phase 2): with the commands packed and their count read
    // from the GPU when they were, else as glMultiDrawElementsIndirect of the whole range
    void drawRange(S32 pass, bool phase2, U32 range, U32 first, U32 count);
    // For the replay: indirect commands, records, matrices, materials (offset 0 each)
    GLuint replayCommands(S32 pass);
    GLuint replayRecords(S32 pass);
    GLuint replayMatrices(S32 pass);
    GLuint replayMaterials(S32 pass);
    // For the replay of a rigged pass whose palette offsets went void (the palette buffer was replaced since they
    // were taken, FSSkinPaletteBuffer::generation): puts the palettes of params (one per command) in the buffer
    // again, writes their offsets into field `field` of records (stride uints per record, a copy of the pass's
    // records) and records into the pass's copy, and generation to what they hold for. False if that can't be.
    bool refreshPalettes(S32 pass, const std::vector<LLDrawInfo*>& params, std::vector<U32>& records, U32 stride,
                         U32 field, U32& generation);

    // The GL objects (with the GPU-driven passes', fsGDDReleaseGL / fsGDDUnloadShaders)
    void releaseGL();
}

#endif // FS_GPUOCCLUSION_H
