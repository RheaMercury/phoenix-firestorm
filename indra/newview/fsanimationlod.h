/**
 * @file fsanimationlod.h
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

#ifndef FS_ANIMATIONLOD_H
#define FS_ANIMATIONLOD_H

class LLVOAvatar;

// FSPerfAnimationLOD
//
// Every avatar drawn in full had its motions evaluated, its pose blended and its joint matrices
// recomputed every frame, however small it was on screen. With this on, an avatar's on-screen size
// picks how often its pose is evaluated: every frame, or every 2nd, 3rd or 4th frame
// (FSPerfAnimationLODScreenSize, FSPerfAnimationLODMaxPeriod).
//
// LLVOAvatar::updateCharacter() asks skipPose() for each visible avatar before its root update.
// On a skipped frame:
// - the root still follows the avatar's position and rotation (mRoot), so movement and vehicles
//   stay smooth; the last pose is carried along with it;
// - the motion clock keeps running (LLCharacter::HIDDEN_UPDATE, as for avatars out of view), so the
//   next evaluated pose is at the right point of every animation, and avatar physics simulate
//   the time in between in their own sub-steps;
// - a root that did not move is not touched, so no joint matrix is recomputed and the rigged
//   mesh palettes of the last pose are reused (see LLVOAvatar::updateSkinInfoMatrixPalette());
// - the pose is not queued for the pose workers (FSPerfParallelAvatarUpdate).
// Throttled avatars are spread over the frames by their ID, and no avatar goes longer than
// an eighth of a second without a pose at low frame rates.
class FSAnimationLOD
{
public:
    // Main thread, once a frame before the object idle loop
    static void beginFrame();

    // Main thread, from LLVOAvatar::updateCharacter() for a visible avatar. True if this frame's pose
    // evaluation is skipped.
    static bool skipPose(LLVOAvatar* avatar);

    // On this frame (the setting is on, not in mouselook or with a snapshot window open)
    static bool isActive() { return sActive; }

private:
    static bool isExempt(LLVOAvatar* avatar);
    static void logStats();

    static bool sActive;
};

#endif // FS_ANIMATIONLOD_H
