/**
 * @file fsshadowscreenfilter.h
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

#ifndef FS_SHADOWSCREENFILTER_H
#define FS_SHADOWSCREENFILTER_H

#include "stdtypes.h"

class LLGLSLShader;
class LLRenderTarget;

// With PCSS soft shadows (RenderShadowPCSS), the sun shadow's wide penumbrae normally come from a
// wide 12 tap kernel in the shadow map. With FSRenderShadowScreenFilter the sun pass keeps the
// blocker search but uses a narrow 4 tap kernel, and writes the penumbra's width (screen pixels)
// to a second target next to the light map; the light map blur then blurs the sun channel that
// wide, depth and normal aware (fsShadowScreenFilterF.glsl). FSRenderShadowScreenFilterTemporal
// also blends the result with the last frame's, reprojected, in the blur's second pass, which
// then writes a history target as its second output.
//
// The permutation (FS_SHADOW_SCREEN_FILTER) is built into the PCSS sun program, its shadowUtil.glsl
// build and the blur program only while the setting is on. At run time it is used only where the
// blur runs after the sun pass; otherwise the shaders take their usual path (uniform
// fs_screen_filter). Nothing is allocated while it is off.
//
// Call order inside LLPipeline::renderDeferredLighting, main view or probe:
//   light_map.bindTarget(); beginSunPass(); bind the sun program; bindSun(); draw; endSunPass(); light_map.flush();
//   blur pass 1: bind the blur program; bindBlur(shader, false); draw; unbindBlur()
//   blur pass 2: bind it; light_map.bindTarget(); beginBlurSecond(); bindBlur(shader, true); draw; unbindBlur(); endBlurSecond()
namespace FSShadowScreenFilter
{
    // The sun pass, light_map bound as target and before the sun program binds its textures (the
    // targets are made here): switches to the light map plus penumbra target pair when sun_shader
    // has the permutation and blur_follows. bindSun then tells the bound program whether to write it.
    void beginSunPass(LLGLSLShader& sun_shader, LLRenderTarget& light_map, bool blur_follows);
    void bindSun(LLGLSLShader& sun_shader);
    void endSunPass();

    // Blur passes (shader bound): the penumbra map, and for the second pass the history and the
    // reprojection. Sets fs_screen_filter either way.
    void bindBlur(LLGLSLShader& blur_shader, bool second_pass);
    void unbindBlur();

    // The blur's second pass (light_map bound as target): with the temporal option, switches to the
    // light map plus history target pair
    void beginBlurSecond(LLRenderTarget& light_map);
    void endBlurSecond();

    void release();
}

#endif // FS_SHADOWSCREENFILTER_H
