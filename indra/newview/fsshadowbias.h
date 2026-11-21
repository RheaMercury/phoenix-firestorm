/**
 * @file fsshadowbias.h
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

#ifndef FS_SHADOWBIAS_H
#define FS_SHADOWBIAS_H

#include "stdtypes.h"

#include <glm/mat4x4.hpp>

class LLGLSLShader;

// The stock sun shadow bias (RenderShadowBias, -0.002) is in shadow depth units: a fraction of each
// cascade's depth range along the light. Cascade 0 spans some 20 m of depth, so it is a few
// centimetres there; the far cascades span hundreds of metres (casters toward the sun are included),
// so it is a metre or more there and shadows come off distant posts and tree trunks.
//
// FSRenderShadowBiasMetric: the bias is FSRenderShadowBiasMetres along the light in every cascade, or
// FSRenderShadowBiasTexels of the cascade's texel size if that is more (a coarse far map needs a
// little more than a fine near one), and shadowUtil.glsl turns it into depth units at each pixel.
// Off, the shaders get 0 and use shadow_bias as before.
//
// FSRenderShadowCascadeBand: half the width of the blend between two cascades, as a fraction of the
// split's distance. 0.25 is the stock band; a narrower one samples two cascades on fewer pixels and,
// as the cascades' slices are cut with the same band, gives each a little more resolution.
//
// Reflection probe captures keep the stock bias and band: their shadow maps are low resolution and
// are cut with the stock padding.
namespace FSShadowBias
{
    // FSRenderShadowCascadeBand, clamped; 0.25 (stock) in a reflection probe capture
    F32 cascadeBand();

    // Sets fs_shadow_bias_metres and fs_shadow_cascade_band on a program LLPipeline::bindDeferredShader
    // binds. sun_matrix: the four sun cascades' matrices (view space to shadow texture);
    // map_width / map_height: their shadow maps' sizes in texels.
    void bind(LLGLSLShader& shader, const glm::mat4* sun_matrix, const U32* map_width, const U32* map_height);
}

#endif // FS_SHADOWBIAS_H
