/**
 * @file fsskinpalettebuffer.h
 * @brief Rigged mesh joint palettes written once per frame into one uniform buffer (FSPerfRiggedPaletteBuffer)
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

#ifndef FS_SKINPALETTEBUFFER_H
#define FS_SKINPALETTEBUFFER_H

#include "stdtypes.h"

#include <vector>

class LLDrawInfo;
class LLVector3;

// Stock rigged drawing uploads each draw's joint palette (up to 110 3x4 matrices) into the bound
// program's matrixPalette uniform, again for every program, pass and shadow cascade that draws the
// same mesh: in a crowd over a thousand uploads and a few MB a frame, all of it recorded and
// replayed by the render thread. With FSPerfRiggedPaletteBuffer the rigged programs read the
// palette from the uniform block FSSkinPalette instead (objectSkinV.glsl, FS_SKIN_PALETTE_BUFFER),
// and each palette build (MatrixPaletteCache::mFSBuildSerial) is written once a frame into one
// buffer; a draw only binds its range.
//
// FSPerfGPUDrivenRigged multi-draws (fsgpudrivenmaterials.cpp, fsgpudrivenpbr.cpp, fsgpudrivenlegacy.cpp) bind the same buffer whole as a shader
// storage buffer instead (objectSkinV.glsl built with FS_GDD_RIGGED) and give each command its palette's
// offset in vec4s. Offsets hold only while the storage does: it is orphaned at the first palette of a frame
// and when it fills up (all offsets start over in the new storage), which generation() counts; a multi-draw
// must not use offsets taken under another generation.
// Skin SSS (FSRenderSkinSSS): the top bit of a multi-draw record's palette offset is set for a batch that doesn't
// mark its pixels as avatar skin (LLRenderPass::fsSkinExcluded), which the GPU-driven rigged programs read in place
// of the stock programs' fs_skin_exclude uniform; objectSkinV.glsl masks it off the offset. Offsets are in vec4s,
// far below 2^31.
constexpr U32 FS_GDD_SKIN_EXCLUDE_BIT = 0x80000000u;

namespace FSSkinPaletteBuffer
{
    // FSPerfRiggedPaletteBuffer is on; the shaders are built for it while this holds
    bool wanted();

    // Whether the shaders now loaded read the block (set where the global defines are made)
    void setShadersBuilt(bool built);
    bool active();

    // Writes the palette with this build serial (0: always a new copy) unless this frame's buffer
    // has it, and binds its range to the FSSkinPalette block. count matrices of 12 floats each.
    void bind(U32 serial, const F32* matrices, U32 count, const LLVector3& skin_origin);

    // As bind, without binding: the byte offset of the palette (a multiple of 16) in the buffer's current
    // storage, written there first unless it has it. U32_MAX without a buffer.
    U32 offsetOf(U32 serial, const F32* matrices, U32 count, const LLVector3& skin_origin);
    // Moves on whenever the storage is replaced (every offset taken before is void)
    U32 generation();
    // The GL buffer (0 before the first palette)
    U32 glName();

    // FSPerfGPUDrivenRigged multi-draws (fsgpudrivenpbr.cpp, fsgpudrivenlegacy.cpp): whether the stock rigged
    // loops draw the batch at all (LLRenderPass::uploadMatrixPalette: an avatar, a skin, and its palette built,
    // i.e. the skin loaded). Builds the palette if this frame hasn't yet, as the stock loop would.
    bool drawable(LLDrawInfo& params);
    // Puts the palettes of drawn (drawable batches, one multi-draw command each) and of stock (batches the
    // same pass draws the stock way, whose bind then only finds them) into the buffer, all in one storage:
    // again if it was replaced meanwhile (the first palette of a frame, or the buffer full). offsets gets each
    // drawn batch's palette offset in vec4s, generation the generation they hold for. False if the buffer
    // never held still, or there is none; then the pass must be drawn the stock way.
    bool place(const std::vector<LLDrawInfo*>& drawn, const std::vector<LLDrawInfo*>& stock, std::vector<U32>& offsets,
               U32& generation);

    void release();
}

#endif // FS_SKINPALETTEBUFFER_H
