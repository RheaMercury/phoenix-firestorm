/**
 * @file fsssrbuffer.cpp
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

#include "llviewerprecompiledheaders.h"

#include "fsssrbuffer.h"

#include "fsssrhiz.h"
#include "lldrawpoolalpha.h"
#include "llframetimer.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llshadermgr.h"
#include "llvertexbuffer.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "pipeline.h"

#include <vector>

extern bool gCubeSnapshot;

namespace
{
    LLGLSLShader sDownsampleProgram; // blur mips of the resolved buffer (ssrDownsampleF.glsl)

    const LLStaticHashedString sFullRes("fs_ssr_full_res");
    const LLStaticHashedString sRes("fs_ssr_res");
    const LLStaticHashedString sJitter("ssrJitter");
    const LLStaticHashedString sHistoryBlend("ssrHistoryBlend");
    const LLStaticHashedString sMipScale("ssrMipScale");
    const LLStaticHashedString sTexelStep("texelStep");
    const LLStaticHashedString sKarisWeight("karisWeight");

    // RGBA16F texture with a full mip chain and a framebuffer per level, so each level can be
    // rendered from the one above it
    struct MipTexture
    {
        U32 mTexture = 0;
        std::vector<U32> mFBOs;
        U32 mWidth = 0;
        U32 mHeight = 0;
        U32 mLevels = 0;

        void release()
        {
            if (!mFBOs.empty())
            {
                glDeleteFramebuffers((GLsizei)mFBOs.size(), mFBOs.data());
                mFBOs.clear();
            }
            if (mTexture)
            {
                for (U32 i = 0; i < LL_NUM_TEXTURE_LAYERS; ++i)
                {
                    LLTexUnit* unit = gGL.getTexUnit(i);
                    if (unit && unit->getCurrTexture() == mTexture)
                    {
                        unit->unbind(LLTexUnit::TT_TEXTURE);
                    }
                }
                glDeleteTextures(1, &mTexture);
                mTexture = 0;
            }
            mWidth = 0;
            mHeight = 0;
            mLevels = 0;
        }

        bool matches(U32 width, U32 height) const
        {
            return mTexture && mWidth == width && mHeight == height;
        }

        bool allocate(U32 width, U32 height)
        {
            release();

            U32 levels = 1;
            while ((llmax(width, height) >> levels) > 0)
            {
                ++levels;
            }

            glGenTextures(1, &mTexture);
            if (!mTexture)
            {
                return false;
            }

            LLTexUnit* unit = gGL.getTexUnit(0);
            unit->bindManual(LLTexUnit::TT_TEXTURE, mTexture, true);
            clear_glerror();
            for (U32 level = 0; level < levels; ++level)
            {
                glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA16F, llmax(width >> level, 1U), llmax(height >> level, 1U), 0, GL_RGBA,
                             GL_HALF_FLOAT, nullptr);
            }
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            bool ok = glGetError() == GL_NO_ERROR;
            unit->unbind(LLTexUnit::TT_TEXTURE);

            mFBOs.resize(levels, 0);
            glGenFramebuffers((GLsizei)levels, mFBOs.data());
            for (U32 level = 0; ok && level < levels; ++level)
            {
                glBindFramebuffer(GL_FRAMEBUFFER, mFBOs[level]);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mTexture, level);
                ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
            }
            glBindFramebuffer(GL_FRAMEBUFFER, LLRenderTarget::sCurFBO);

            if (!ok)
            {
                LL_WARNS("Render") << "SSR buffer " << width << "x" << height << " could not be allocated" << LL_ENDL;
                release();
                return false;
            }

            mWidth = width;
            mHeight = height;
            mLevels = levels;
            return true;
        }

        void drawLevel(U32 level, LLVertexBuffer* screen_triangle)
        {
            glBindFramebuffer(GL_FRAMEBUFFER, mFBOs[level]);
            glViewport(0, 0, llmax(mWidth >> level, 1U), llmax(mHeight >> level, 1U));
            screen_triangle->setBuffer();
            screen_triangle->drawArrays(LLRender::TRIANGLES, 0, 3);
        }
    };

    LLRenderTarget sTrace;      // this frame's trace
    MipTexture sResolved[2];    // resolved results, alternating: the latest is history next frame
    U32 sIndex = 0;             // the latest resolved
    U32 sFrame = 0;             // frame it was resolved in, 0 none
    U32 sBuiltLevels = 0;       // its levels that hold blur mips

    void restoreTarget()
    {
        glBindFramebuffer(GL_FRAMEBUFFER, LLRenderTarget::sCurFBO);
        if (LLRenderTarget::sCurFBO)
        {
            glViewport(0, 0, LLRenderTarget::sCurResX, LLRenderTarget::sCurResY);
        }
        else
        {
            glViewport(gGLViewport[0], gGLViewport[1], gGLViewport[2], gGLViewport[3]);
        }
    }

    bool coneTrace()
    {
        // The hi-Z cone trace blurs at the hit already; the buffer is then read at mip 0
        static LLCachedControl<bool> cone_trace(gSavedSettings, "FSRenderSSRConeTrace", false);
        return cone_trace && FSSSRHiZ::enabled();
    }
}

void FSSSRBuffer::loadShaders(S32 shader_level)
{
    sDownsampleProgram.unload();
    if (!gSavedSettings.getBOOL("RenderScreenSpaceReflections"))
    {
        release();
        return;
    }
    sDownsampleProgram.mName = "SSR Downsample";
    sDownsampleProgram.mShaderFiles.clear();
    sDownsampleProgram.clearPermutations();
    sDownsampleProgram.mShaderFiles.push_back(std::make_pair("deferred/postDeferredNoTCV.glsl", GL_VERTEX_SHADER));
    sDownsampleProgram.mShaderFiles.push_back(std::make_pair("interface/ssrDownsampleF.glsl", GL_FRAGMENT_SHADER));
    sDownsampleProgram.mShaderLevel = shader_level;
    if (!sDownsampleProgram.createShader())
    {
        LL_WARNS("ShaderLoading") << "SSR Downsample failed to build, screen-space reflections trace per pixel" << LL_ENDL;
        sDownsampleProgram.unload();
    }
}

bool FSSSRBuffer::render(LLRenderTarget* screen)
{
    if (!gScreenSpaceReflTraceProgram.isComplete() || !gSSRResolveProgram.isComplete() || !sDownsampleProgram.isComplete() ||
        !gDeferredSoftenProgram.mDefines.count("FS_SSR_BUFFER"))
    {
        if (sTrace.isComplete() || sResolved[0].mTexture || sResolved[1].mTexture)
        {
            release();
        }
        return false;
    }
    if (gCubeSnapshot || !screen)
    {
        return false;
    }

    static LLCachedControl<F32> res_mult(gSavedSettings, "RenderScreenSpaceReflectionResolutionMultiplier", 1.f);
    static LLCachedControl<bool> temporal(gSavedSettings, "RenderScreenSpaceReflectionTemporal", true);
    static LLCachedControl<F32> temporal_blend(gSavedSettings, "RenderScreenSpaceReflectionTemporalBlend", 0.9f);
    static LLCachedControl<F32> filter_scale(gSavedSettings, "RenderScreenSpaceReflectionsFilterScale", 2.f);
    static LLCachedControl<F32> max_roughness(gSavedSettings, "RenderScreenSpaceReflectionMaxRoughness", 0.7f);

    const F32 mult = llclamp((F32)res_mult, 0.25f, 1.f);
    const U32 full_x = screen->getWidth();
    const U32 full_y = screen->getHeight();
    const U32 res_x = llmax(1U, (U32)ll_round((F32)full_x * mult));
    const U32 res_y = llmax(1U, (U32)ll_round((F32)full_y * mult));

    const U32 frame = LLFrameTimer::getFrameCount();
    // A gap in the frames (teleport, login, minimised) or a new size leaves no usable history
    bool history = temporal && sFrame != 0 && sFrame + 1 == frame && sResolved[0].matches(res_x, res_y) &&
                   sResolved[1].matches(res_x, res_y);

    if (!sTrace.isComplete() || sTrace.getWidth() != res_x || sTrace.getHeight() != res_y)
    {
        sTrace.release();
        if (!sTrace.allocate(res_x, res_y, GL_RGBA16F))
        {
            release();
            return false;
        }
    }
    for (MipTexture& resolved : sResolved)
    {
        if (!resolved.matches(res_x, res_y) && !resolved.allocate(res_x, res_y))
        {
            release();
            return false;
        }
    }

    LLVertexBuffer* triangle = gPipeline.mScreenTriangleVB;
    LLGLDisable blend(GL_BLEND);
    LLGLDepthTest depth(GL_FALSE, GL_FALSE);

    // 1. Trace
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("renderDeferredLighting - ssr trace");
        LL_PROFILE_GPU_ZONE("ssr trace");
        LLGLSLShader& shader = gScreenSpaceReflTraceProgram;
        sTrace.bindTarget();
        gPipeline.bindDeferredShader(shader); // the g-buffer, the scene map and hi-Z, the SSR settings
        shader.uniform2f(sFullRes, (GLfloat)full_x, (GLfloat)full_y);
        shader.uniform2f(sRes, (GLfloat)res_x, (GLfloat)res_y);
        // the march works in full-resolution pixels; bindDeferredShader set screen_res to the g-buffer
        shader.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (GLfloat)full_x, (GLfloat)full_y);
        shader.uniform4fv(LLShaderMgr::WATER_WATERPLANE, 1, LLDrawPoolAlpha::sWaterPlane.mV); // wet shoreline
        // New noise every frame only while the frames are blended; a still pattern otherwise
        shader.uniform1f(LLShaderMgr::DEFERRED_SSR_NOISE_SINE, temporal ? (GLfloat)(frame % 1024) : 0.f);
        shader.uniform1f(sJitter, temporal ? 1.f : 0.f);
        triangle->setBuffer();
        triangle->drawArrays(LLRender::TRIANGLES, 0, 3);
        gPipeline.unbindDeferredShader(shader);
        sTrace.flush();
    }

    const U32 next = 1 - sIndex;
    MipTexture& dst = sResolved[next];
    MipTexture& prev = sResolved[sIndex];

    // 2. Resolve with the last frame's result
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("renderDeferredLighting - ssr resolve");
        LL_PROFILE_GPU_ZONE("ssr resolve");
        LLGLSLShader& shader = gSSRResolveProgram;
        gPipeline.bindDeferredShader(shader); // depth, g-buffer, inv_proj, the camera motion
        shader.uniform2f(sFullRes, (GLfloat)full_x, (GLfloat)full_y);
        shader.uniform2f(sRes, (GLfloat)res_x, (GLfloat)res_y);
        shader.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (GLfloat)full_x, (GLfloat)full_y);
        shader.bindTexture(LLShaderMgr::FS_SSR_TRACE_MAP, &sTrace, false, LLTexUnit::TFO_POINT);
        S32 channel = shader.enableTexture(LLShaderMgr::FS_SSR_HISTORY_MAP);
        if (channel > -1)
        {
            gGL.getTexUnit(channel)->bindManual(LLTexUnit::TT_TEXTURE, prev.mTexture, true);
        }
        shader.uniform1f(sHistoryBlend, history ? llclamp((F32)temporal_blend, 0.5f, 0.98f) : 0.f);
        dst.drawLevel(0, triangle);
        gPipeline.unbindDeferredShader(shader);
    }

    // 3. Blur mips. Compositing fades SSR out at the maximum roughness, so the levels above the
    // one it reads there are never sampled.
    U32 built = 1;
    if (!coneTrace())
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("renderDeferredLighting - ssr mips");
        LL_PROFILE_GPU_ZONE("ssr mips");
        const F32 max_r = llclamp((F32)max_roughness, 0.f, 1.f);
        built = llmin(dst.mLevels, (U32)ceilf(max_r * (F32)(dst.mLevels - 1)) + 2);

        sDownsampleProgram.bind();
        sDownsampleProgram.uniform1i(sKarisWeight, 0); // the trace is luminance compressed already
        S32 channel = sDownsampleProgram.enableTexture(LLShaderMgr::DEFERRED_DIFFUSE);
        if (channel > -1)
        {
            LLTexUnit* unit = gGL.getTexUnit(channel);
            unit->bindManual(LLTexUnit::TT_TEXTURE, dst.mTexture, true);
            const F32 scale = llclamp((F32)filter_scale, 0.5f, 4.f);
            for (U32 level = 1; level < built; ++level)
            {
                // Only the source level is visible to the sampler while the next is rendered
                unit->activate();
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, level - 1);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, level - 1);
                const F32 src_w = (F32)llmax(dst.mWidth >> (level - 1), 1U);
                const F32 src_h = (F32)llmax(dst.mHeight >> (level - 1), 1U);
                sDownsampleProgram.uniform2f(sTexelStep, scale / src_w, scale / src_h);
                dst.drawLevel(level, triangle);
            }
            unit->unbind(LLTexUnit::TT_TEXTURE);
        }
        sDownsampleProgram.unbind();
    }

    // Sampled from mip 0 to the last level built
    {
        LLTexUnit* unit = gGL.getTexUnit(0);
        unit->bindManual(LLTexUnit::TT_TEXTURE, dst.mTexture, true);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, built - 1);
        unit->unbind(LLTexUnit::TT_TEXTURE);
    }

    restoreTarget();

    sIndex = next;
    sFrame = frame;
    sBuiltLevels = built;
    return true;
}

void FSSSRBuffer::bind(LLGLSLShader& soften, LLRenderTarget* screen, bool rendered)
{
    const S32 channel = soften.getTextureChannel(LLShaderMgr::FS_SSR_MAP);
    if (channel < 0)
    {
        return;
    }
    if (screen)
    {
        soften.uniform2f(sFullRes, (GLfloat)screen->getWidth(), (GLfloat)screen->getHeight());
    }
    const MipTexture& resolved = sResolved[sIndex];
    if (rendered && resolved.mTexture)
    {
        soften.enableTexture(LLShaderMgr::FS_SSR_MAP);
        gGL.getTexUnit(channel)->bindManual(LLTexUnit::TT_TEXTURE, resolved.mTexture, true);
        soften.uniform1f(sMipScale, coneTrace() ? 0.f : (F32)(resolved.mLevels - 1));
    }
    else
    {
        // probe captures: the upsample returns early there, but the sampler still needs a 2D texture
        gGL.getTexUnit(channel)->bindFast(LLViewerFetchedTexture::sBlackImagep);
        soften.uniform1f(sMipScale, 0.f);
    }
}

void FSSSRBuffer::release()
{
    sTrace.release();
    sResolved[0].release();
    sResolved[1].release();
    sIndex = 0;
    sFrame = 0;
    sBuiltLevels = 0;
}
