/**
 * @file fswaterobstacles.h
 * @brief Interactive water: the obstacle mask that ripples reflect off and flow around (FSRenderWaterRippleObstacles)
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

#ifndef FS_WATEROBSTACLES_H
#define FS_WATEROBSTACLES_H

#include "stdtypes.h"

class LLRenderTarget;
class LLVector3d;

// Which cells of the ripple grid (fswaterripples.cpp) are solid at the water surface: posts,
// piers, hulls, rocks and the shore. A small top-down render of the static geometry that reaches
// the water surface and of the terrain, world anchored, a little larger than the ripple grid so it
// keeps covering it while the grid follows the camera. It is drawn again only when the camera has
// moved an eighth of the grid away from where it was drawn, the grid changed, or a check every few
// seconds finds the geometry near the water changed. Never per frame while still, never for
// reflection probes, mirrors or snapshots (the ripples' own update rules), and no readbacks.
//
// Avatars and moving objects are left out: they make ripples of their own, and a mask a few
// seconds old would leave walls where they were.
namespace FSWaterObstacles
{
    // FSRenderWaterRippleObstacles (the ripples must be on as well)
    bool enabled();

    // Builds the mask program; unloads it and frees the mask while the obstacles are off
    void loadShaders(S32 shader_level);

    // Brings the mask up to date for a ripple grid of 'res' cells of 'cell' metres around 'camera'
    // (global). 'force' checks the geometry now (the simulation started over). Main view only,
    // from FSWaterRipples::update; restores the bound render target.
    void update(S32 res, F64 cell, const LLVector3d& camera, F64 now, bool force);

    // The mask for cells of 'cell' metres, or nullptr. Its texel (0, 0) is the cell at whole cells
    // (origin_x, origin_y) of global coordinates.
    LLRenderTarget* mask(F64 cell, S64& origin_x, S64& origin_y);

    // Frees the mask (GL buffers released, the feature switched off)
    void release();

    // FSRenderWaterRippleStats: a simulation pass ran with the obstacles on, with or without a mask
    void noteSimulation(bool with_mask);

    // FSRenderWaterRippleStats: counts a water pass and, every 10 s, logs whether the obstacles are
    // active and when the mask was last drawn. From FSWaterRipples::update, every call.
    void logStats(F64 now);
}

#endif // FS_WATEROBSTACLES_H
