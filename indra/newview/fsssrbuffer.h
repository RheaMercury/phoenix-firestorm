/**
 * @file fsssrbuffer.h
 * @brief Screen-space reflections traced into their own buffer, blended over frames and blurred
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

#ifndef FS_SSRBUFFER_H
#define FS_SSRBUFFER_H

#include "stdtypes.h"

class LLGLSLShader;
class LLRenderTarget;

// The opaque surfaces' screen-space reflections, laid out after the graphics care package's SSR
// (Geenz, SLV project/graphics-care-package) so the two stay easy to compare and merge:
//
//  1. trace   (screenSpaceReflTraceF.glsl) into a buffer at
//             RenderScreenSpaceReflectionResolutionMultiplier times the screen, against the
//             previous frame's lit scene (hi-Z when FSRenderSSRHiZ is on)
//  2. resolve (screenSpaceReflResolveF.glsl) with the last frame's result, reprojected by the
//             camera motion and clipped to the neighbourhood's variance
//             (RenderScreenSpaceReflectionTemporal, RenderScreenSpaceReflectionTemporalBlend)
//  3. filter  (ssrDownsampleF.glsl) the result into blur mips for rough surfaces
//             (RenderScreenSpaceReflectionsFilterScale)
//
// before the lighting pass, which reads it back through softenLightF's FS_SSR_BUFFER upsample.
// Alpha and water keep tracing per pixel in their own passes.
namespace FSSSRBuffer
{
    // Builds the blur program (call from the deferred shader load; the trace and resolve programs
    // are built with the soften program's features in LLViewerShaderMgr)
    void loadShaders(S32 shader_level);

    // Runs the three passes for the screen target. Call with the screen bound and before the
    // soften program is bound. False when the soften program traces per pixel instead (SSR off,
    // a program did not build, a probe capture).
    bool render(LLRenderTarget* screen);

    // Binds the result to the soften program (after its bindDeferredShader); with rendered false
    // binds a black stand-in, the upsample returning early there
    void bind(LLGLSLShader& soften, LLRenderTarget* screen, bool rendered);

    void release();
}

#endif // FS_SSRBUFFER_H
