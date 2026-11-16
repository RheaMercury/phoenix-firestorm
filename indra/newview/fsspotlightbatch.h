/**
 * @file fsspotlightbatch.h
 * @brief Instanced drawing of deferred spotlights that cast no shadow
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, Trish
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

#ifndef FS_SPOTLIGHTBATCH_H
#define FS_SPOTLIGHTBATCH_H

#include "llpointer.h"
#include "v3color.h"
#include "v4math.h"

#include <vector>

class LLCamera;
class LLDrawable;
class LLGLSLShader;
class LLVertexBuffer;
class LLViewerTexture;

// Draws the deferred spotlights that hold no shadow slot as instanced light volumes
// (FSPerfBatchedSpotLights). LLPipeline::renderDeferredLighting still decides which lights
// are drawn, which of them the camera is inside of, and ranks every one for the shadow slots;
// with batching on it queues each unshadowed one here instead of calling setupSpotLight
// (a matrix inverse, about fifteen uniforms and a texture bind) and drawing it. flush() uploads
// every queued light's projection once, sorts them by projection texture and by whether the
// camera is inside, and draws each group with one instanced call per ~100 lights.
//
// Lights the camera is outside of draw the faces of their box toward the camera with the
// stock LEQUAL depth test, as the stock box path. Lights the camera is inside of (the stock
// rule: within the box grown by 0.2 m) would each be a full screen pass on the stock path;
// here they draw every face of their box with GEQUAL and depth clamping, so only the pixels
// whose surface lies inside the box are shaded. See flush() for why that drops no pixel the
// stock pass lights. Shadowed lights keep the stock paths untouched.
class FSSpotLightBatch
{
public:
    // vec4s per light in the FSSpotLights block (fsBatchedSpotLightV.glsl / F.glsl):
    // [0] centre (agent space) + size   [1] linear colour + falloff
    // [2..5] screen to light projection [6] proj_p + proj_near   [7] proj_n + proj_range
    // [8] proj_origin + proj_ambiance   [9] proj_focus, proj_lod, proj_ambient_lod, 0
    static constexpr U32 FS_VEC4_PER_LIGHT = 10;
    // vec4s in the shader's block: 16 KB, the minimum GL_MAX_UNIFORM_BLOCK_SIZE.
    static constexpr U32 FS_BLOCK_VEC4 = 1024;
    // Uniform buffer binding point: past LLGLSLShader::UniformBlock (0-4), below the batched
    // point lights' 15.
    static constexpr U32 FS_UBO_BINDING = 14;

    // Start of a deferred lighting pass. True when batching is on and usable this frame
    // (the setting, the program and the uniform buffer alignment); the queue is cleared.
    bool begin();

    // Queue one spotlight: its projection exactly as setupSpotLight computes it against the
    // current modelview. linear_color and falloff as the stock loops send them (colour already
    // scaled). inside: the camera is within the light's box (renderDeferredLighting's rule).
    void add(LLDrawable* drawablep, bool inside, const LLColor3& linear_color, F32 falloff);

    // Upload and draw everything queued since begin(). The lighting target, additive blend
    // and depth buffer are those renderDeferredLighting already has bound.
    void flush(LLCamera* camera);

    // Point the program's FSSpotLights block at FS_UBO_BINDING. Called once per link.
    static void setupProgram(LLGLSLShader& shader);

    // Release the GL objects (LLPipeline::cleanup).
    void release();

    // Last flush, for the FSLights stats.
    U32 frameLights() const { return mFrameLights; }
    U32 frameGroups() const { return mFrameGroups; }
    U32 frameDraws() const { return mFrameDraws; }

private:
    struct Light
    {
        LLPointer<LLViewerTexture> mTexture;    // projection texture, white when the light has none
        U32                        mFirst;      // first vec4 in mStaging
        bool                       mInside;
    };

    std::vector<Light>        mLights;
    std::vector<LLVector4>    mStaging;         // FS_VEC4_PER_LIGHT per light, in add() order
    std::vector<LLVector4>    mData;            // the upload, in draw order
    LLPointer<LLVertexBuffer> mBoxVB;           // 6 faces x 2 triangles, unindexed, [-1, 1] cube
    U32                       mUBO = 0;
    S32                       mUBOAlignment = -1; // GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, queried once

    U32 mFrameLights = 0;
    U32 mFrameGroups = 0;
    U32 mFrameDraws  = 0;
};

extern FSSpotLightBatch gFSSpotLightBatch;

#endif // FS_SPOTLIGHTBATCH_H
