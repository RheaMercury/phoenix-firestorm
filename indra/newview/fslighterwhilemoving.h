/**
 * @file fslighterwhilemoving.h
 * @brief FSRenderLighterWhileMoving: lighter frames while the camera walks, runs or turns
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

#ifndef FS_LIGHTERWHILEMOVING_H
#define FS_LIGHTERWHILEMOVING_H

#include "stdtypes.h"

class LLDrawable;
class LLUUID;

// The fork's camera motion test (fs_update_camera_motion in pipeline.cpp) only counted turning faster than
// about 35 degrees a second or travelling faster than 20 m/s, so plain walking (3 m/s) and running never
// engaged any of the motion throttles, while every step changed object detail, re-rendered the far sun
// cascades and pushed texture memory into the soft zone.
//
// FSRenderLighterWhileMoving adds a "walking" tier below that (walking, running, gentle turns, smoothed and
// held for half a second so it doesn't flicker), and options that use it, each with a setting of its own:
//  - FSRenderMovingRebuildBudget: the group rebuild throttle (FSPerfGroupRebuildBudgetMs) also while walking,
//    with a budget that follows what the frame has left of the target frame time;
//  - FSRenderMovingLODHold: no detail decreases while moving, increases need a margin; caught up once still;
//  - FSRenderMovingShadowReduce: FSRenderShadowReduceWhileMoving also while walking (gentler);
//  - FSRenderMovingTextureBiasHold: the texture discard bias doesn't step up from the soft zone while moving;
//  - FSRenderMovingShadowHold: the far sun cascades are held while the view still fits their maps;
//  - FSRenderMovingShadowCache: the far cascade static cache ignores changes it can't show, defers detail
//    changes while moving and is stored while moving;
//  - FSRenderMovingProbeHold: no periodic reflection probe refreshes while moving.
// FSRenderAsyncOcclusionReadback (not under the master option): occlusion query results are read without
// waiting for the render thread. FSRenderMotionStats logs what all of this did, every 10 seconds.
// Everything here runs on the main thread.
namespace FSMoving
{
    enum ETier : U32 { TIER_STILL = 0, TIER_WALKING, TIER_FAST, TIER_COUNT };
    // Why a far sun cascade was rendered before its interval (FSRenderShadowFarCascadeInterval)
    enum EForce : U32 { FORCE_SLICE = 0,    // FSRenderMovingShadowHold: the view's slice left the held map
                        FORCE_DEPTH,        // ...or its depth range
                        FORCE_NO_MAP,       // ...the held map had nothing to receive shadows when rendered
                        FORCE_VIEW,         // the sun, field of view or aspect changed, or a snapshot
                        FORCE_MOVED,        // the camera moved 2% of the range or turned 3 degrees (option off)
                        FORCE_COUNT };

    // pipeline.cpp, fs_update_camera_motion: once a frame, before the frame renders. speed in m/s and
    // turn_rate in radians a second of the camera this frame; fast: the existing "moving fast" test
    void updateTier(F32 speed, F32 turn_rate, bool fast, F32 dt);

    bool enabled();                 // FSRenderLighterWhileMoving
    ETier tier();                   // TIER_STILL while the master option is off
    bool moving();                  // master on, and walking or faster

    bool rebuildBudget();           // FSRenderMovingRebuildBudget, while moving
    bool rebuildSettling();         // FSRenderMovingRebuildBudget, the first seconds still after moving
    bool lodHold();                 // FSRenderMovingLODHold, while moving
    bool shadowReduce();            // FSRenderMovingShadowReduce with FSRenderShadowReduceWhileMoving, while walking
    bool textureBiasHold();         // FSRenderMovingTextureBiasHold, while moving
    bool shadowHoldEnabled();       // FSRenderMovingShadowHold, moving or not
    bool shadowCacheEnabled();      // FSRenderMovingShadowCache, moving or not
    bool probeHold();               // FSRenderMovingProbeHold, while moving
    bool asyncOcclusion();          // FSRenderAsyncOcclusionReadback
    bool statsEnabled();            // FSRenderMotionStats

    // FSRenderMovingRebuildBudget (pipeline.cpp updateGeom): the group rebuild budget in ms for this frame,
    // from the target frame time and what the last frame spent outside group rebuilds
    F32 frameRebuildBudgetMs(F32 max_budget_ms, F32 last_frame_ms, F32 last_group_ms);
    // llappviewer.cpp: the frame limiter (FSLimitFramerate) slept this long at the end of the last frame, which
    // the frame time includes but which is no work
    void noteLimiterSleep(S32 ms);
    // pipeline.cpp updateGeom, once a frame: the last frame's group rebuild ms (catchUpLOD's pace)
    void noteGroupRebuildMs(F32 ms);

    // FSRenderMovingLODHold (llvovolume.cpp calcLOD): a detail change of this object was held back
    // False when it can't be remembered (too many held already): the change must go ahead then
    bool noteLODHeld(const LLUUID& id, bool decrease);
    // ...and a decrease held for a few seconds may go one level down now (the wait then starts over)
    bool lodDecreaseDue(const LLUUID& id);
    // pipeline.cpp, once a frame before the rebuild queue: once still (or the option off), held objects
    // work out their detail again, a few hundred a frame (a few dozen while group rebuilds are heavy)
    void catchUpLOD();

    // FSRenderMovingShadowCache: llvovolume.cpp updateGeometry notes whether a drawable's geometry update was a
    // detail change only; llspatialpartition.cpp asks (and forgets) when its group rebuilds
    void noteVolumeUpdate(const LLDrawable* drawable, bool lod_only);
    bool takeLODOnly(const LLDrawable* drawable);

    // FSRenderMotionStats counters, reset after each log line
    struct Stats
    {
        F64 mTierSeconds[TIER_COUNT] = {};
        U32 mBudgetFrames = 0;          // frames the frame aware budget was worked out (moving)
        U32 mBudgetThrottled = 0;       // ...and drawable updates were cut short by it
        F64 mBudgetMsSum = 0.0;
        F64 mGroupMsSum = 0.0;          // group rebuild ms in those frames
        F32 mGroupMsPeak = 0.f;
        U32 mLODHeldDecrease = 0;
        U32 mLODHeldIncrease = 0;
        U32 mLODCaughtUp = 0;
        F64 mBiasHoldSeconds = 0.0;     // the texture bias would have stepped up, and didn't
        U32 mCascadeHeld = 0;           // far cascade updates skipped (held map reused)
        U32 mCascadeDue = 0;            // rendered on their interval
        U32 mCascadeForced[FORCE_COUNT] = {}; // rendered before their interval, by EForce
        U32 mCacheSmallIgnored = 0;     // static changes too small for the cascade that holds the cache
        U32 mCacheLODDeferred = 0;      // detail changes deferred while moving
        U32 mCacheLODApplied = 0;       // ...and applied later
        U32 mCacheDeferredReuse = 0;    // cache reused although only deferred detail changes differ
        U32 mCacheStoredMoving = 0;     // stored although the projection moved since the last update
        U32 mProbeSkipped = 0;          // frames a periodic probe refresh was put off
        U32 mOcclusionCached = 0;       // query results read from the render thread's collected results
        U32 mOcclusionPending = 0;      // ...not in yet (asked again next time)
        U32 mOcclusionUntracked = 0;    // queries the render thread no longer tracks: last result kept, asked again
        U32 mOcclusionWaited = 0;       // ...or read with a call that waits (option off)
    };
    Stats& stats();
}

#endif // FS_LIGHTERWHILEMOVING_H
