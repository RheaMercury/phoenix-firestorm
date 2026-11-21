/**
 * @file fsmirrorplanar.h
 * @brief Planar reflections for flat mirrors (FSRenderMirrorPlanar)
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

#ifndef FS_MIRRORPLANAR_H
#define FS_MIRRORPLANAR_H

#include "stdtypes.h"
#include "v3math.h"

class LLCamera;
class LLCubeMapArray;
class LLGLSLShader;
class LLPlane;
class LLVOVolume;

// A mirror (hero probe) is a cube map centred on the camera reflected through the mirror plane,
// looked up along the reflected view direction. For a flat mirror only the directions through the
// mirror itself are ever looked up, a small part of the cube. With FSRenderMirrorPlanar on, a flat
// mirror is rendered as one view from that same reflected eye, fitted to the part of the mirror the
// camera sees, at a resolution that follows its size on screen, and the mirror surface reads it with
// the same direction lookup the cube used (FS_HERO_PLANAR in reflectionProbeF.glsl). Thick or curved
// mirrors, and views the fit cannot cover, keep the cube.
//
// With the setting off nothing here is used and the mirrors render exactly as before.
namespace FSMirrorPlanar
{
    // The view LLViewerWindow::cubeSnapshot renders in place of a cube face, set only while the
    // planar view renders
    struct View
    {
        LLVector3   mAt;
        LLVector3   mUp;
        F32         mFov = 1.f;     // vertical, radians
        F32         mAspect = 1.f;
    };

    // FSRenderMirrorPlanar (the shaders read the planar view only when they were built with it on)
    bool enabled();

    // Non-null only inside the planar render
    const View* currentView();

    // cubeSnapshot calls this once the planar camera is set up, with the camera it renders with
    // (after its own field of view and aspect clamps)
    void recordCamera(const LLCamera& camera);

    // Far clip for the planar view (FSRenderMirrorDrawDistance, never past the main view's)
    F32 drawDistance(F32 main_far);

    // FSRenderMirrorAvatars: avatars, animesh and particles in every mirror, not only "dynamic" ones
    bool forceDynamic();

    // How much a mirror candidate fills the view, for picking the mirror to render in planar mode
    F32 selectionScore(LLVOVolume* vo, const LLVector3& normal, const LLVector3& camera_pos);

    // Renders the mirror as one planar view. True: handled this frame (rendered, or deliberately
    // skipped because it is off screen, tiny or not due), the caller skips the cube faces.
    // False: the mirror can't be done as a plane, the caller renders the cube as before.
    bool render(LLVOVolume* hero, const LLVector3& mirror_pos, const LLVector3& mirror_normal,
                const LLVector3& virtual_eye, LLCubeMapArray* cube, bool dynamic, U32 cube_res, U32 cube_mips);

    // The cube path is about to render: the shaders read the cube again, and the mirror render
    // target goes back to the cube's size once the cube has been in use for a while. True when the
    // cube wasn't rendered last frame (the planar view was in use), so all its faces are out of date.
    bool useCube(U32 cube_res, const LLVector3& mirror_pos, const LLVector3& mirror_normal);

    // The reflection cube's faces (bit per face, LLViewerWindow::cubeSnapshot's order) that no
    // reflection off a flat mirror can reach from the reflected eye: the mirror, a little larger for
    // normal maps, seen from there misses them. 0 for a thick or curved mirror.
    U32 unreachedCubeFaces(LLVOVolume* hero, const LLVector3& mirror_pos, const LLVector3& virtual_eye);

    // The mirror plane as a culling plane for the world camera of the mirror pass
    bool cullPlane(LLPlane& plane);

    // Binds the planar view and its uniforms (LLPipeline::bindReflectionProbes)
    void bind(LLGLSLShader& shader, bool set_uniforms);

    // Mirrors normally don't update while a snapshot renders; the planar view does, so a snapshot
    // with another aspect ratio or a part of the mirror that was off screen still gets all of it
    bool updateDuringSnapshot();

    // Release the GL objects (LLHeroProbeManager::cleanup)
    void cleanup();
}

#endif // FS_MIRRORPLANAR_H
