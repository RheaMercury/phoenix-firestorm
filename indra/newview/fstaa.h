/**
 * @file fstaa.h
 * @brief Temporal anti-aliasing (FSRenderTAA).
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
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

#ifndef FS_TAA_H
#define FS_TAA_H

#include "stdtypes.h"

class LLGLSLShader;
class LLRenderTarget;
class LLVertexBuffer;

extern LLGLSLShader gFSTAAResolveProgram;   // fsTAAResolveF.glsl
extern LLGLSLShader gFSTAASharpenProgram;   // fsTAASharpenF.glsl
extern LLGLSLShader gFSTAAOverlayDepthProgram; // fsTAAOverlayDepthF.glsl
extern LLGLSLShader gFSTAADepthCopyProgram;    // fsTAADepthCopyF.glsl

// Temporal anti-aliasing, stage 1: camera reprojection from depth.
//
// Each frame the main camera's projection is moved by a different sub-pixel offset (Halton 2,3
// over 8 frames, fstaajitter.h), so successive frames sample different points inside every
// pixel. renderFinalize then blends this frame's HDR scene with the accumulated history of the
// frames before, before bloom, exposure and tone mapping:
//  - Where each pixel was last frame comes from its depth: the position it shows, reconstructed
//    with this frame's (jittered) projection, projected with last frame's camera. The closest
//    depth around the pixel is used, so edges reproject with the surface in front.
//  - The history is read with a Catmull-Rom filter (sharp under motion) and clipped to the
//    variance box of this frame's 3x3 neighbourhood in YCoCg, so what moved by itself (avatars,
//    animated objects: stage 1 has no per-object motion) can only leave colours that are already
//    there; history that lands far outside the box is also partly dropped.
//  - Disocclusion: the history keeps each pixel's linear depth; where the reprojected depth does
//    not match any of the 3x3 history texels around, the history is dropped.
//  - Where a pixel did not move (a still camera), the history is clipped to the neighbourhood's
//    whole min and max instead of its variance box, keeps more weight, and the reactive mask and
//    the "moved by itself" test only act on colours the neighbourhood doesn't show: the jitter's
//    own differences average out instead of shimmering.
//  - Blending is done on colours compressed by 1 / (1 + exposed luminance), close to what the
//    tone mapper does, so bright highlights don't dominate and dark edges stay anti-aliased.
//  - Reactive mask: blended alpha, particles and water have no reliable depth. The scene is
//    copied right before the blended alpha (after the haze), and the depth before alpha is kept;
//    where the colour (softer) or the depth (water, rigged alpha) changed after that, the history
//    weight is cut.
//  - The resolved image goes back into the screen target as it is; the history keeps it
//    unsharpened. renderFinalize sharpens it once, with CAS after the tone mapping, at
//    FSRenderTAASharpness in place of CAS's own setting.
//  - Textures: the material shaders sample with a negative LOD bias while the jitter is on
//    (FSRenderTAATextureBias, fstaajitter.h), as the accumulated frames resolve finer detail
//    than one sample per pixel.
//  - FSRenderTAADebug (debug setting, 0 off) shows what the resolve decided in false colours
//    instead of the image: 1 history weight, 2 why the history was dropped, 3 motion, 4 the
//    jitter, 5 how much this frame differs from the history.
//
// TAA replaces FXAA and SMAA while it runs. Snapshots, avatars only snapshots, reflection probes,
// mirrors, impostors, HUDs and the UI are never jittered; snapshots use the anti-aliasing chosen
// in Graphics (FXAA or SMAA). A snapshot frame leaves the history as it was (the snapshot
// floater's preview refreshes every second or so), an avatars only one starts it over. It also
// starts over after a teleport, a region crossing (the agent origin moves), a camera jump, a
// resize (dynamic resolution steps too) and when TAA is switched on. The upscaler (dynamic resolution, NIS) runs
// after TAA, as before.
//
// Main thread only. With FSRenderTAA off nothing is allocated, nothing is jittered and every
// entry point returns after a cached bool read.
namespace FSTAA
{
    // display(), once a frame once the screen targets are sized: whether TAA runs this frame
    // (setting, HDR, not a snapshot) and this frame's jitter.
    void beginFrame(const LLRenderTarget& screen);
    bool frameActive();

    // The jitter on for its lifetime, when main_view (the pipeline's test for the main camera's
    // own passes) and TAA runs this frame. The first one of a frame also takes the camera's
    // matrices for the reprojection.
    class JitterScope
    {
    public:
        explicit JitterScope(bool main_view);
        ~JitterScope();
        JitterScope(const JitterScope&) = delete;
        JitterScope& operator=(const JitterScope&) = delete;
    private:
        bool mOn;
    };

    // renderGeomPostDeferred, main view, right before the blended alpha (screen bound as target):
    // the scene as it is now, for the reactive mask.
    void capturePreAlpha(LLRenderTarget& screen, bool main_view);

    // The pipeline's depth before alpha (fs_take_tag_depth): whether TAA wants it this frame, and
    // that it was taken.
    bool wantsPreAlphaDepth();
    void notePreAlphaDepth();

    // renderFinalize, first in the HDR chain: resolves screen against the history and writes the
    // result back into screen. False when TAA did not run this frame (the frame then takes the
    // anti-aliasing chosen in Graphics).
    bool resolve(LLRenderTarget& screen, LLRenderTarget& depth_source, LLRenderTarget* pre_alpha_depth,
                 LLRenderTarget* exposure, LLVertexBuffer* screen_triangle);
    // The CAS sharpness for a frame the resolve ran in (FSRenderTAASharpness, 0 to 1, in place of
    // RenderCASSharpness; 0 while FSRenderTAADebug shows its false colours)
    F32 casSharpness();

    // Selection highlights, "Highlight Transparent" and the debug displays. Drawn inside the
    // jittered alpha pass they moved by up to a pixel every frame (and went into the history), so
    // in a frame TAA runs the main view holds them back and renderFinalize draws them into the
    // resolved HDR scene, right after the resolve, with the plain projection. Their depth test then
    // uses a conservative depth: the nearest of the jittered scene depth's 2x2 texels around each
    // pixel's centre, built only when something will be drawn.
    //
    // renderGeomPostDeferred, main view: start holding them back (true when it does; also returns
    // true in dropped_stale when the last frame's were never drawn), and stop at its end.
    bool beginDeferOverlays(bool main_view, bool& dropped_stale);
    void endDeferOverlays();
    // While the main view's alpha pass holds them back: LLDrawPoolAlpha notes "Highlight
    // Transparent" instead of drawing it
    bool deferringOverlays();
    void noteDebugAlphaDeferred();
    // renderFinalize: whether this frame's were held back, taking them (and whether "Highlight
    // Transparent" was among them)
    bool takeDeferredOverlays(bool& debug_alpha);
    // renderFinalize, after the resolve: builds this frame's conservative depth (width x height,
    // the screen target's size, from the scene depth), when anything held back needs it
    bool prepareOverlayDepth(LLRenderTarget& depth_source, U32 width, U32 height, LLVertexBuffer* screen_triangle);
    // The same for the draws after the present, from the depth the present writes (the depth
    // before alpha when name tags ignore alpha): reuses the one above when that is the same depth
    bool preparePresentedDepth(LLRenderTarget& present_depth, U32 width, U32 height, LLVertexBuffer* screen_triangle);
    // Binds screen for drawing the held back ones, with this frame's conservative depth attached in
    // place of the scene's (or the scene's own when none was built); endOverlays puts the scene's
    // depth back and unbinds. depth_source is the target that owns screen's depth.
    void beginOverlays(LLRenderTarget& screen);
    void endOverlays(LLRenderTarget& screen, LLRenderTarget& depth_source);

    // Selection and hover outlines, beacons and physics shapes are drawn after the present, never
    // jittered, but tested against the depth the present wrote: the jittered scene depth, so an
    // outline flickered along its own object's edge. For their duration this puts this frame's
    // conservative depth into the bound framebuffer's depth (where it is nearer than what is there,
    // so HUDs stay in front) and puts the framebuffer's own depth back after, exactly (a GPU copy
    // of it is kept): name tags and everything else keep the present's depth. Does nothing unless
    // TAA ran this frame and preparePresentedDepth built the depth.
    class PresentedDepthScope
    {
    public:
        PresentedDepthScope();
        ~PresentedDepthScope();
        PresentedDepthScope(const PresentedDepthScope&) = delete;
        PresentedDepthScope& operator=(const PresentedDepthScope&) = delete;
    private:
        bool mOn;
    };
    // Debug beacons were added (LLViewerObjectList::addDebugBeacon), until render_ui_3d drew them
    void noteBeacon();
    bool beaconsPending();
    void beaconsDrawn();

    // The render targets go (resize, shader reload, shutdown).
    void release();
}

#endif // FS_TAA_H
