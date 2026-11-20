/**
 * @file fsreversez.h
 * @brief Reverse-Z depth (FSRenderReverseZ): near at depth 1, far at depth 0, in a float depth
 *        buffer, for depth precision at range.
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

#ifndef FS_REVERSEZ_H
#define FS_REVERSEZ_H

#include "stdtypes.h"
#include "llglheaders.h"
#include "glm/mat4x4.hpp"

// How it works. The C++ side keeps every projection matrix in the usual OpenGL form, so culling,
// picking, glm::project/unProject, oblique clip planes and every other piece of CPU maths is
// untouched. Only what reaches the GPU changes: while reverse-Z is active, the projection the
// shaders receive (LLRender::syncMatrices, the GPU-driven draws' matrix tables, and the few
// pipeline uniforms that decode depth) is R * P, where R takes the standard NDC depth z in [-1, 1]
// to (1 - z) / 2 in [0, 1]. With glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE) that is the window
// depth itself: 1 at the near plane, 0 at the far plane. R maps the whole standard clip volume onto
// the new one, so everything that was drawn before is still drawn; only the order of depths is
// reversed, which the flipped depth tests (LESS <-> GREATER), the depth clear to 0 and the negated
// polygon offsets follow. Shaders that read depth are built with REVERSE_Z.
//
// The shadow maps keep the standard convention (StandardScope around their generation): their
// orthographic depth is linear, so a reversed float buffer gains them nothing, and the shadow
// lookups, biases and polygon offsets stay exactly as they are.
//
// All of it is main thread only. With the setting off (or without glClipControl) every function
// here returns its input unchanged and issues no GL call.
namespace FSReverseZ
{
    // glClipControl (OpenGL 4.5 or GL_ARB_clip_control). A float depth buffer is core since 3.0.
    bool supported();

    // FSRenderReverseZ and supported(), decided when the shaders are (re)loaded: the shaders carry
    // REVERSE_Z and the render targets (but the shadow maps) have float depth.
    bool enabled();

    // The shader manager, while the shaders and render targets are unloaded. Makes the GL state
    // match (reverse-Z active when on).
    void setEnabled(bool on);

    // Reverse-Z is the depth convention of the GL state right now.
    bool active();

    // Sets the GL state for active() again: the clip convention, depth clear value and depth
    // function. Each frame, which also covers a lost and restored context.
    void applyState();

    // The standard depth convention for its lifetime (shadow maps). Nests.
    class StandardScope
    {
    public:
        StandardScope();
        ~StandardScope();
        StandardScope(const StandardScope&) = delete;
        StandardScope& operator=(const StandardScope&) = delete;
    private:
        bool mWasActive;
    };

    // The projection to rasterise with now: R * proj while active, proj otherwise.
    glm::mat4 rasterProjection(const glm::mat4& proj);

    // The projection that decodes what the camera's depth buffers hold: R * proj while enabled,
    // even inside a StandardScope (the shadow passes read the camera's depth to fit the cascades).
    glm::mat4 depthProjection(const glm::mat4& proj);

    // The GL depth function for a standard one (LESS <-> GREATER, LEQUAL <-> GEQUAL while active).
    GLenum glDepthFuncFor(GLenum func);

    // glDepthFunc(glDepthFuncFor(func)), remembering func so a change of convention can set it
    // again. LLGLDepthTest's only way to the depth function.
    void depthFunc(GLenum func);

    // glPolygonOffset with the offset turned toward the camera under either convention: negated
    // while active, as nearer is larger.
    void polygonOffset(F32 factor, F32 units);

    // The internal format of a new depth attachment: float while active (a target made inside a
    // StandardScope, a shadow map, stays 24 bit fixed point).
    GLenum depthFormat();

    // A depth value read back from a camera depth buffer, as the standard convention would hold it
    // (1 - d while enabled). For the depth snapshots.
    F32 toStandardDepth(F32 depth);
}

#endif // FS_REVERSEZ_H
