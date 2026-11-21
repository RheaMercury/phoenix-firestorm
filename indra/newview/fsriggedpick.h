/**
 * @file fsriggedpick.h
 * @brief Cheaper picking of rigged meshes (FSPerfFastRiggedPick)
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

#ifndef FS_RIGGEDPICK_H
#define FS_RIGGEDPICK_H

#include "llmatrix4a.h"
#include "llvector4a.h"

#include <vector>

class LLMeshSkinInfo;
class LLVolume;

// A pick through a rigged mesh (a right click on an avatar, the edit tools on a worn mesh) skinned every
// vertex of every face of every rigged mesh along the ray, on the main thread, for each pick: 300 ms and
// more for a right click over a few detailed avatars (four picks). With FSPerfFastRiggedPick on, each
// LLRiggedVolume keeps one of these:
//  - Faces the ray can't touch are left alone. A skinned vertex is a weighted average (the weights add up
//    to one) of the joints' transforms of its bind position, so it lies in the box around the face's bind
//    box moved by each joint that weighs on the face. That box is a few multiplies per joint; only faces
//    whose box the ray crosses are skinned.
//  - A face already skinned for the same pose (the same joint matrices, skin and source volume) is not
//    skinned again: the four picks of a right click, a pick in the frame after a still pose, a paused
//    animation.
//  - While FSRiggedPickCache::HoverScope is alive (the mouse-over pick), a face skinned less than
//    HOVER_RESKIN_SECONDS ago is used as it is, even if the pose moved since: hovering over a worn rigged
//    mesh with the build tools open skins it at most a few times a second. Clicks always skin the pose of
//    the moment.
class FSRiggedPickCache
{
public:
    static bool enabled();

    // The rigged volume's faces were made again from the source volume: nothing skinned is current
    void reset();

    // The pose of this update: the palette LLRiggedVolume::update built (count matrices) with the skin and
    // the source volume. Returns its key, never 0.
    U64 notePose(const LLMatrix4a* palette, U32 count, const LLMatrix4a& bind_shape, const LLMeshSkinInfo* skin,
                 const LLVolume* src);
    // Face skinned for this pose already (or, hovering, recently enough)
    bool isSkinned(S32 face, U64 pose) const;
    void noteSkinned(S32 face, U64 pose, bool octree_built);
    // When a face is used as it is but an octree for it is wanted: whether the one it has was built for its
    // current positions
    bool octreeCurrent(S32 face) const;
    void noteOctreeBuilt(S32 face);

    // True when the segment (in the rigged volume's space) can't touch face of src under the pose last noted.
    // False whenever that can't be told (weights out of the palette, nothing noted).
    bool canSkip(S32 face, const LLVolume* src, const LLVector4a& start, const LLVector4a& end);

    // The mouse-over pick
    class HoverScope
    {
    public:
        HoverScope();
        ~HoverScope();
        HoverScope(const HoverScope&) = delete;
        HoverScope& operator=(const HoverScope&) = delete;
    private:
        bool mWas;
    };

private:
    struct FaceState
    {
        LLVector4a mBindCenter;     // the face's box after the bind shape matrix: centre and half size
        LLVector4a mBindHalf;
        U64 mJoints[2] = { 0, 0 };  // the palette entries that weigh on it
        U64 mPose = 0;              // pose its positions were skinned for, 0 when none
        U64 mOctreePose = 0;        // pose its octree was built for
        F64 mSkinnedAt = 0.0;
        bool mInfo = false;         // mBind*, mJoints filled in
        bool mBoundable = false;    // the box can be worked out (every weight in the palette)
    };
    FaceState& face(S32 i);
    void buildInfo(FaceState& state, S32 i, const LLVolume* src);

    std::vector<FaceState> mFaces;
    std::vector<LLMatrix4a> mPalette;   // of the last noted pose
    LLMatrix4a mBindShape;
    U64 mPose = 0;
    const LLMeshSkinInfo* mSkin = nullptr;  // compared only, never dereferenced
    const LLVolume* mSrc = nullptr;         // compared only, never dereferenced
};

#endif // FS_RIGGEDPICK_H
