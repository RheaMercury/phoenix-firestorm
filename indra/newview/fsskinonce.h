/**
 * @file fsskinonce.h
 * @brief Rigged meshes skinned once a frame in a batched compute pre-pass (FSRenderSkinOnce)
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

#ifndef FS_SKINONCE_H
#define FS_SKINONCE_H

#include "stdtypes.h"
#include "llvertexbuffer.h"

#include <map>
#include <string>

class LLMeshSkinInfo;
class LLVOAvatar;

// Rigged meshes are skinned in the vertex shader of every pass that draws them: the G-buffer, each sun
// cascade, projector shadows, probe and mirror captures, the rigged alpha passes, impostors. With
// FSRenderSkinOnce on, the vertex buffers that the stock draw loops skinned at least FSRenderSkinOnceMinPasses
// times over (vertices drawn / vertices held) last frame are skinned once at the start of this frame, before
// the sun shadows, by one compute pass (avatar/fsSkinOnceC.glsl: one dispatch per buffer, one barrier at the
// end) into one output buffer. Every later stock draw of such a buffer (LLVertexBuffer::drawRange, through
// LLVertexBuffer::sFSSkinOnceHook) points its position, normal and tangent attributes at the skinned copy and
// sets fs_skin_once, which makes the skinning code (avatar/objectSkinV.glsl) take an identity blend; the
// palette and skin_origin stay bound as before and the programs transform as before.
//
// A draw uses the copy only if the copy was made this frame from the buffer's current content
// (LLVertexBuffer::getFSContentSerial) with the palette build the draw itself bound (noteSkin, from the palette
// upload: MatrixPaletteCache::mFSBuildSerial). Everything else (new buffers, avatars that just came into view,
// a LOD change, buffers skinned too few times, the output budget used up, the GPU-driven rigged multi-draws,
// which read their own arenas) is skinned in the shader as before.
//
// The earlier attempt (removed in ff11531fa04) captured each draw's skin matrices with transform feedback
// right before the draw and read 48 bytes a vertex back in every pass: the GPU waited on every capture.
// Here nothing is captured between draws, the passes read fewer bytes than they did (no weights) and do no
// blending, and only buffers drawn often enough are skinned ahead.
namespace FSSkinOnce
{
    // FSRenderSkinOnce is on
    bool wanted();

    // From the global define setup (LLViewerShaderMgr): when wanted and the GL can run compute shaders, adds
    // FS_SKIN_ONCE and installs the draw hook; otherwise removes the hook and releases everything.
    void configureShaders(std::map<std::string, std::string>& global_defines);

    // Removes the draw hook, releases the program, the buffers and every kept buffer, avatar and skin (shader
    // unload, logout)
    void release();

    // Once a frame (the first call of a frame does the work), main thread, before the first pass that draws
    // rigged meshes: the pre-pass.
    void beginFrame();

    void noteSkinImpl(const LLVOAvatar* avatar, const LLMeshSkinInfo* skin, U32 palette_serial);

    // The palette the following rigged draws use, from LLRenderPass::uploadMatrixPalette
    inline void noteSkin(const LLVOAvatar* avatar, const LLMeshSkinInfo* skin, U32 palette_serial)
    {
        if (LLVertexBuffer::sFSSkinOnceHook)
        {
            noteSkinImpl(avatar, skin, palette_serial);
        }
    }
}

#endif // FS_SKINONCE_H
