/**
 * @file fsclusteredlights.h
 * @brief Clustered shading of the deferred local point lights
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

#ifndef FS_CLUSTEREDLIGHTS_H
#define FS_CLUSTEREDLIGHTS_H

#include "llglslshader.h"
#include "v3color.h"
#include "v4math.h"

#include "glm/mat4x4.hpp"

#include <vector>

class LLVertexBuffer;

// The local point lights of the main view, shaded in one full screen pass (FSRenderClusteredLights)
// instead of a light volume or a share of a full screen pass each.
//
// LLPipeline::renderDeferredLighting still decides which lights are drawn: RenderLocalLightCount,
// attached lights, the size and colour cut-offs and the frustum test all run in the stock loop,
// which hands each point light that passes them to add() instead of drawing it. flush() then sorts
// them into clusters, the cells of a grid of 64 pixel screen tiles by 16 depth slices (spaced
// evenly in log depth), each cluster listing the lights whose sphere can reach into it: per depth
// slice, the part of the sphere inside the slice is boxed and the screen tiles that box covers
// get the light, a conservative test. Light data and the lists go up as two buffer textures, and
// fsClusteredLightF.glsl shades every pixel with the lights of its own cluster only, with the
// stock point light maths. Projectors, spotlights and reflection probe captures keep the stock
// passes. The binning runs on the CPU, so it works on OpenGL 3.3 and macOS alike.
class FSClusteredLights
{
public:
    static constexpr U32 TILE_PIXELS = 64;
    static constexpr U32 DEPTH_SLICES = 16;

    // Start of the main view's local light pass, before the stock loop. Clears the light list.
    // True when the loop should hand its point lights to add() instead of drawing them: the
    // setting is on and the program, buffer textures and texture units are usable. Everything
    // that could stop flush() from drawing the lights is checked here.
    bool begin(const glm::mat4& modelview, const glm::mat4& projection, U32 width, U32 height);

    // One point light that passed the stock loop's tests: agent space centre, volume half size
    // (radius * 1.5, as the stock path sends), linear colour already scaled, and falloff. False
    // when the light is not taken, and the stock passes must draw it.
    bool add(const F32* center_agent, F32 size, const LLColor3& linear_color, F32 falloff);

    // After the stock loop: bin, upload and draw the lights into the bound lighting target
    // (additive blend, as the stock light passes). screen_tri is the full screen triangle.
    void flush(LLVertexBuffer* screen_tri);

    // Release the GL objects (LLPipeline::cleanup).
    void release();

private:
    void binLights();
    void upload();
    // Binds the two buffer textures to texture units past the shader's own samplers and sets
    // the grid uniforms.
    void bindBuffers(LLGLSLShader& shader);
    void unbindBuffers();

    struct Span
    {
        U32 mLight;
        U32 mSlice;
        U32 mX0, mX1, mY0, mY1; // tiles, inclusive
    };

    glm::mat4 mModelview = glm::mat4(1.f);
    glm::mat4 mProjection = glm::mat4(1.f);
    U32 mWidth = 1;
    U32 mHeight = 1;
    bool mActive = false;

    std::vector<LLVector4> mLights;     // per light: eye space centre + size, colour + falloff
    F32 mFarDepth = 1.f;                // farthest depth any light reaches

    // Grid of the last binning
    U32 mTilesX = 1;
    U32 mTilesY = 1;
    U32 mSlices = 1;
    F32 mSliceScale = 1.f;              // slices per unit of log depth
    F32 mLogNear = 0.f;

    std::vector<Span> mSpans;
    std::vector<U32> mCounts;
    std::vector<U32> mFill;
    std::vector<U32> mLists;            // header (offset, count per cluster), then the light indices

    U32 mLightBuffer = 0;
    U32 mLightTexture = 0;
    U32 mListBuffer = 0;
    U32 mListTexture = 0;
    S32 mMaxTexels = -1;                // GL_MAX_TEXTURE_BUFFER_SIZE, queried once
    S32 mUnits[2] = { -1, -1 };         // texture units of the bound buffers
    bool mWarned = false;
};

extern FSClusteredLights gFSClusteredLights;

extern LLGLSLShader gFSClusteredLightProgram; // fsClusteredLightF.glsl, built in loadShadersDeferred

#endif // FS_CLUSTEREDLIGHTS_H
