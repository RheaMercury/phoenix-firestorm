/**
 * @file fsshadowbias.cpp
 * @brief Sun shadow bias in metres per cascade, and the cascade blend band
 *        (FSRenderShadowBiasMetric, FSRenderShadowCascadeBand)
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

#include "fsshadowbias.h"

#include "llglslshader.h"
#include "llstaticstringtable.h"
#include "llviewercontrol.h"

#include <cmath>

extern bool gCubeSnapshot;

namespace
{
    constexpr F32 STOCK_BAND = 0.25f;

    // world size of one texel of a cascade's map, in metres: the map's x and y rows of the
    // view space to texture matrix give texture units per metre across the light (exact for the
    // orthographic cascades; for a warped one, its scale where w is 1)
    F32 texel_metres(const glm::mat4& m, U32 width, U32 height)
    {
        const F32 sx = std::sqrt(m[0][0] * m[0][0] + m[1][0] * m[1][0] + m[2][0] * m[2][0]);
        const F32 sy = std::sqrt(m[0][1] * m[0][1] + m[1][1] * m[1][1] + m[2][1] * m[2][1]);
        const F32 tx = (sx > 1.e-9f && width) ? 1.f / (sx * (F32)width) : 0.f;
        const F32 ty = (sy > 1.e-9f && height) ? 1.f / (sy * (F32)height) : 0.f;
        const F32 t = llmax(tx, ty);
        return std::isfinite(t) ? llmin(t, 100.f) : 0.f;
    }
}

F32 FSShadowBias::cascadeBand()
{
    if (gCubeSnapshot)
    {
        return STOCK_BAND;
    }
    static LLCachedControl<F32> band(gSavedSettings, "FSRenderShadowCascadeBand", STOCK_BAND);
    return llclamp((F32)band, 0.05f, STOCK_BAND);
}

void FSShadowBias::bind(LLGLSLShader& shader, const glm::mat4* sun_matrix, const U32* map_width, const U32* map_height)
{
    static const LLStaticHashedString s_bias_metres("fs_shadow_bias_metres");
    static const LLStaticHashedString s_band("fs_shadow_cascade_band");

    shader.uniform1f(s_band, cascadeBand());

    static LLCachedControl<bool> metric(gSavedSettings, "FSRenderShadowBiasMetric", false);
    F32 bias[4] = { 0.f, 0.f, 0.f, 0.f };
    if (metric && !gCubeSnapshot)
    {
        static LLCachedControl<F32> metres_setting(gSavedSettings, "FSRenderShadowBiasMetres", 0.05f);
        static LLCachedControl<F32> texels_setting(gSavedSettings, "FSRenderShadowBiasTexels", 1.f);
        const F32 metres = llclamp((F32)metres_setting, 0.f, 1.f);
        const F32 texels = llclamp((F32)texels_setting, 0.f, 4.f);
        // FSRenderShadowSlopeBias keeps only part of the fixed bias where the slope is measured from
        // the depth buffer (fs_bind_shadow_slope_bias in pipeline.cpp scales shadow_bias the same way)
        F32 keep = 1.f;
        if (shader.mDefines.count("FS_SHADOW_DEPTH_SLOPE"))
        {
            static LLCachedControl<F32> keep_setting(gSavedSettings, "FSRenderShadowSlopeBiasConstant", 0.5f);
            keep = llclamp((F32)keep_setting, 0.f, 1.f);
        }
        for (U32 i = 0; i < 4; ++i)
        {
            const F32 b = llmax(metres, texels * texel_metres(sun_matrix[i], map_width[i], map_height[i])) * keep;
            // never exactly 0, which the shaders read as off
            bias[i] = llmax(b, 1.e-5f);
        }
    }
    shader.uniform4fv(s_bias_metres, 1, bias);
}
