/**
 * @file fsshadowscreenfilter.cpp
 * @brief Screen-space filtering of the PCSS sun shadow (FSRenderShadowScreenFilter)
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

#include "fsshadowscreenfilter.h"

#include "llframetimer.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llviewercontrol.h"

#include <glm/gtc/type_ptr.hpp>

extern bool gCubeSnapshot;

namespace
{
    constexpr F32 HISTORY_WEIGHT = 0.8f;    // about five frames

    const LLStaticHashedString sFilter("fs_screen_filter");
    const LLStaticHashedString sPenumbraMap("fs_shadow_penumbra");
    const LLStaticHashedString sHistoryMap("fs_shadow_history");
    const LLStaticHashedString sHistoryWeight("fs_shadow_history_weight");
    const LLStaticHashedString sToPrevView("fs_shadow_to_prev_view");
    const LLStaticHashedString sProj("fs_shadow_proj");
    const char* const PERMUTATION = "FS_SHADOW_SCREEN_FILTER";

    U32 sWidth = 0;                 // size of the textures below (the light map's)
    U32 sHeight = 0;
    U32 sPenumbraTex = 0;           // R16F: penumbra radius, screen pixels
    U32 sSunFBO = 0;                // light map + penumbra
    U32 sHistoryTex[2] = { 0, 0 };  // RG16F: filtered sun shadow, view depth
    U32 sHistoryFBO[2] = { 0, 0 };  // light map + history
    U32 sHistoryIndex = 0;          // the history written next
    U32 sHistoryFrame = 0;          // frame the history was last written, 0 none

    bool sActive = false;           // the sun pass wrote the penumbra; the blur reads it
    bool sTemporalPass = false;     // the blur's second pass writes the history
    S32 sBoundUnit = -1;            // first of the two units bindBlur took

    void delete_texture(U32& tex)
    {
        if (!tex)
        {
            return;
        }
        // don't leave a texture unit pointing at a deleted name
        for (U32 i = 0; i < LL_NUM_TEXTURE_LAYERS; ++i)
        {
            LLTexUnit* unit = gGL.getTexUnit(i);
            if (unit && unit->getCurrTexture() == tex)
            {
                unit->unbind(LLTexUnit::TT_TEXTURE);
            }
        }
        glDeleteTextures(1, &tex);
        tex = 0;
    }

    void delete_fbo(U32& fbo)
    {
        if (fbo)
        {
            glDeleteFramebuffers(1, &fbo);
            fbo = 0;
        }
    }

    void release_history()
    {
        for (U32 i = 0; i < 2; ++i)
        {
            delete_fbo(sHistoryFBO[i]);
            delete_texture(sHistoryTex[i]);
        }
        sHistoryFrame = 0;
    }

    U32 make_texture(U32 width, U32 height, GLenum internal_format, GLenum format, GLint filter)
    {
        U32 tex = 0;
        glGenTextures(1, &tex);
        if (!tex)
        {
            return 0;
        }
        LLTexUnit* unit = gGL.getTexUnit(0);
        unit->bindManual(LLTexUnit::TT_TEXTURE, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, internal_format, width, height, 0, format, GL_HALF_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        unit->unbind(LLTexUnit::TT_TEXTURE);
        return tex;
    }

    // An FBO with the light map at attachment 0 and second at 1, both drawn to. Checked once here;
    // the light map is attached again on every use (attach_light_map), as its texture can be
    // replaced under the same size.
    bool make_fbo(U32& fbo, U32 light_map, U32 second)
    {
        glGenFramebuffers(1, &fbo);
        if (!fbo)
        {
            return false;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, light_map, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, second, 0);
        const GLenum buffers[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
        glDrawBuffers(2, buffers);
        const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glBindFramebuffer(GL_FRAMEBUFFER, LLRenderTarget::sCurFBO);
        return ok;
    }

    // binds fbo with this frame's light map texture at attachment 0
    void bind_with_light_map(U32 fbo, U32 light_map)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, light_map, 0);
    }

    bool ensure_sun_targets(LLRenderTarget& light_map)
    {
        const U32 tex = light_map.getTexture(0);
        const U32 width = light_map.getWidth();
        const U32 height = light_map.getHeight();
        if (!tex || !width || !height)
        {
            return false;
        }
        if (width == sWidth && height == sHeight && sPenumbraTex && sSunFBO)
        {
            return true;
        }

        FSShadowScreenFilter::release();
        sPenumbraTex = make_texture(width, height, GL_R16F, GL_RED, GL_NEAREST);
        if (!sPenumbraTex || !make_fbo(sSunFBO, tex, sPenumbraTex))
        {
            LL_WARNS("Render") << "Shadow screen filter targets for " << width << "x" << height << " could not be allocated" << LL_ENDL;
            FSShadowScreenFilter::release();
            return false;
        }
        sWidth = width;
        sHeight = height;
        return true;
    }

    bool ensure_history(LLRenderTarget& light_map)
    {
        const U32 tex = light_map.getTexture(0);
        if (!tex || light_map.getWidth() != sWidth || light_map.getHeight() != sHeight)
        {
            return false;
        }
        if (sHistoryTex[0] && sHistoryTex[1] && sHistoryFBO[0] && sHistoryFBO[1])
        {
            return true;
        }
        release_history();
        for (U32 i = 0; i < 2; ++i)
        {
            sHistoryTex[i] = make_texture(sWidth, sHeight, GL_RG16F, GL_RG, GL_LINEAR);
            if (!sHistoryTex[i] || !make_fbo(sHistoryFBO[i], tex, sHistoryTex[i]))
            {
                LL_WARNS("Render") << "Shadow screen filter history could not be allocated" << LL_ENDL;
                release_history();
                return false;
            }
        }
        sHistoryIndex = 0;
        return true;
    }

    bool temporal_wanted()
    {
        static LLCachedControl<bool> temporal(gSavedSettings, "FSRenderShadowScreenFilterTemporal", false);
        return temporal;
    }
}

void FSShadowScreenFilter::beginSunPass(LLGLSLShader& sun_shader, LLRenderTarget& light_map, bool blur_follows)
{
    sActive = false;
    const bool built = sun_shader.mDefines.count(PERMUTATION) != 0;
    if (!built)
    {
        if (!gCubeSnapshot && sPenumbraTex)
        {
            release(); // switched off: give the memory back
        }
        return;
    }
    if (!blur_follows || gCubeSnapshot || !ensure_sun_targets(light_map))
    {
        return;
    }
    // the history is made here too, while no program has its textures bound (making a texture
    // goes through unit 0)
    if (!temporal_wanted())
    {
        if (sHistoryTex[0] || sHistoryTex[1])
        {
            release_history();
        }
    }
    else
    {
        ensure_history(light_map);
    }
    bind_with_light_map(sSunFBO, light_map.getTexture(0));
    sActive = true;
}

void FSShadowScreenFilter::bindSun(LLGLSLShader& sun_shader)
{
    if (sun_shader.mDefines.count(PERMUTATION))
    {
        sun_shader.uniform1i(sFilter, sActive ? 1 : 0);
    }
}

void FSShadowScreenFilter::endSunPass()
{
    if (sActive)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, LLRenderTarget::sCurFBO);
    }
}

void FSShadowScreenFilter::beginBlurSecond(LLRenderTarget& light_map)
{
    sTemporalPass = false;
    if (!sActive || !temporal_wanted() || !sHistoryFBO[0] || !sHistoryFBO[1] || !light_map.getTexture(0) ||
        light_map.getWidth() != sWidth || light_map.getHeight() != sHeight)
    {
        return;
    }
    bind_with_light_map(sHistoryFBO[sHistoryIndex], light_map.getTexture(0));
    sTemporalPass = true;
}

void FSShadowScreenFilter::endBlurSecond()
{
    if (sTemporalPass)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, LLRenderTarget::sCurFBO);
        sHistoryFrame = LLFrameTimer::getFrameCount();
        sHistoryIndex = 1 - sHistoryIndex;
    }
    sTemporalPass = false;
    sActive = false;
}

void FSShadowScreenFilter::bindBlur(LLGLSLShader& blur_shader, bool second_pass)
{
    sBoundUnit = -1;
    if (!blur_shader.mDefines.count(PERMUTATION))
    {
        return;
    }
    // two units past the ones the program uses for its own samplers
    const S32 unit = blur_shader.mActiveTextureChannels;
    const bool on = sActive && sPenumbraTex && unit >= 0 && unit + 1 < (S32)LL_NUM_TEXTURE_LAYERS;
    blur_shader.uniform1i(sFilter, on ? 1 : 0);
    blur_shader.uniform1f(sHistoryWeight, 0.f);
    if (!on)
    {
        return;
    }

    gGL.getTexUnit(unit)->bindManual(LLTexUnit::TT_TEXTURE, sPenumbraTex);
    blur_shader.uniform1i(sPenumbraMap, unit);

    F32 weight = 0.f;
    if (second_pass && sTemporalPass)
    {
        // last frame's result, when it was written last frame (a resize or a frame without the
        // filter breaks the chain)
        const U32 frame = LLFrameTimer::getFrameCount();
        const bool valid = sHistoryFrame != 0 && sHistoryFrame + 1 == frame;
        gGL.getTexUnit(unit + 1)->bindManual(LLTexUnit::TT_TEXTURE, sHistoryTex[1 - sHistoryIndex]);
        weight = valid ? HISTORY_WEIGHT : 0.f;
        const glm::mat4 proj = get_current_projection();
        blur_shader.uniformMatrix4fv(sToPrevView, 1, GL_FALSE, glm::value_ptr(gGLInverseDeltaModelView));
        blur_shader.uniformMatrix4fv(sProj, 1, GL_FALSE, glm::value_ptr(proj));
    }
    else
    {
        // not read with a weight of 0, but the sampler still gets a 2D texture of its own unit
        gGL.getTexUnit(unit + 1)->bindManual(LLTexUnit::TT_TEXTURE, sPenumbraTex);
    }
    blur_shader.uniform1i(sHistoryMap, unit + 1);
    blur_shader.uniform1f(sHistoryWeight, weight);
    sBoundUnit = unit;
}

void FSShadowScreenFilter::unbindBlur()
{
    if (sBoundUnit >= 0)
    {
        gGL.getTexUnit(sBoundUnit)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.getTexUnit(sBoundUnit + 1)->unbind(LLTexUnit::TT_TEXTURE);
        sBoundUnit = -1;
    }
}

void FSShadowScreenFilter::release()
{
    release_history();
    delete_fbo(sSunFBO);
    delete_texture(sPenumbraTex);
    sWidth = 0;
    sHeight = 0;
    sActive = false;
    sTemporalPass = false;
}
