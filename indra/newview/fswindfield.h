/**
 * @file fswindfield.h
 * @brief The region's wind, smoothed for the rendering effects (FSRenderWindEffects)
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

#ifndef FS_WINDFIELD_H
#define FS_WINDFIELD_H

#include "v3dmath.h"
#include "v3math.h"

class LLGLSLShader;

// Each region sends a coarse (16 x 16 per region) and jumpy wind field (LLViewerRegion::mWind).
// Once a frame, while FSRenderWindEffects is on, this turns it into one smooth wind around the
// camera that rendering effects can follow:
//  - the wind at the camera, averaged over five points 32 m apart (they may fall in neighbouring
//    regions, which also softens region crossings), eased over a few seconds and held to a sane
//    speed; no region under a point means no wind there;
//  - the drift: the eased wind integrated over time (in metres, double precision). Effects scroll
//    their patterns by it rather than by velocity times time, so a change of wind never makes a
//    pattern jump;
//  - a gust signal: slow noise, stronger in stronger wind.
// After a teleport the eased wind starts again from the new place's wind. Main thread only.
class FSWindField
{
public:
    struct State
    {
        bool mActive = false;           // FSRenderWindEffects on and in world; everything else is zero when false
        LLVector3 mVelocity;            // eased wind at the camera, m/s, region axes (x east, y north, z 0),
                                        // not scaled by the strength
        LLVector3d mDrift;              // integral of mVelocity times the strength over time, metres; grows without bound
        F32 mStrength = 0.f;            // FSRenderWindEffectsStrength, 0 to 2
        F32 mGust = 0.f;                // 0 to 1: slow gusts, near 0 in still air
        F32 mGustSwing = 0.f;           // -1 to 1: the same gusts about their average, times the windiness (0 to 1)
        U32 mResets = 0;                // counts teleports and switch-ons, for users that keep their own state
    };

    // Once per frame, from the main loop
    static void update();

    // The current values (all zero while the option is off). For other effects, e.g. water.
    static const State& get();

    // Factor for the height fog's density (gusts): exactly 1 while the option is off
    static F32 fogDensityScale();

    // The dust motes' offset in metres: the drift, slowed for the sheltered air the motes float in.
    // Zero while the option is off.
    static LLVector3d dustDriftOffset();

    // The height fog's drifting density noise (fs_fog_wind*) for the deferred haze pass, from the
    // matrices current at its bind. Off (amplitude 0) while the option is off and for reflection
    // probes. A no-op without the height fog.
    static void uploadHeightFogUniforms(LLGLSLShader& shader);
};

#endif // FS_WINDFIELD_H
