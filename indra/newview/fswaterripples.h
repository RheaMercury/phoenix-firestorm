/**
 * @file fswaterripples.h
 * @brief Interactive water: ripples, wakes, entry splashes and churned-water foam (FSRenderWaterRipples)
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

#ifndef FS_WATERRIPPLES_H
#define FS_WATERRIPPLES_H

#include "stdtypes.h"

class LLGLSLShader;
class LLVertexBuffer;

// A wave simulation on a square grid of cells that follows the camera (world aligned, its corner
// snapped to whole cells, so ripples stay put while the grid moves). Each step splats the avatars
// and moving objects that cross the water plane, as capsules from where they were to where they
// are (wakes stay continuous at any frame rate), then advances a damped wave equation by a fixed
// 1/60 s. The water shaders (fsWaterRipplesF.glsl) tilt their normal by the slope of the height
// field, and with FSRenderWaterRippleFoam draw foam where it is steep.
//
// Everything is off and nothing is allocated or gathered unless FSRenderWaterRipples is on.
namespace FSWaterRipples
{
    // FSRenderWaterRipples
    bool enabled();

    // Builds the simulation program (call from the water shader load). It stays unloaded while
    // the feature is off.
    void loadShaders(S32 shader_level);

    // Gathers the sources and advances the simulation, at most once a frame and only for the main
    // view: reflection probes, mirrors, HUDs and snapshots (whose tiles must match) never step it.
    // Call before the water shader is bound; it restores the bound render target.
    void update(LLVertexBuffer* screen_triangle);

    // Binds the ripple map and its uniforms to a water shader built with FS_WATER_RIPPLES, on
    // texture unit 'unit'. Returns the unit used, or -1 (nothing bound: the shader is told the
    // ripples are off).
    S32 bind(LLGLSLShader& shader, S32 unit);

    // Frees the maps and forgets the sources (GL buffers released, feature switched off)
    void release();
}

#endif // FS_WATERRIPPLES_H
