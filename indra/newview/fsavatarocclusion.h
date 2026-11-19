/**
 * @file fsavatarocclusion.h
 * @brief Soft ambient occlusion around avatars from capsules on their skeletons (FSRenderAvatarOcclusion)
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

#ifndef FS_AVATAROCCLUSION_H
#define FS_AVATAROCCLUSION_H

#include "stdtypes.h"

class LLGLSLShader;

// Avatars darken the ambient light around them: the ground under their feet, a wall they lean on,
// the avatar next to them (GPU Gems 2 ch. 14 in its modern form, capsule occlusion as in The Last of
// Us and Unreal's capsule shadows, with Quilez's sphere occlusion).
//
// Each frame, for the nearest avatars within range, a set of capsules is built on the CPU from
// their skeleton joints (pelvis, spine, chest, neck, head, upper and lower arms and legs, feet),
// sized by the avatar's own proportions, and uploaded in eye space as a uniform block. The soften
// (sun and ambient) pass, through fsAvatarOcclusionF.glsl (FS_AVATAR_OCCLUSION), takes the analytic
// occlusion of those capsules out of the ambient light of every pixel within an avatar's reach,
// together with a soft shadow of the open sky above, and combines it with the screen-space ambient
// occlusion by taking the darker of the two, so they don't add up where both see the avatar.
// The joints are read on the main thread at render time, after the frame's pose update has finished.
// Reflection probe captures get no capsules. Animesh (control avatars) are left out: their skeleton
// need not be a human shape.
namespace FSAvatarOcclusion
{
    // Binding point of the FSAvatarCapsules uniform block (the batched lights use 14 and 15)
    constexpr U32 UBO_BINDING = 13;
    constexpr U32 MAX_AVATARS = 16;
    constexpr U32 CAPSULES_PER_AVATAR = 14;

    // FSRenderAvatarOcclusion: whether the soften program gets the permutation
    bool enabled();

    // Builds this view's capsules and binds them for the bound soften program, which must have
    // been built with FS_AVATAR_OCCLUSION (a no-op otherwise). The eye space is the current
    // modelview's.
    void bind(LLGLSLShader& shader);

    // Release the GL objects (LLPipeline::cleanup)
    void release();
}

#endif // FS_AVATAROCCLUSION_H
