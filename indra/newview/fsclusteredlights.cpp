/**
 * @file fsclusteredlights.cpp
 * @brief Clustered shading of the deferred local point lights, and their glow in fog
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

#include "llviewerprecompiledheaders.h"

#include "fsclusteredlights.h"

#include "lldrawpoolalpha.h"
#include "llenvironment.h"
#include "llgl.h"
#include "llglheaders.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llvertexbuffer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "llworld.h"
#include "pipeline.h"

#include "glm/gtc/type_ptr.hpp"

#include <algorithm>
#include <cmath>

extern U32 gFrameCount;
extern bool gSnapshotNoPost;

FSClusteredLights gFSClusteredLights;
LLGLSLShader gFSClusteredLightProgram;
LLGLSLShader gFSPointLightScatterProgram;

namespace
{
    // Depth slices start here: everything nearer is slice 0. Spaced evenly in log depth out to
    // the farthest light, so near slices are thin and far ones deep, as lights on screen are.
    constexpr F32 SLICE_NEAR = 1.f;
    // Relative slack on every depth bound, so a pixel the GPU puts on a slice boundary finds its
    // light on either side of it
    constexpr F32 DEPTH_SLACK = 1e-3f;
    // A cluster lists at most this many lights, the nearest first (a light's index is its rank in
    // the stock loop, which runs nearest first). Only reached when RenderLocalLightCount is raised
    // far past its default of 256.
    constexpr U32 MAX_PER_CLUSTER = 1024;
    // Lights past this many are left to the stock passes (RenderLocalLightCount caps the loop
    // long before)
    constexpr U32 MAX_LIGHTS = 65535;

    LLStaticHashedString s_lights("fs_cl_lights");
    LLStaticHashedString s_lists("fs_cl_lists");
    LLStaticHashedString s_dims("fs_cl_dims");
    LLStaticHashedString s_scale("fs_cl_scale");
    LLStaticHashedString s_debug("fs_cl_debug");
    LLStaticHashedString s_scatter_max("fs_scatter_max");
    LLStaticHashedString s_scatter_strength("fs_scatter_strength");
    LLStaticHashedString s_scatter_density("fs_scatter_density");
    LLStaticHashedString s_anisotropy("vol_anisotropy");
    LLStaticHashedString s_air_fog("fs_air_fog");
    LLStaticHashedString s_air_plane("fs_air_plane");
    LLStaticHashedString s_pl_shadow("fs_pl_shadow");
    LLStaticHashedString s_pl_shadow_proj("fs_pl_shadow_proj");

    // Point light shadows (FSRenderPointLightShadows): the shader marches towards the first
    // FSRenderPointLightShadowCount lights of the buffer only
    constexpr U32 MAX_SHADOWED = 8;
}

// Moves the lights that weigh most on screen to the front of the list: brightness times the share
// of the view the light's sphere takes (its size over its distance from the camera, squared,
// capped where the camera is inside it). Only called before binning, so the lists and the glow
// follow the new order. Returns how many lights lead the list.
U32 FSClusteredLights::sortShadowedLights(U32 wanted)
{
    const U32 count = (U32)(mLights.size() / 2);
    wanted = llmin(wanted, count);
    if (wanted == 0)
    {
        return 0;
    }

    std::vector<std::pair<F32, U32>> score(count);
    for (U32 i = 0; i < count; ++i)
    {
        const LLVector4& c = mLights[i * 2];
        const LLVector4& col = mLights[i * 2 + 1];
        const F32 size2 = c.mV[VW] * c.mV[VW];
        const F32 d2 = c.mV[VX] * c.mV[VX] + c.mV[VY] * c.mV[VY] + c.mV[VZ] * c.mV[VZ];
        const F32 lum = 0.2126f * col.mV[VX] + 0.7152f * col.mV[VY] + 0.0722f * col.mV[VZ];
        score[i] = { -lum * size2 / llmax(d2, size2, 0.01f), i }; // negated: the strongest first
    }
    std::partial_sort(score.begin(), score.begin() + wanted, score.end());

    // stable for the rest: the others keep their nearest-first order
    std::vector<U8> taken(count, 0);
    std::vector<LLVector4> sorted;
    sorted.reserve(mLights.size());
    for (U32 i = 0; i < wanted; ++i)
    {
        const U32 idx = score[i].second;
        taken[idx] = 1;
        sorted.push_back(mLights[idx * 2]);
        sorted.push_back(mLights[idx * 2 + 1]);
    }
    for (U32 i = 0; i < count; ++i)
    {
        if (!taken[i])
        {
            sorted.push_back(mLights[i * 2]);
            sorted.push_back(mLights[i * 2 + 1]);
        }
    }
    mLights.swap(sorted);
    return wanted;
}

bool FSClusteredLights::begin(const glm::mat4& modelview, const glm::mat4& projection, U32 width, U32 height, bool probe)
{
    static LLCachedControl<bool> enabled(gSavedSettings, "FSRenderClusteredLights", false);

    mLights.clear();
    mFarDepth = SLICE_NEAR;
    mActive = false;
    mShade = false;
    mBinned = false;
    mTileLists = false;
    mProbe = probe;
    if (probe)
    {   // a probe face's lights are no main view frame's: prepareScatter must not take them
        mFrame = U32_MAX;
    }

    const bool scatter = !probe && scatterWanted();
    if ((!enabled || !gFSClusteredLightProgram.isComplete()) && !scatter)
    {
        return false;
    }

    if (mMaxTexels < 0)
    {
        GLint texels = 0;
        glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &texels);
        mMaxTexels = llmax(texels, 0);
        LL_INFOS("FSClusteredLights") << "Buffer textures hold up to " << mMaxTexels << " texels" << LL_ENDL;
    }
    mWidth = llmax(width, 1u);
    mHeight = llmax(height, 1u);

    if (!mLightBuffer)
    {
        glGenBuffers(1, &mLightBuffer);
        glGenBuffers(1, &mListBuffer);
        glGenTextures(1, &mLightTexture);
        glGenTextures(1, &mListTexture);
    }
    if (!mLightBuffer || !mListBuffer || !mLightTexture || !mListTexture || mMaxTexels < 65536)
    {
        return false;
    }

    // Lights handed over for shading must be drawn, so everything that could stop flush() from
    // drawing them is checked here, before the loop: room in the buffer texture for the grid's
    // header (clusters and tile lists) and a useful number of list entries, and two texture
    // units past the program's own samplers
    bool shade = enabled && gFSClusteredLightProgram.isComplete();
    const U64 header = (U64)((mWidth + TILE_PIXELS - 1) / TILE_PIXELS) * ((mHeight + TILE_PIXELS - 1) / TILE_PIXELS) * (DEPTH_SLICES + 1) * 2;
    if ((U64)mMaxTexels < header + 65536)
    {
        shade = false;
    }
    const S32 units = llmin(gGLManager.mNumTextureImageUnits, (S32)LL_NUM_TEXTURE_LAYERS);
    if (shade && gFSClusteredLightProgram.mActiveTextureChannels + 2 > units)
    {
        if (!mWarned)
        {
            mWarned = true;
            LL_WARNS("FSClusteredLights") << gFSClusteredLightProgram.mName << " uses " << gFSClusteredLightProgram.mActiveTextureChannels
                                          << " of " << units << " texture units, no room for the light lists" << LL_ENDL;
        }
        shade = false;
    }
    if (!shade && !scatter)
    {
        return false;
    }

    mModelview = modelview;
    mProjection = projection;
    mShade = shade;
    mActive = true;
    mFrame = probe ? U32_MAX : LLFrameTimer::getFrameCount();
    return true;
}

bool FSClusteredLights::add(const F32* center_agent, F32 size, const LLColor3& linear_color, F32 falloff)
{
    if (!mActive || mLights.size() >= (size_t)MAX_LIGHTS * 2)
    {
        return false;
    }
    // Eye space, as the stock passes transform their light centres (mul_mat4_vec3 of the modelview)
    const glm::vec3 c = mul_mat4_vec3(mModelview, glm::vec3(center_agent[0], center_agent[1], center_agent[2]));
    mLights.emplace_back(c.x, c.y, c.z, size);
    mLights.emplace_back(linear_color.mV[0], linear_color.mV[1], linear_color.mV[2], falloff);
    mFarDepth = llmax(mFarDepth, -c.z + size);
    return mShade;
}

// Sorts the lights into the grid: mLists gets a header of (offset, count) per cluster (and with
// tile_lists, per screen tile after them), then the lists of light indices, each list nearest
// light first.
void FSClusteredLights::binLights(bool tile_lists)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;

    const U32 light_count = (U32)(mLights.size() / 2);
    mTilesX = (mWidth + TILE_PIXELS - 1) / TILE_PIXELS;
    mTilesY = (mHeight + TILE_PIXELS - 1) / TILE_PIXELS;
    mSlices = DEPTH_SLICES;
    mLogNear = logf(SLICE_NEAR);
    const F32 log_far = logf(llmax(mFarDepth, SLICE_NEAR * 2.f));
    mSliceScale = (F32)mSlices / (log_far - mLogNear);

    const F32 tiles_per_x = (F32)mWidth / (F32)TILE_PIXELS;  // tiles per unit of screen coordinate
    const F32 tiles_per_y = (F32)mHeight / (F32)TILE_PIXELS;
    const F32 pixel_x = 1.f / (F32)TILE_PIXELS;               // one pixel, in tiles
    const S32 last_x = (S32)mTilesX - 1;
    const S32 last_y = (S32)mTilesY - 1;

    // The slice of a depth, as fsClusteredLightF.glsl finds it
    auto slice_of = [&](F32 depth) -> U32
    {
        if (depth <= SLICE_NEAR)
        {
            return 0;
        }
        const F32 s = floorf((logf(depth) - mLogNear) * mSliceScale);
        return (U32)llclamp(s, 0.f, (F32)(mSlices - 1));
    };
    auto slice_near = [&](U32 slice) -> F32
    {
        return slice == 0 ? 0.f : expf(mLogNear + (F32)slice / mSliceScale);
    };
    auto slice_far = [&](U32 slice) -> F32
    {
        return slice + 1 >= mSlices ? F32_MAX : expf(mLogNear + (F32)(slice + 1) / mSliceScale);
    };

    // The screen tiles a view space box covers, conservatively. False when it is off screen.
    auto box_tiles = [&](F32 x0, F32 x1, F32 y0, F32 y1, F32 d0, F32 d1, Span& span) -> bool
    {
        F32 min_x = F32_MAX, min_y = F32_MAX, max_x = -F32_MAX, max_y = -F32_MAX;
        bool whole = false;
        for (U32 corner = 0; corner < 8; ++corner)
        {
            const glm::vec4 p = mProjection * glm::vec4((corner & 1) ? x1 : x0, (corner & 2) ? y1 : y0, -((corner & 4) ? d1 : d0), 1.f);
            if (p.w < 1e-4f)
            {
                whole = true; // reaches the camera plane: any pixel may look into it
                break;
            }
            const F32 sx = p.x / p.w * 0.5f + 0.5f;
            const F32 sy = p.y / p.w * 0.5f + 0.5f;
            min_x = llmin(min_x, sx);
            max_x = llmax(max_x, sx);
            min_y = llmin(min_y, sy);
            max_y = llmax(max_y, sy);
        }
        if (whole)
        {
            span.mX0 = 0;
            span.mY0 = 0;
            span.mX1 = (U32)last_x;
            span.mY1 = (U32)last_y;
            return true;
        }
        if (max_x < 0.f || max_y < 0.f || min_x > 1.f || min_y > 1.f)
        {
            return false;
        }
        span.mX0 = (U32)llclamp((S32)floorf(min_x * tiles_per_x - pixel_x), 0, last_x);
        span.mX1 = (U32)llclamp((S32)floorf(max_x * tiles_per_x + pixel_x), 0, last_x);
        span.mY0 = (U32)llclamp((S32)floorf(min_y * tiles_per_y - pixel_x), 0, last_y);
        span.mY1 = (U32)llclamp((S32)floorf(max_y * tiles_per_y + pixel_x), 0, last_y);
        return true;
    };

    // Per light and depth slice its sphere reaches: the box around the part of the sphere inside
    // the slice (its widest cross section there), and the tiles that box covers
    mSpans.clear();
    for (U32 i = 0; i < light_count; ++i)
    {
        const LLVector4& l = mLights[i * 2];
        const F32 s = l.mV[3];
        const F32 dc = -l.mV[2];
        const F32 dmin = dc - s;
        const F32 dmax = dc + s;
        if (dmax <= 0.f)
        {
            continue; // behind the camera
        }
        const U32 k0 = slice_of(llmax(dmin, 0.f) * (1.f - DEPTH_SLACK));
        const U32 k1 = slice_of(dmax * (1.f + DEPTH_SLACK));
        for (U32 k = k0; k <= k1; ++k)
        {
            const F32 a = llmax(slice_near(k) * (1.f - DEPTH_SLACK), dmin);
            const F32 b = llmin(slice_far(k) * (1.f + DEPTH_SLACK), dmax);
            if (a > b)
            {
                continue;
            }
            const F32 dz = llclamp(dc, a, b) - dc;
            const F32 rs = sqrtf(llmax(s * s - dz * dz, 0.f)) * (1.f + DEPTH_SLACK) + 1e-3f;
            Span span;
            span.mLight = i;
            span.mSlice = k;
            if (box_tiles(l.mV[0] - rs, l.mV[0] + rs, l.mV[1] - rs, l.mV[1] + rs, a, b, span))
            {
                mSpans.push_back(span);
            }
        }
        // The glow's tile list: every tile the whole sphere covers, any depth
        if (tile_lists)
        {
            const F32 rs = s * (1.f + DEPTH_SLACK) + 1e-3f;
            Span span;
            span.mLight = i;
            span.mSlice = mSlices;
            if (box_tiles(l.mV[0] - rs, l.mV[0] + rs, l.mV[1] - rs, l.mV[1] + rs, dmin, dmax, span))
            {
                mSpans.push_back(span);
            }
        }
    }

    // Count, cap, lay out the lists, fill them. Spans run light by light, so each list comes out
    // in light order, nearest first.
    const U32 entries = mTilesX * mTilesY * (mSlices + (tile_lists ? 1 : 0));
    mTileHeader = mTilesX * mTilesY * mSlices;
    mCounts.assign(entries, 0);
    for (const Span& span : mSpans)
    {
        for (U32 y = span.mY0; y <= span.mY1; ++y)
        {
            U32 idx = (span.mSlice * mTilesY + y) * mTilesX + span.mX0;
            for (U32 x = span.mX0; x <= span.mX1; ++x)
            {
                ++mCounts[idx++];
            }
        }
    }

    U32 cap = MAX_PER_CLUSTER;
    U64 total = 0;
    for (U32 count : mCounts)
    {
        total += llmin(count, cap);
    }
    const U64 room = (U64)mMaxTexels - (U64)entries * 2;
    if (total > room)
    {
        // Never at sane settings: keep the nearest lights of each cluster
        cap = llmax((U32)(room / entries), 1u);
        total = 0;
        for (U32 count : mCounts)
        {
            total += llmin(count, cap);
        }
        if (!mWarned)
        {
            mWarned = true;
            LL_WARNS("FSClusteredLights") << "Light lists overflow the buffer texture, each cluster keeps its nearest "
                                          << cap << " lights" << LL_ENDL;
        }
    }

    mLists.resize((size_t)entries * 2 + (size_t)total);
    U32 offset = entries * 2;
    for (U32 i = 0; i < entries; ++i)
    {
        const U32 count = llmin(mCounts[i], cap);
        mLists[i * 2] = offset;
        mLists[i * 2 + 1] = count;
        offset += count;
    }
    mFill.assign(entries, 0);
    for (const Span& span : mSpans)
    {
        for (U32 y = span.mY0; y <= span.mY1; ++y)
        {
            U32 idx = (span.mSlice * mTilesY + y) * mTilesX + span.mX0;
            for (U32 x = span.mX0; x <= span.mX1; ++x, ++idx)
            {
                U32& fill = mFill[idx];
                if (fill < mLists[idx * 2 + 1])
                {
                    mLists[mLists[idx * 2] + fill++] = span.mLight;
                }
            }
        }
    }

    if (gFrameCount % 300 == 0)
    {
        LL_DEBUGS("FSClusteredLights") << light_count << " lights, " << mTilesX << "x" << mTilesY << "x" << mSlices
                                       << " clusters, " << total << " list entries" << LL_ENDL;
    }
}

void FSClusteredLights::upload()
{
    glBindBuffer(GL_TEXTURE_BUFFER, mLightBuffer);
    glBufferData(GL_TEXTURE_BUFFER, (GLsizeiptr)(mLights.size() * sizeof(LLVector4)), mLights.data(), GL_STREAM_DRAW);
    glBindBuffer(GL_TEXTURE_BUFFER, mListBuffer);
    glBufferData(GL_TEXTURE_BUFFER, (GLsizeiptr)(mLists.size() * sizeof(U32)), mLists.data(), GL_STREAM_DRAW);
    glBindBuffer(GL_TEXTURE_BUFFER, 0);

    // Attached again after every upload: the stores were just replaced. Buffer textures have a
    // target of their own, so this leaves the unit's 2D binding that LLTexUnit tracks alone.
    glBindTexture(GL_TEXTURE_BUFFER, mLightTexture);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, mLightBuffer);
    glBindTexture(GL_TEXTURE_BUFFER, mListTexture);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, mListBuffer);
    glBindTexture(GL_TEXTURE_BUFFER, 0);
}

void FSClusteredLights::bindBuffers(LLGLSLShader& shader)
{
    // Buffer samplers are not given units when the program is linked, so they take the first
    // two past the program's own (begin() checked there are two)
    const S32 base = shader.mActiveTextureChannels;
    mUnits[0] = base;
    mUnits[1] = base + 1;
    gGL.getTexUnit(mUnits[0])->activate();
    glBindTexture(GL_TEXTURE_BUFFER, mLightTexture);
    gGL.getTexUnit(mUnits[1])->activate();
    glBindTexture(GL_TEXTURE_BUFFER, mListTexture);
    gGL.getTexUnit(0)->activate();

    shader.uniform1i(s_lights, mUnits[0]);
    shader.uniform1i(s_lists, mUnits[1]);
    const GLint dims[4] = { (GLint)mTilesX, (GLint)mTilesY, (GLint)mSlices, (GLint)mTileHeader };
    shader.uniform4iv(s_dims, 1, dims);
    shader.uniform4f(s_scale, (F32)mWidth / (F32)TILE_PIXELS, (F32)mHeight / (F32)TILE_PIXELS, mSliceScale, mLogNear);
}

void FSClusteredLights::unbindBuffers()
{
    for (S32& unit : mUnits)
    {
        if (unit >= 0)
        {
            gGL.getTexUnit(unit)->activate();
            glBindTexture(GL_TEXTURE_BUFFER, 0);
            unit = -1;
        }
    }
    gGL.getTexUnit(0)->activate();
}

void FSClusteredLights::flush(LLVertexBuffer* screen_tri)
{
    if (!mActive)
    {
        return;
    }
    mActive = false;
    if (mLights.empty() || !mShade || !screen_tri)
    {
        return; // without shading, kept for the glow
    }

    LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("renderDeferredLighting - clustered lights");
    LL_PROFILE_GPU_ZONE("clustered lights");

    // Point light shadows: the main view only, never a probe face
    static LLCachedControl<bool> pl_shadows(gSavedSettings, "FSRenderPointLightShadows", false);
    static LLCachedControl<S32> pl_shadow_count(gSavedSettings, "FSRenderPointLightShadowCount", 4);
    static LLCachedControl<F32> pl_shadow_length(gSavedSettings, "FSRenderPointLightShadowLength", 1.f);
    static LLCachedControl<F32> pl_shadow_strength(gSavedSettings, "FSRenderPointLightShadowStrength", 0.8f);
    U32 shadowed = 0;
    if (pl_shadows && !mProbe && gFSClusteredLightProgram.mDefines.count("FS_POINT_SHADOWS"))
    {
        shadowed = sortShadowedLights((U32)llclamp((S32)pl_shadow_count, 1, (S32)MAX_SHADOWED));
    }

    // The glow's tile lists are binned with the clusters, while the lights are at hand (not for
    // a probe face, which has no glow)
    mTileLists = !mProbe && scatterWanted();
    binLights(mTileLists);
    upload();
    mBinned = true;

    static LLCachedControl<bool> debug(gSavedSettings, "FSRenderClusteredLightsDebug", false);

    LLGLSLShader& shader = gFSClusteredLightProgram;
    LLGLDepthTest depth(GL_FALSE);
    gPipeline.bindDeferredShader(shader);
    bindBuffers(shader);
    LLSettingsSky::ptr_t psky = LLEnvironment::instance().getCurrentSky();
    shader.uniform1i(LLShaderMgr::CLASSIC_MODE, (psky && psky->canAutoAdjust()) ? 1 : 0);
    shader.uniform1i(s_debug, debug ? 1 : 0);
    if (shader.mDefines.count("FS_POINT_SHADOWS"))
    {
        shader.uniform4f(s_pl_shadow, (F32)shadowed, llclamp((F32)pl_shadow_length, 0.1f, 4.f),
                         llclamp((F32)pl_shadow_strength, 0.f, 1.f), 0.f);
        shader.uniformMatrix4fv(s_pl_shadow_proj, 1, GL_FALSE, glm::value_ptr(mProjection));
    }
    screen_tri->setBuffer();
    screen_tri->drawArrays(LLRender::TRIANGLES, 0, 3);
    unbindBuffers();
    gPipeline.unbindDeferredShader(shader);
}

// static
bool FSClusteredLights::scatterWanted()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "FSRenderPointLightScatter", false);
    static LLCachedControl<F32> strength(gSavedSettings, "FSRenderPointLightScatterStrength", 1.f);
    return enabled && strength > 0.f && gFSPointLightScatterProgram.isComplete() && !gSnapshotNoPost &&
           !LLPipeline::sUnderWaterRender &&
           !(LLViewerCamera::getInstance()->cameraUnderWater() && LLWorld::getInstance()->getAllowRenderWater());
}

bool FSClusteredLights::prepareScatter(S32 max_lights)
{
    // Only this frame's lights: the light pass may not have collected any (local lights off,
    // a frame without the main view's light pass)
    if (mFrame != LLFrameTimer::getFrameCount() || mLights.empty() || max_lights <= 0)
    {
        return false;
    }
    if (mBinned)
    {
        return mTileLists; // the tile lists went up with the clusters (unless the glow was off then)
    }

    // Without clustered shading: one tile covering the screen, listing the nearest lights (the
    // stock loop runs nearest first)
    const U32 count = llmin((U32)(mLights.size() / 2), (U32)max_lights);
    mTilesX = 1;
    mTilesY = 1;
    mSlices = 0;
    mTileHeader = 0;
    mSliceScale = 1.f;
    mLogNear = 0.f;
    mLists.resize(2 + (size_t)count);
    mLists[0] = 2;
    mLists[1] = count;
    for (U32 i = 0; i < count; ++i)
    {
        mLists[2 + i] = i;
    }
    upload();
    mBinned = true;
    mTileLists = true;
    return true;
}

bool FSClusteredLights::drawScatter(LLRenderTarget& depth_source, LLVertexBuffer* screen_tri, F32 strength_scale, F32 anisotropy, S32 max_lights)
{
    static LLCachedControl<F32> density(gSavedSettings, "FSRenderPointLightScatterDensity", 0.02f);

    LLGLSLShader& shader = gFSPointLightScatterProgram;
    const S32 units = llmin(gGLManager.mNumTextureImageUnits, (S32)LL_NUM_TEXTURE_LAYERS);

    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    LL_PROFILE_GPU_ZONE("point light scatter");

    shader.bind();
    const bool drawn = shader.mActiveTextureChannels + 2 <= units;
    if (drawn)
    {
        shader.bindTexture(LLShaderMgr::DEFERRED_DEPTH, &depth_source, true);
        bindBuffers(shader);
        shader.uniform1i(s_scatter_max, llclamp(max_lights, 1, 64));
        shader.uniform1f(s_scatter_strength, strength_scale);
        shader.uniform1f(s_scatter_density, llclamp((F32)density, 0.f, 1.f));
        shader.uniform1f(s_anisotropy, llclamp(anisotropy, 0.f, 0.9f));
        setAirUniforms(shader, true);
        screen_tri->setBuffer();
        screen_tri->drawArrays(LLRender::TRIANGLES, 0, 3);
        unbindBuffers();
    }
    shader.unbind();
    return drawn;
}

// static
void FSClusteredLights::setAirUniforms(LLGLSLShader& shader, bool use_fog)
{
    // The height fog's values as fs_upload_height_fog_uniforms (llviewershadermgr.cpp) sends them
    static LLCachedControl<bool> fog_on(gSavedSettings, "FSRenderHeightFog", false);
    static LLCachedControl<F32> fog_density(gSavedSettings, "FSRenderHeightFogDensity", 0.003f);
    static LLCachedControl<F32> fog_falloff(gSavedSettings, "FSRenderHeightFogFalloff", 0.02f);
    static LLCachedControl<F32> fog_base(gSavedSettings, "FSRenderHeightFogBaseHeight", 0.f);
    static LLCachedControl<bool> fog_base_absolute(gSavedSettings, "FSRenderHeightFogBaseAbsolute", false);
    static LLCachedControl<bool> fog_sky_haze(gSavedSettings, "FSRenderHeightFogSkyHaze", true);

    // Only while the fog itself is drawn (its define is set once the shaders were rebuilt)
    if (!use_fog || !fog_on || !LLGLSLShader::sGlobalDefines.count("FS_HEIGHT_FOG"))
    {
        shader.uniform4f(s_air_fog, 0.f, 0.f, 0.f, 0.f);
        shader.uniform4f(s_air_plane, 0.f, 0.f, 1.f, 0.f);
        return;
    }

    LLEnvironment& environment = LLEnvironment::instance();
    F32 sigma = llclamp((F32)fog_density, 0.f, 0.1f);
    LLSettingsSky::ptr_t psky = environment.getCurrentSky();
    if (fog_sky_haze && psky)
    {
        sigma *= llclamp(sqrtf(llmax(psky->getHazeDensity(), 0.f) / 0.7f), 0.5f, 2.f);
    }
    F32 base = llclamp((F32)fog_base, -4096.f, 8192.f);
    if (fog_base_absolute)
    {
        base -= environment.getWaterHeight();
    }

    // Height above the fog's base of a view space point p: dot(p, plane.xyz) + plane.w, from the
    // water plane (view space, normalised) the haze pass gives the fog
    const LLVector4& water = LLDrawPoolAlpha::sWaterPlane;
    shader.uniform4f(s_air_fog, sigma, llclamp((F32)fog_falloff, 0.f, 1.f), 0.f, 1.f);
    shader.uniform4f(s_air_plane, water.mV[VX], water.mV[VY], water.mV[VZ], water.mV[VW] - base);
}

void FSClusteredLights::release()
{
    if (mLightTexture)
    {
        glDeleteTextures(1, &mLightTexture);
        mLightTexture = 0;
    }
    if (mListTexture)
    {
        glDeleteTextures(1, &mListTexture);
        mListTexture = 0;
    }
    if (mLightBuffer)
    {
        glDeleteBuffers(1, &mLightBuffer);
        mLightBuffer = 0;
    }
    if (mListBuffer)
    {
        glDeleteBuffers(1, &mListBuffer);
        mListBuffer = 0;
    }
    mLights.clear();
    mLights.shrink_to_fit();
    mSpans.clear();
    mSpans.shrink_to_fit();
    mCounts.clear();
    mCounts.shrink_to_fit();
    mFill.clear();
    mFill.shrink_to_fit();
    mLists.clear();
    mLists.shrink_to_fit();
    mActive = false;
    mShade = false;
    mBinned = false;
}
