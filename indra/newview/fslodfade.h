/**
 * @file fslodfade.h
 * @brief Cross-dissolve of object detail changes (FSRenderFadeLOD)
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

#ifndef FS_LODFADE_H
#define FS_LODFADE_H

#include "stdtypes.h"

class LLCamera;
class LLCullResult;
class LLDrawable;
class LLSpatialGroup;

// When an object switched detail level (its LOD, or a mesh's better LOD arriving), its old shape
// vanished and the new one appeared in the same frame. With FSRenderFadeLOD (on top of FSRenderFadeIn,
// whose screen-door dissolve it uses) the two cross-dissolve over FSRenderFadeInTime instead:
//
//  - LLVOVolume::lodOrSculptChanged: before the new volume is set, the batches of the object's group
//    that hold its faces are found by vertex buffer and index range (capture), and kept if the detail
//    did change (commit).
//  - LLSpatialGroup::clearDrawMap: when that group is rebuilt, the kept batches leave its draw map
//    without being freed. They are marked as fading out from now on, and the object's new batches,
//    built right after, fade in from the same moment (fadeInStart, in registerFace).
//  - LLPipeline::postSort: for the main view, the kept batches of visible groups are added to the
//    render map. LLRenderPass::fsFadeInHidden hands them a negative fs_fade_hide, and fsFadeInF.glsl
//    shows each one exactly where the new batches are hidden, so every pixel gets one of the two.
//
// Only opaque and alpha-masked passes whose programs have the dissolve take part (not blended alpha,
// which the alpha pool draws from the group itself, not glow, not rigged meshes); attachments,
// animated objects and HUDs are left out, as from the fade-in. Shadows, reflection probes, mirrors and
// water reflections draw only the new shape. A batch that held other objects' faces as well shows
// those twice for the moment, the same geometry in the same place.
//
// The kept batches hold their vertex buffers, textures, object and group, and are dropped once the
// dissolve is over, when the object or group dies, or on a teleport. Nothing is captured or kept, and
// no lookup made, while the setting is off.
namespace FSLODFade
{
    // FSRenderFadeLOD with FSRenderFadeIn (whose shader permutation it needs)
    bool active();

    // whether batches of this render pass type take part
    bool fadesPass(U32 type);

    // LLVOVolume::lodOrSculptChanged, before the volume changes: the batches holding drawable's faces
    void capture(LLDrawable* drawable);
    // and after: keeps them if the detail changed, else drops them
    void commit(LLDrawable* drawable, bool changed);

    // LLSpatialGroup::clearDrawMap: starts the dissolve of what was kept from this group
    void onClearDrawMap(LLSpatialGroup* group);

    // registerFace: when the new batches of drawable in pass type start fading in, -1 for none
    F32 fadeInStart(const LLDrawable* drawable, U32 type);

    // LLPipeline::postSort, main view: the batches fading out of visible groups go into the render map
    void pushFadingOut(LLCullResult* cull);

    // drops everything kept (teleport, shutdown)
    void clear();
}

#endif // FS_LODFADE_H
