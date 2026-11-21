/**
 * @file fsanimationlod.cpp
 * @brief Distant and small avatars evaluate their animations at a lower rate
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, Trish
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

#include "fsanimationlod.h"

#include "llagentcamera.h"
#include "llcontrolavatar.h"
#include "lldrawable.h"
#include "llfloaterreg.h"
#include "llframetimer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llvoavatarself.h"
#include "llvovolume.h"

extern bool gSnapshot;

bool FSAnimationLOD::sActive = false;

namespace
{
    constexpr U32 MAX_PERIOD = 4;
    // An avatar that just appeared, or whose sitting changed, is posed every frame for this many
    // frames: its skeleton, sit target and first animations settle in them
    constexpr U8 WARMUP_POSES = 30;
    // At low frame rates a period of frames is long: never leave a pose older than this
    constexpr F64 MAX_POSE_AGE = 0.125;
    // A tier only changes once the size is this factor past its boundary, so an avatar at a
    // boundary does not switch rate every frame
    constexpr F32 HYSTERESIS = 1.15f;

    U32 sFrame = 0;
    F64 sNow = 0.0;
    F32 sScreenHeightPx = 0.f;          // pixels per radian times the vertical view angle
    F32 sFullRateFraction = 0.3f;       // FSPerfAnimationLODScreenSize
    U32 sMaxPeriod = MAX_PERIOD;        // FSPerfAnimationLODMaxPeriod
    const LLViewerObject* sMySitRoot = nullptr;
    const LLVOAvatar* sFocusAvatar = nullptr;

    // Stats over the log interval (FSPerfProfileLogInterval)
    U64 sTierCount[MAX_PERIOD + 1] = {}; // [0]: exempt, [n]: posed every nth frame
    U64 sSkipped = 0;
    U64 sPosed = 0;
    U32 sStatFrames = 0;
    LLTimer sStatTimer;

    // Slowest tier for this share of the screen height: every frame from sFullRateFraction up,
    // every 2nd from half of it, every 3rd from a quarter, every 4th below
    U32 tier_for(F32 fraction)
    {
        U32 tier = 1;
        F32 boundary = sFullRateFraction;
        while (tier < sMaxPeriod && fraction < boundary)
        {
            ++tier;
            boundary *= 0.5f;
        }
        return tier;
    }

    // Share of the screen height the avatar's bounding sphere covers. The pixel area follows the
    // camera's zoom (a zoomed in avatar is large), and, as everywhere it is used, ramps nearby
    // avatars up.
    F32 screen_fraction(LLVOAvatar* avatar, F32 pixel_area)
    {
        if (sScreenHeightPx <= 0.f)
        {
            return 1.f;
        }
        if (pixel_area > 0.f && llfinite(pixel_area))
        {
            return 2.f * sqrtf(pixel_area / F_PI) / sScreenHeightPx;
        }
        // No area yet: the distance from the camera, as for a one meter radius
        F32 distance = avatar->mDrawable.notNull() ? avatar->mDrawable->mDistanceWRTCamera
                                                   : dist_vec(avatar->getPositionAgent(), LLViewerCamera::getInstance()->getOrigin());
        if (distance <= 1.f || !llfinite(distance))
        {
            return 1.f;
        }
        return 2.f * atanf(1.f / distance) * LLDrawable::sCurPixelAngle / sScreenHeightPx;
    }

    bool snapshot_open()
    {
        return gSnapshot
            || LLFloaterReg::instanceVisible("snapshot")
            || LLFloaterReg::instanceVisible("simple_snapshot")
            || LLFloaterReg::instanceVisible("flickr")
            || LLFloaterReg::instanceVisible("primfeed")
            || LLFloaterReg::instanceVisible("360capture");
    }
}

//static
void FSAnimationLOD::beginFrame()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "FSPerfAnimationLOD", false);
    static LLCachedControl<F32> screen_size(gSavedSettings, "FSPerfAnimationLODScreenSize", 0.3f);
    static LLCachedControl<U32> max_period(gSavedSettings, "FSPerfAnimationLODMaxPeriod", 4);

    logStats();

    // Everyone at full rate in mouselook (fast camera turns, aiming) and while any snapshot window
    // is open, so a picture never catches a pose a few frames old
    sActive = enabled && !gAgentCamera.cameraMouselook() && !snapshot_open();
    if (!sActive)
    {
        return;
    }

    sFrame = LLFrameTimer::getFrameCount();
    sNow = LLFrameTimer::getElapsedSeconds();
    sScreenHeightPx = LLDrawable::sCurPixelAngle * LLViewerCamera::getInstance()->getView();
    sFullRateFraction = llclamp((F32)screen_size, 0.01f, 2.f);
    sMaxPeriod = llclamp((U32)max_period, 1u, MAX_PERIOD);

    // Avatars sitting on the same object as you
    sMySitRoot = nullptr;
    if (isAgentAvatarValid() && gAgentAvatarp->getParent())
    {
        sMySitRoot = gAgentAvatarp->getRootEdit();
    }

    // The avatar the camera is focused on (alt-zoom, zoom in from the radar or a profile), or the
    // wearer of an attachment or the animated object it is focused on
    sFocusAvatar = nullptr;
    LLViewerObject* focus = gAgentCamera.getFocusObject();
    if (focus && !focus->isDead())
    {
        sFocusAvatar = focus->asAvatar();
        if (!sFocusAvatar)
        {
            sFocusAvatar = focus->getAvatar();
        }
    }
}

//static
bool FSAnimationLOD::isExempt(LLVOAvatar* avatar)
{
    if (avatar->isSelf() || avatar->isUIAvatar() || avatar->mNeedsAnimUpdate)
    {
        return true;
    }
    // Impostors are already only posed when their picture is redrawn, and that pose must be current
    if (avatar->isImpostor())
    {
        return true;
    }
    if (avatar->mFSAnimLODWarmup < WARMUP_POSES)
    {
        return true;
    }
    if (avatar == sFocusAvatar || avatar->isSelected())
    {
        return true;
    }
    if (sMySitRoot && avatar->getParent() && avatar->getRootEdit() == sMySitRoot)
    {
        return true;
    }
    if (avatar->isControlAvatar())
    {
        const LLVOVolume* root_volume = static_cast<LLControlAvatar*>(avatar)->mRootVolp;
        if (!root_volume)
        {
            return true;
        }
        // Worn animated objects follow their wearer's attachment point; ridden ones carry you
        if (root_volume->isAttachment() || root_volume->isSelected())
        {
            return true;
        }
        if (sMySitRoot && root_volume->getRootEdit() == sMySitRoot)
        {
            return true;
        }
    }
    return false;
}

//static
bool FSAnimationLOD::skipPose(LLVOAvatar* avatar)
{
    if (!avatar)
    {
        return false;
    }
    if (!sActive)
    {
        avatar->mFSAnimLODPeriod = 1;
        return false;
    }
    // updateCharacter() ran again this idle loop after queuing the pose for the pose workers
    // (FSPerfParallelAvatarUpdate): that pose is evaluated, nothing else may touch the motions
    if (avatar->mFSPosePending)
    {
        return false;
    }

    // Sitting down or standing up starts a new pose and moves the root: pose it at once
    const bool sitting = avatar->isSitting();
    if (sitting != avatar->mFSAnimLODSitting)
    {
        avatar->mFSAnimLODSitting = sitting;
        avatar->mFSAnimLODWarmup = 0;
    }

    U32 period = 1;
    if (isExempt(avatar))
    {
        avatar->mFSAnimLODPeriod = 1;
        ++sTierCount[0];
    }
    else
    {
        const F32 fraction = screen_fraction(avatar, avatar->mImpostorPixelArea);
        // The slowest tier the avatar may drop to, and the fastest it must rise to
        const U32 slow_tier = tier_for(fraction * HYSTERESIS);
        const U32 fast_tier = tier_for(fraction / HYSTERESIS);
        U32 tier = llclamp((U32)avatar->mFSAnimLODPeriod, 1u, sMaxPeriod);
        if (tier < slow_tier)
        {
            tier = slow_tier;
        }
        else if (tier > fast_tier)
        {
            tier = fast_tier;
        }
        avatar->mFSAnimLODPeriod = (U8)tier;
        period = tier;
        ++sTierCount[tier];
    }

    if (period > 1)
    {
        // Spread the throttled avatars over the frames by their ID, so the poses don't all come
        // due on the same frame
        const U32 phase = (U32)avatar->getID().mData[3] + (U32)avatar->getID().mData[11];
        const bool due = (sFrame + phase) % period == 0;
        if (!due && sNow - avatar->mFSAnimLODLastPose < MAX_POSE_AGE)
        {
            ++sSkipped;
            return true;
        }
    }

    avatar->mFSAnimLODLastPose = sNow;
    if (avatar->mFSAnimLODWarmup < 255)
    {
        ++avatar->mFSAnimLODWarmup;
    }
    ++sPosed;
    return false;
}

//static
void FSAnimationLOD::logStats()
{
    static LLCachedControl<F32> interval(gSavedSettings, "FSPerfProfileLogInterval", 0.f);
    static LLCachedControl<bool> enabled(gSavedSettings, "FSPerfAnimationLOD", false);
    if (interval <= 0.f || !enabled)
    {
        for (U64& count : sTierCount)
        {
            count = 0;
        }
        sSkipped = 0;
        sPosed = 0;
        sStatFrames = 0;
        sStatTimer.reset();
        return;
    }
    ++sStatFrames;
    const F64 elapsed = sStatTimer.getElapsedTimeF64();
    if (elapsed < (F64)llmax((F32)interval, 2.f))
    {
        return;
    }
    const F64 frames = (F64)llmax(sStatFrames, 1u);
    const U64 total = sSkipped + sPosed;
    LL_INFOS("FSAnimationLOD") << "Animation detail, avatars posed per frame on average: "
        << llformat("%.1f always every frame, %.1f every frame, %.1f every 2nd, %.1f every 3rd, %.1f every 4th; ",
                    (F64)sTierCount[0] / frames, (F64)sTierCount[1] / frames, (F64)sTierCount[2] / frames,
                    (F64)sTierCount[3] / frames, (F64)sTierCount[4] / frames)
        << llformat("%.0f poses skipped a second (%.0f%% of visible avatar poses)%s",
                    (F64)sSkipped / elapsed, total ? 100.0 * (F64)sSkipped / (F64)total : 0.0,
                    sActive ? "" : ", paused (mouselook or a snapshot window open)")
        << LL_ENDL;
    for (U64& count : sTierCount)
    {
        count = 0;
    }
    sSkipped = 0;
    sPosed = 0;
    sStatFrames = 0;
    sStatTimer.reset();
}
