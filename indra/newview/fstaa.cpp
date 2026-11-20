/**
 * @file fstaa.cpp
 * @brief Temporal anti-aliasing (FSRenderTAA), see fstaa.h.
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

#include "llviewerprecompiledheaders.h"

#include "fstaa.h"

#include "fsreversez.h"
#include "fstaajitter.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llimagegl.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llshadermgr.h"
#include "llvertexbuffer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerdisplay.h"
#include "pipeline.h"

#include <glm/gtc/type_ptr.hpp>

LLGLSLShader gFSTAAResolveProgram;
LLGLSLShader gFSTAASharpenProgram;
LLGLSLShader gFSTAAOverlayDepthProgram;
LLGLSLShader gFSTAADepthCopyProgram;

extern bool gSnapshot;
extern bool gShiftFrame;

namespace
{
    constexpr U32 JITTER_PHASES = 8;
    constexpr F32 CAMERA_CUT_METRES = 32.f;     // a camera move this long in one frame starts over

    bool sFrameActive = false;      // TAA runs this frame
    bool sHistoryValid = false;     // the history holds last frame's result for this view
    bool sCameraTaken = false;      // this frame's camera matrices are in sCurView/sCurProj
    F32 sCASSharpness = 0.f;        // the CAS sharpness for the frame the last resolve ran in
    U32 sPhase = 0;
    F32 sJitterX = 0.f;             // this frame's jitter, NDC
    F32 sJitterY = 0.f;
    U32 sWidth = 0;
    U32 sHeight = 0;

    glm::mat4 sCurView(1.f), sCurProj(1.f);
    glm::mat4 sPrevView(1.f), sPrevProj(1.f);
    LLVector3 sPrevCameraOrigin;

    // Heap allocated: a static LLRenderTarget would release GL objects after the context is gone
    LLRenderTarget* sHistory[2] = { nullptr, nullptr };
    U32 sHistoryIndex = 0;          // the one written last
    LLRenderTarget* sPreAlpha = nullptr;
    LLRenderTarget* sDebug = nullptr;   // FSRenderTAADebug's false colours, only while it is set
    U32 sPreAlphaFrame = 0;         // frame whose scene sPreAlpha holds
    U32 sPreAlphaDepthFrame = 0;    // frame whose depth before alpha the pipeline kept

    // Overlays drawn after the resolve (beginDeferOverlays)
    LLRenderTarget* sOverlayDepth = nullptr;    // depth only: the conservative depth
    bool sDeferring = false;        // inside the main view's alpha pass, holding them back
    U32 sOverlayFrame = 0;          // frame whose overlays wait for renderFinalize, 0 none
    U32 sDebugAlphaFrame = 0;       // frame whose "Highlight Transparent" waits with them
    bool sOverlayDepthSwapped = false;  // screen has the conservative depth attached
    U32 sOverlayDepthFrame = 0;     // frame sOverlayDepth was built for
    LLRenderTarget* sOverlayDepthSource = nullptr;  // the scene depth it was built from
    // The conservative depth of the depth the present writes (PresentedDepthScope): sOverlayDepth
    // when the present writes the same depth, else built into sPresentedDepthOwn
    LLRenderTarget* sPresentedDepthOwn = nullptr;
    LLRenderTarget* sPresentedDepth = nullptr;
    U32 sPresentedDepthFrame = 0;
    bool sBeaconsPending = false;   // debug beacons were added since render_ui_3d last drew them

    // Draws after the present (PresentedDepthScope): the framebuffer's depth, kept while the
    // conservative depth stands in for it
    U32 sSavedDepth = 0;            // GL texture
    U32 sSavedDepthFormat = 0;
    S32 sSavedDepthWidth = 0;
    S32 sSavedDepthHeight = 0;

    LLStaticHashedString sHistoryMap("fs_taa_history");
    LLStaticHashedString sJitter("fs_taa_jitter");
    LLStaticHashedString sInvProj("fs_taa_inv_proj");
    LLStaticHashedString sReproject("fs_taa_reproject");
    LLStaticHashedString sBlend("fs_taa_blend");
    LLStaticHashedString sReactive("fs_taa_reactive");
    LLStaticHashedString sReset("fs_taa_reset");
    LLStaticHashedString sPreAlphaValid("fs_taa_pre_alpha");
    LLStaticHashedString sSharpness("fs_taa_sharpness");
    LLStaticHashedString sJitterNDC("fs_taa_jitter_ndc");
    LLStaticHashedString sViewport("fs_taa_viewport");
    LLStaticHashedString sDebugView("fs_taa_debug");

    F32 halton(U32 index, U32 base)
    {
        F32 f = 1.f;
        F32 r = 0.f;
        while (index > 0)
        {
            f /= (F32)base;
            r += f * (F32)(index % base);
            index /= base;
        }
        return r;
    }

    // The mean of the jitter's 8 Halton offsets: the base 2 sequence from index 1 to 8 (1/2, 1/4,
    // 3/4, 1/8, 5/8, 3/8, 7/8, 1/16) averages 0.4453, not 0.5, so taking 0.5 off left the image
    // 0.055 pixel off centre on average, against everything drawn unjittered (the overlays, the
    // conservative depth). Taken off instead, the offsets average exactly 0 and stay within half
    // a pixel (-0.38 to +0.43).
    F32 halton_mean(U32 base)
    {
        F32 sum = 0.f;
        for (U32 i = 1; i <= JITTER_PHASES; ++i)
        {
            sum += halton(i, base);
        }
        return sum / (F32)JITTER_PHASES;
    }

    bool setting_on()
    {
        static LLCachedControl<bool> taa(gSavedSettings, "FSRenderTAA", false);
        return taa;
    }

    // Allocated at size and cleared (a new target's contents are undefined, on AMD they can hold
    // NaN, which the history would then carry on forever)
    bool ensure_target(LLRenderTarget*& target, U32 width, U32 height)
    {
        if (target && target->isComplete() && target->getWidth() == width && target->getHeight() == height)
        {
            return false;
        }
        if (!target)
        {
            target = new LLRenderTarget();
        }
        target->release();
        if (!target->allocate(width, height, GL_RGBA16F))
        {
            target->release();
            return true;
        }
        target->bindTarget();
        glClearColor(0.f, 0.f, 0.f, 0.f);
        target->clear(GL_COLOR_BUFFER_BIT);
        target->flush();
        return true;
    }

    void delete_target(LLRenderTarget*& target)
    {
        if (target)
        {
            target->release();
            delete target;
            target = nullptr;
        }
    }

    // The conservative depth for the overlays, at width x height, from the jittered scene depth:
    // see fsTAAOverlayDepthF.glsl. False when it can't be built (the overlays then test against
    // the scene's own depth).
    bool build_overlay_depth(LLRenderTarget*& target, LLRenderTarget& depth_source, U32 width, U32 height, LLVertexBuffer* screen_triangle)
    {
        if (!gFSTAAOverlayDepthProgram.isComplete() || !screen_triangle || !depth_source.getDepth() || !width || !height)
        {
            return false;
        }
        if (!target)
        {
            target = new LLRenderTarget();
        }
        // allocate() keeps the target when its size and depth format (reverse-Z) still match
        const U32 old_depth = target->getDepth();
        if (!target->allocate(width, height, 0, true) || !target->getDepth())
        {
            target->release();
            return false;
        }

        LL_PROFILE_GPU_ZONE("taa overlay depth");
        LLGLDepthTest depth(GL_TRUE, GL_TRUE, GL_ALWAYS);
        LLGLDisable blend(GL_BLEND);
        LLGLDisable cull(GL_CULL_FACE);
        target->bindTarget();
        if (target->getDepth() != old_depth)
        {
            // a new target's contents are undefined until written (every pixel is, below)
            target->clear(GL_DEPTH_BUFFER_BIT);
        }
        LLGLSLShader& shader = gFSTAAOverlayDepthProgram;
        shader.bind();
        shader.bindTexture(LLShaderMgr::DEFERRED_DEPTH, &depth_source, true);
        shader.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (F32)width, (F32)height);
        shader.uniform2f(sJitterNDC, sJitterX, sJitterY);
        screen_triangle->setBuffer();
        screen_triangle->drawArrays(LLRender::TRIANGLES, 0, 3);
        shader.unbind();
        target->flush();
        return true;
    }

    // The bound framebuffer's colour writes off (the depth passes below write depth only) or back on.
    // Draw buffers rather than the colour mask: LLRender's cached mask can't be read back.
    void set_color_writes(bool on)
    {
        LLRenderTarget* target = LLRenderTarget::getCurrentBoundTarget();
        if (!on)
        {
            glDrawBuffer(GL_NONE);
        }
        else if (!target)
        {
            glDrawBuffer(GL_BACK);  // as LLRenderTarget::flush leaves the default framebuffer
        }
        else
        {
            const GLenum buffers[] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3 };
            const U32 count = target->getNumTextures();
            if (count)
            {
                glDrawBuffers((GLsizei)llmin(count, (U32)4), buffers);
            }
            else
            {
                glDrawBuffer(GL_NONE);
            }
        }
    }

    // Writes depth_tex (stretched over gGLViewport, nearest texel) into the bound framebuffer's
    // depth with depth_func. Colour is left alone; the texture unit, shader and states are restored.
    void write_depth(U32 depth_tex, GLenum depth_func)
    {
        LLVertexBuffer* triangle = gPipeline.mScreenTriangleVB;
        LLGLSLShader& shader = gFSTAADepthCopyProgram;
        LLGLSLShader* prev_shader = LLGLSLShader::sCurBoundShaderPtr;

        gGL.flush();
        LLGLDepthTest depth(GL_TRUE, GL_TRUE, depth_func);
        LLGLDisable cull(GL_CULL_FACE);
        LLGLDisable blend(GL_BLEND);
        glViewport(gGLViewport[0], gGLViewport[1], gGLViewport[2], gGLViewport[3]);
        set_color_writes(false);

        shader.bind();
        const S32 channel = shader.getTextureChannel(LLShaderMgr::DEFERRED_DEPTH);
        LLTexUnit* unit = channel >= 0 ? gGL.getTexUnit(channel) : nullptr;
        const U32 prev_tex = unit ? unit->getCurrTexture() : 0;
        const LLTexUnit::eTextureType prev_type = unit ? unit->getCurrType() : LLTexUnit::TT_NONE;
        if (unit)
        {
            unit->bindManual(LLTexUnit::TT_TEXTURE, depth_tex);
        }
        shader.uniform4f(sViewport, (F32)gGLViewport[0], (F32)gGLViewport[1], (F32)gGLViewport[2], (F32)gGLViewport[3]);
        triangle->setBuffer();
        triangle->drawArrays(LLRender::TRIANGLES, 0, 3);
        if (unit)
        {
            if (prev_tex && prev_type != LLTexUnit::TT_NONE)
            {
                unit->bindManual(prev_type, prev_tex);
            }
            else
            {
                unit->unbind(LLTexUnit::TT_TEXTURE);
            }
        }
        shader.unbind();
        if (prev_shader)
        {
            prev_shader->bind();
        }
        set_color_writes(true);
        LLVertexBuffer::unbind();
    }

    // The jitter as a matrix, the way FSTAAJitter::apply adds it
    glm::dmat4 jittered(const glm::dmat4& proj, F64 x, F64 y)
    {
        glm::dmat4 j = proj;
        for (int c = 0; c < 4; ++c)
        {
            j[c][0] += x * proj[c][3];
            j[c][1] += y * proj[c][3];
        }
        return j;
    }
}

namespace FSTAA
{
    void beginFrame(const LLRenderTarget& screen)
    {
        sCameraTaken = false;
        if (!setting_on())
        {
            if (sFrameActive || sHistory[0] || sPreAlpha)
            {
                release();
            }
            sFrameActive = false;
            sHistoryValid = false;
            return;
        }

        static LLCachedControl<bool> has_hdr(gSavedSettings, "RenderHDREnabled", true);
        const bool hdr = gGLManager.mGLVersion > 4.05f && has_hdr();    // as renderFinalize decides
        sFrameActive = hdr && LLPipeline::sRenderDeferred && !gSnapshot && !LLPipeline::isAvatarOnlyRender() &&
                       gFSTAAResolveProgram.isComplete() && gFSTAASharpenProgram.isComplete() &&
                       screen.getWidth() > 0 && screen.getHeight() > 0;
        if (!sFrameActive)
        {
            // A frame without HDR, or an avatars only snapshot: what is drawn now is no history for the
            // next one. A snapshot leaves the history, last frame's camera and the jitter phase as they
            // were (it neither resolves nor jitters): the next frame goes on from them. The snapshot
            // floater's preview refreshes about once a second, and each one dropped the history.
            if (!gSnapshot || LLPipeline::isAvatarOnlyRender())
            {
                sHistoryValid = false;
            }
            return;
        }

        // Teleports, region crossings (the agent origin the matrices are relative to moves) and
        // camera cuts: last frame's picture is of somewhere else
        const LLVector3 origin = LLViewerCamera::getInstance()->getOrigin();
        if (gTeleportDisplay || gShiftFrame || dist_vec(origin, sPrevCameraOrigin) > CAMERA_CUT_METRES)
        {
            sHistoryValid = false;
        }
        sPrevCameraOrigin = origin;

        if (screen.getWidth() != sWidth || screen.getHeight() != sHeight)
        {
            sHistoryValid = false;
        }

        sPhase = (sPhase + 1) % JITTER_PHASES;
        const F32 w = (F32)screen.getWidth();
        const F32 h = (F32)screen.getHeight();
        // Halton from index 1: index 0 would be the corner of the pixel. In pixels, within half a
        // pixel, then in NDC (2 / width per pixel: NDC spans 2 over the width of the target the
        // scene is drawn into, which dynamic resolution sizes, so the jitter follows it)
        static const F32 mean_x = halton_mean(2);
        static const F32 mean_y = halton_mean(3);
        sJitterX = (halton(sPhase + 1, 2) - mean_x) * 2.f / w;
        sJitterY = (halton(sPhase + 1, 3) - mean_y) * 2.f / h;

        // the material shaders' texture LOD bias while the jitter is on
        static LLCachedControl<F32> lod_bias(gSavedSettings, "FSRenderTAATextureBias", -0.5f);
        FSTAAJitter::setLodBias(llclamp((F32)lod_bias, -1.5f, 0.f));
    }

    bool frameActive()
    {
        return sFrameActive;
    }

    JitterScope::JitterScope(bool main_view)
    : mOn(main_view && sFrameActive)
    {
        if (!mOn)
        {
            return;
        }
        if (!sCameraTaken)
        {
            // the C++ side's matrices stay unjittered: this is the camera as culling saw it
            sCurView = get_current_modelview();
            sCurProj = get_current_projection();
            sCameraTaken = true;
        }
        FSTAAJitter::set(sJitterX, sJitterY);
    }

    JitterScope::~JitterScope()
    {
        if (mOn)
        {
            FSTAAJitter::clear();
        }
    }

    void capturePreAlpha(LLRenderTarget& screen, bool main_view)
    {
        if (!main_view || !sFrameActive)
        {
            return;
        }
        const U32 width = screen.getWidth();
        const U32 height = screen.getHeight();
        LL_PROFILE_GPU_ZONE("taa pre alpha copy");
        screen.flush();
        ensure_target(sPreAlpha, width, height);
        if (sPreAlpha && sPreAlpha->isComplete())
        {
            sPreAlpha->copyContents(screen, 0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            sPreAlphaFrame = LLFrameTimer::getFrameCount();
        }
        screen.bindTarget();
    }

    bool wantsPreAlphaDepth()
    {
        return sFrameActive;
    }

    void notePreAlphaDepth()
    {
        if (sFrameActive)
        {
            sPreAlphaDepthFrame = LLFrameTimer::getFrameCount();
        }
    }

    bool resolve(LLRenderTarget& screen, LLRenderTarget& depth_source, LLRenderTarget* pre_alpha_depth,
                 LLRenderTarget* exposure, LLVertexBuffer* screen_triangle)
    {
        sCASSharpness = 0.f;
        if (!sFrameActive)
        {
            return false; // beginFrame decided what becomes of the history
        }
        if (!sCameraTaken || !screen_triangle || !exposure || !screen.isComplete())
        {
            sHistoryValid = false;
            return false;
        }
        LL_PROFILE_GPU_ZONE("taa resolve");

        const U32 width = screen.getWidth();
        const U32 height = screen.getHeight();
        bool reset = !sHistoryValid;
        if (ensure_target(sHistory[0], width, height))
        {
            reset = true;
        }
        if (ensure_target(sHistory[1], width, height))
        {
            reset = true;
        }
        if (!sHistory[0]->isComplete() || !sHistory[1]->isComplete())
        {
            sHistoryValid = false;
            return false;
        }
        sWidth = width;
        sHeight = height;

        const U32 frame = LLFrameTimer::getFrameCount();
        const bool pre_alpha = sPreAlpha && sPreAlpha->isComplete() && sPreAlphaFrame == frame &&
                               sPreAlpha->getWidth() == width && sPreAlpha->getHeight() == height;
        const bool have_pre_alpha_depth = pre_alpha_depth && pre_alpha_depth->isComplete() && sPreAlphaDepthFrame == frame &&
                                     pre_alpha_depth->getWidth() == depth_source.getWidth() &&
                                     pre_alpha_depth->getHeight() == depth_source.getHeight();

        // This frame's depth is decoded with the projection it was rasterised with: the depth
        // convention (reverse-Z) and the jitter. In double: the view matrices hold agent space
        // positions, and the difference of two nearly equal cameras is what matters here.
        const glm::dmat4 cur_proj = jittered(glm::dmat4(FSReverseZ::depthProjection(sCurProj)), sJitterX, sJitterY);
        const glm::dmat4 inv_proj = glm::inverse(cur_proj);
        // this frame's view space -> last frame's clip space (unjittered: the history is)
        const glm::dmat4 reproject = glm::dmat4(sPrevProj) * glm::dmat4(sPrevView) * glm::inverse(glm::dmat4(sCurView));
        const glm::mat4 inv_proj_f(inv_proj);
        const glm::mat4 reproject_f(reproject);

        static LLCachedControl<F32> history_weight(gSavedSettings, "FSRenderTAAHistory", 0.9f);
        static LLCachedControl<F32> reactive(gSavedSettings, "FSRenderTAAReactive", 0.7f);
        static LLCachedControl<F32> sharpness(gSavedSettings, "FSRenderTAASharpness", 0.5f);
        static LLCachedControl<U32> debug_view(gSavedSettings, "FSRenderTAADebug", 0);
        const U32 debug = llmin((U32)debug_view, 5U);
        if (debug)
        {
            ensure_target(sDebug, width, height);
        }
        else if (sDebug)
        {
            delete_target(sDebug);
        }
        const bool show_debug = debug && sDebug && sDebug->isComplete();

        LLRenderTarget& prev = *sHistory[sHistoryIndex];
        LLRenderTarget& next = *sHistory[1 - sHistoryIndex];

        LLGLDepthTest depth(GL_FALSE, GL_FALSE);
        LLGLDisable blend(GL_BLEND);

        // Resolve into the other history target
        {
            LLGLSLShader& shader = gFSTAAResolveProgram;
            next.bindTarget();
            shader.bind();
            shader.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, &screen);
            shader.bindTexture(LLShaderMgr::DEFERRED_DEPTH, &depth_source, true);
            shader.bindTexture(LLShaderMgr::EXPOSURE_MAP, exposure);
            // unused samplers still get a texture of their own kind
            shader.bindTexture(LLShaderMgr::SCENE_MAP, pre_alpha ? sPreAlpha : &screen);
            shader.bindTexture(LLShaderMgr::SCENE_DEPTH, have_pre_alpha_depth ? pre_alpha_depth : &depth_source, true);

            // the history, on the first unit past the program's own samplers
            const S32 unit = shader.mActiveTextureChannels;
            const bool unit_ok = unit >= 0 && unit < llmin(gGLManager.mNumTextureImageUnits, (S32)LL_NUM_TEXTURE_LAYERS);
            if (unit_ok)
            {
                LLTexUnit* tex_unit = gGL.getTexUnit(unit);
                tex_unit->bind(&prev);
                tex_unit->setTextureFilteringOption(LLTexUnit::TFO_BILINEAR);
                tex_unit->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
                shader.uniform1i(sHistoryMap, unit);
            }
            else
            {
                reset = true;
            }

            shader.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (F32)width, (F32)height);
            shader.uniform2f(sJitter, sJitterX * 0.5f * (F32)width, sJitterY * 0.5f * (F32)height);
            shader.uniformMatrix4fv(sInvProj, 1, GL_FALSE, glm::value_ptr(inv_proj_f));
            shader.uniformMatrix4fv(sReproject, 1, GL_FALSE, glm::value_ptr(reproject_f));
            shader.uniform1f(sBlend, 1.f - llclamp((F32)history_weight, 0.5f, 0.98f));
            shader.uniform1f(sReactive, llclamp((F32)reactive, 0.f, 1.f));
            shader.uniform1f(sReset, reset ? 1.f : 0.f);
            shader.uniform2f(sPreAlphaValid, pre_alpha ? 1.f : 0.f, have_pre_alpha_depth ? 1.f : 0.f);
            shader.uniform4f(sDebugView, 0.f, (F32)sPhase, 0.f, 0.f);

            screen_triangle->setBuffer();
            screen_triangle->drawArrays(LLRender::TRIANGLES, 0, 3);

            if (show_debug)
            {
                // the same resolve again, writing the false colours instead (the history above is
                // the real result, so the view can be switched on and off without disturbing it)
                next.flush();
                sDebug->bindTarget();
                shader.uniform4f(sDebugView, (F32)debug, (F32)sPhase, 0.f, 0.f);
                screen_triangle->setBuffer();
                screen_triangle->drawArrays(LLRender::TRIANGLES, 0, 3);
                shader.uniform4f(sDebugView, 0.f, (F32)sPhase, 0.f, 0.f);
                sDebug->flush();
                next.bindTarget();
            }

            if (unit_ok)
            {
                gGL.getTexUnit(unit)->unbind(LLTexUnit::TT_TEXTURE);
            }
            shader.unbind();
            next.flush();
        }

        // Back into the screen target as it is (or the false colours). Its alpha is left as it
        // was. The sharpening is CAS's, once, after the tone mapping (casSharpness): the 5-tap
        // unsharp mask this pass ran, held within the taps' range, could hardly sharpen at all.
        {
            LLGLSLShader& shader = gFSTAASharpenProgram;
            screen.bindTarget();
            gGL.setColorMask(true, false);
            shader.bind();
            shader.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, show_debug ? sDebug : &next);
            shader.bindTexture(LLShaderMgr::EXPOSURE_MAP, exposure);
            shader.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (F32)width, (F32)height);
            shader.uniform1f(sSharpness, 0.f);
            sCASSharpness = show_debug ? 0.f : llclamp((F32)sharpness, 0.f, 1.f);
            screen_triangle->setBuffer();
            screen_triangle->drawArrays(LLRender::TRIANGLES, 0, 3);
            shader.unbind();
            gGL.setColorMask(true, true);
            screen.flush();
        }

        sHistoryIndex = 1 - sHistoryIndex;
        sHistoryValid = true;
        sPrevView = sCurView;
        sPrevProj = sCurProj;
        return true;
    }

    F32 casSharpness()
    {
        return sCASSharpness;
    }

    bool beginDeferOverlays(bool main_view, bool& dropped_stale)
    {
        dropped_stale = false;
        if (!main_view || !sFrameActive)
        {
            return false;
        }
        const U32 frame = LLFrameTimer::getFrameCount();
        // Held back in an earlier frame that never reached renderFinalize: what that frame
        // collected may refer to faces gone since
        dropped_stale = sOverlayFrame != 0 && sOverlayFrame != frame;
        sOverlayFrame = frame;
        sDebugAlphaFrame = 0;
        sDeferring = true;
        return true;
    }

    void endDeferOverlays()
    {
        sDeferring = false;
    }

    bool deferringOverlays()
    {
        return sDeferring;
    }

    void noteDebugAlphaDeferred()
    {
        if (sDeferring)
        {
            sDebugAlphaFrame = sOverlayFrame;
        }
    }

    bool takeDeferredOverlays(bool& debug_alpha)
    {
        const U32 frame = LLFrameTimer::getFrameCount();
        const bool pending = sOverlayFrame != 0 && sOverlayFrame == frame;
        debug_alpha = pending && sDebugAlphaFrame == frame;
        sOverlayFrame = 0;
        sDebugAlphaFrame = 0;
        return pending;
    }

    bool prepareOverlayDepth(LLRenderTarget& depth_source, U32 width, U32 height, LLVertexBuffer* screen_triangle)
    {
        if (!sFrameActive || !build_overlay_depth(sOverlayDepth, depth_source, width, height, screen_triangle))
        {
            return false;
        }
        sOverlayDepthFrame = LLFrameTimer::getFrameCount();
        sOverlayDepthSource = &depth_source;
        return true;
    }

    bool preparePresentedDepth(LLRenderTarget& present_depth, U32 width, U32 height, LLVertexBuffer* screen_triangle)
    {
        const U32 frame = LLFrameTimer::getFrameCount();
        if (!sFrameActive)
        {
            return false;
        }
        if (sOverlayDepth && sOverlayDepth->getDepth() && sOverlayDepthFrame == frame && sOverlayDepthSource == &present_depth &&
            sOverlayDepth->getWidth() == width && sOverlayDepth->getHeight() == height)
        {
            sPresentedDepth = sOverlayDepth;
        }
        else if (build_overlay_depth(sPresentedDepthOwn, present_depth, width, height, screen_triangle))
        {
            sPresentedDepth = sPresentedDepthOwn;
        }
        else
        {
            return false;
        }
        sPresentedDepthFrame = frame;
        return true;
    }

    void beginOverlays(LLRenderTarget& screen)
    {
        sOverlayDepthSwapped = false;
        const bool built = sFrameActive && sOverlayDepth && sOverlayDepth->getDepth() &&
                           sOverlayDepthFrame == LLFrameTimer::getFrameCount() &&
                           sOverlayDepth->getWidth() == screen.getWidth() && sOverlayDepth->getHeight() == screen.getHeight();
        screen.bindTarget();
        if (built)
        {
            // screen shares depth_source's depth buffer (LLPipeline::allocateScreenBuffer); for
            // the overlays it gets the conservative one instead, put back by endOverlays
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, sOverlayDepth->getDepth(), 0);
            sOverlayDepthSwapped = true;
        }
    }

    void endOverlays(LLRenderTarget& screen, LLRenderTarget& depth_source)
    {
        gGL.flush();
        llassert(LLRenderTarget::getCurrentBoundTarget() == &screen);
        if (sOverlayDepthSwapped)
        {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth_source.getDepth(), 0);
            sOverlayDepthSwapped = false;
        }
        screen.flush();
    }

    void noteBeacon()
    {
        sBeaconsPending = true;
    }

    bool beaconsPending()
    {
        return sBeaconsPending;
    }

    void beaconsDrawn()
    {
        sBeaconsPending = false;
    }

    PresentedDepthScope::PresentedDepthScope()
    : mOn(false)
    {
        if (!sFrameActive || gSnapshot || LLPipeline::sRenderingHUDs || LLPipeline::sReflectionRender ||
            !sPresentedDepth || !sPresentedDepth->getDepth() || sPresentedDepthFrame != LLFrameTimer::getFrameCount() ||
            !gFSTAADepthCopyProgram.isComplete() || !gPipeline.mScreenTriangleVB)
        {
            return;
        }
        const S32 width = gGLViewport[2];
        const S32 height = gGLViewport[3];
        if (width <= 0 || height <= 0)
        {
            return;
        }
        LL_PROFILE_GPU_ZONE("taa presented overlay depth");

        // Keep the framebuffer's depth (the present's, and whatever was drawn into it since) in a
        // texture of its format: the default framebuffer's is 24 bit, a render target's the
        // pipeline's. A copy on the GPU, nothing is read back.
        const U32 format = LLRenderTarget::getCurrentBoundTarget() ? (U32)FSReverseZ::depthFormat() : (U32)GL_DEPTH_COMPONENT24;
        LLTexUnit* unit0 = gGL.getTexUnit(0);
        const U32 prev_tex = unit0->getCurrTexture();
        const LLTexUnit::eTextureType prev_type = unit0->getCurrType();
        if (!sSavedDepth)
        {
            LLImageGL::generateTextures(1, &sSavedDepth, GL_TEXTURE_2D);
        }
        unit0->bindManual(LLTexUnit::TT_TEXTURE, sSavedDepth);
        if (sSavedDepthFormat != format || sSavedDepthWidth != width || sSavedDepthHeight != height)
        {
            glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, GL_DEPTH_COMPONENT,
                         format == GL_DEPTH_COMPONENT32F ? GL_FLOAT : GL_UNSIGNED_INT, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
            sSavedDepthFormat = format;
            sSavedDepthWidth = width;
            sSavedDepthHeight = height;
        }
        gGL.flush();
        // every texel is written here before it is read
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, gGLViewport[0], gGLViewport[1], width, height);
        if (prev_tex && prev_type != LLTexUnit::TT_NONE)
        {
            unit0->bindManual(prev_type, prev_tex);
        }
        else
        {
            unit0->unbind(LLTexUnit::TT_TEXTURE);
        }

        // The conservative depth where it is nearer than what is there: what was drawn over the
        // world since the present (HUDs, the physics shapes' depth) stays in front
        write_depth(sPresentedDepth->getDepth(), GL_LEQUAL);
        mOn = true;
    }

    PresentedDepthScope::~PresentedDepthScope()
    {
        if (mOn)
        {
            // the framebuffer's own depth back, exactly
            write_depth(sSavedDepth, GL_ALWAYS);
        }
    }

    void release()
    {
        if (sSavedDepth)
        {
            LLImageGL::deleteTextures(1, &sSavedDepth);
            sSavedDepth = 0;
            sSavedDepthFormat = 0;
            sSavedDepthWidth = 0;
            sSavedDepthHeight = 0;
        }
        delete_target(sHistory[0]);
        delete_target(sHistory[1]);
        delete_target(sPreAlpha);
        delete_target(sDebug);
        delete_target(sOverlayDepth);
        delete_target(sPresentedDepthOwn);
        sPresentedDepth = nullptr;
        sOverlayDepthSource = nullptr;
        sHistoryValid = false;
        sWidth = 0;
        sHeight = 0;
        sPreAlphaFrame = 0;
        sPreAlphaDepthFrame = 0;
    }
}
