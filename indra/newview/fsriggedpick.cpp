/**
 * @file fsriggedpick.cpp
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

#include "llviewerprecompiledheaders.h"

#include "fsriggedpick.h"

#include "hbxxh.h"
#include "llframetimer.h"
#include "llskinningutil.h"
#include "llviewercontrol.h"
#include "llvolume.h"

namespace
{
    // A hovered rigged face is skinned again at most this often while the pose moves
    constexpr F64 HOVER_RESKIN_SECONDS = 0.1;
    // The box a face can be in is widened by this much (metres) and this share of its size, for rounding
    constexpr F32 BOX_SLACK = 0.001f;
    constexpr F32 BOX_SLACK_SHARE = 0.001f;
    constexpr U32 MAX_PALETTE = 128;  // two U64 masks (LL_MAX_JOINTS_PER_MESH_OBJECT is 110)

    bool sHovering = false; // main thread
}

// static
bool FSRiggedPickCache::enabled()
{
    static LLCachedControl<bool> fast_pick(gSavedSettings, "FSPerfFastRiggedPick", false);
    return fast_pick;
}

FSRiggedPickCache::HoverScope::HoverScope()
    : mWas(sHovering)
{
    sHovering = true;
}

FSRiggedPickCache::HoverScope::~HoverScope()
{
    sHovering = mWas;
}

void FSRiggedPickCache::reset()
{
    mFaces.clear();
    mPose = 0;
    mSkin = nullptr;
    mSrc = nullptr;
}

FSRiggedPickCache::FaceState& FSRiggedPickCache::face(S32 i)
{
    if ((size_t)i >= mFaces.size())
    {
        mFaces.resize((size_t)i + 1);
    }
    return mFaces[i];
}

U64 FSRiggedPickCache::notePose(const LLMatrix4a* palette, U32 count, const LLMatrix4a& bind_shape, const LLMeshSkinInfo* skin,
                                const LLVolume* src)
{
    if (skin != mSkin || src != mSrc)
    {   // another skin or source volume: the faces' boxes and joints are someone else's
        mFaces.clear();
        mSkin = skin;
        mSrc = src;
    }
    count = llmin(count, MAX_PALETTE);
    mPalette.assign(palette, palette + count);
    mBindShape = bind_shape;

    HBXXH64 hash;
    hash.update(palette, sizeof(LLMatrix4a) * count);
    hash.update(&bind_shape, sizeof(LLMatrix4a));
    hash.update(&skin, sizeof(skin));
    hash.update(&src, sizeof(src));
    mPose = hash.digest();
    if (!mPose)
    {
        mPose = 1;
    }
    return mPose;
}

bool FSRiggedPickCache::isSkinned(S32 i, U64 pose) const
{
    if (i < 0 || (size_t)i >= mFaces.size())
    {
        return false;
    }
    const FaceState& state = mFaces[i];
    if (!state.mPose)
    {
        return false;
    }
    if (state.mPose == pose)
    {
        return true;
    }
    return sHovering && LLFrameTimer::getElapsedSeconds() - state.mSkinnedAt < HOVER_RESKIN_SECONDS;
}

void FSRiggedPickCache::noteSkinned(S32 i, U64 pose, bool octree_built)
{
    if (i < 0)
    {
        return;
    }
    FaceState& state = face(i);
    state.mPose = pose;
    state.mSkinnedAt = LLFrameTimer::getElapsedSeconds();
    if (octree_built)
    {
        state.mOctreePose = pose;
    }
}

bool FSRiggedPickCache::octreeCurrent(S32 i) const
{
    return i >= 0 && (size_t)i < mFaces.size() && mFaces[i].mPose && mFaces[i].mOctreePose == mFaces[i].mPose;
}

void FSRiggedPickCache::noteOctreeBuilt(S32 i)
{
    if (i >= 0 && (size_t)i < mFaces.size())
    {
        mFaces[i].mOctreePose = mFaces[i].mPose;
    }
}

// Once per face and source volume: the face's bind box (its extents after the bind shape matrix) and the
// palette entries its weights use, decoded as FSSkinningUtil::getPerVertexSkinMatrixSSE does (the whole part
// of each weight is the joint, clamped to the most joints a mesh can have).
void FSRiggedPickCache::buildInfo(FaceState& state, S32 i, const LLVolume* src)
{
    state.mInfo = true;
    state.mBoundable = false;
    state.mJoints[0] = 0;
    state.mJoints[1] = 0;
    if (!src || i < 0 || i >= src->getNumVolumeFaces())
    {
        return;
    }
    const LLVolumeFace& vol_face = src->getVolumeFace(i);
    if (!vol_face.mWeights || !vol_face.mExtents || vol_face.mNumVertices <= 0)
    {
        return;
    }
    const S32 max_joint = LLSkinningUtil::getMaxJointCount() - 1;
    const S32 palette = (S32)mPalette.size();
    for (S32 v = 0; v < vol_face.mNumVertices; ++v)
    {
        const F32* w = vol_face.mWeights[v].getF32ptr();
        for (S32 k = 0; k < 4; ++k)
        {
            if (!(w[k] >= 0.f) || w[k] >= 32768.f)
            {
                return; // negative or not a number: the skinning's result can't be bounded
            }
            const S32 joint = llmin((S32)w[k], max_joint);
            if (joint >= palette)
            {
                return; // a joint the palette doesn't fill in
            }
            state.mJoints[joint >> 6] |= 1ull << (joint & 63);
        }
    }

    // the face's extents through the bind shape matrix (affine): centre and half size
    LLVector4a center;
    center.setAdd(vol_face.mExtents[0], vol_face.mExtents[1]);
    center.mul(0.5f);
    LLVector4a half;
    half.setSub(vol_face.mExtents[1], vol_face.mExtents[0]);
    half.mul(0.5f);
    mBindShape.affineTransform(center, state.mBindCenter);
    LLVector4a ax, ay, az;
    ax.setAbs(mBindShape.mMatrix[0]);
    ay.setAbs(mBindShape.mMatrix[1]);
    az.setAbs(mBindShape.mMatrix[2]);
    ax.mul(half.getF32ptr()[0]);
    ay.mul(half.getF32ptr()[1]);
    az.mul(half.getF32ptr()[2]);
    state.mBindHalf.setAdd(ax, ay);
    state.mBindHalf.add(az);
    state.mBoundable = state.mBindCenter.isFinite3() && state.mBindHalf.isFinite3();
}

bool FSRiggedPickCache::canSkip(S32 i, const LLVolume* src, const LLVector4a& start, const LLVector4a& end)
{
    if (!mPose || src != mSrc || i < 0 || mPalette.empty())
    {
        return false;
    }
    FaceState& state = face(i);
    if (!state.mInfo)
    {
        buildInfo(state, i, src);
    }
    if (!state.mBoundable)
    {
        return false;
    }

    // Each joint's transform of the bind box, joined
    LLVector4a box_min, box_max;
    bool any = false;
    const LLVector4a& c = state.mBindCenter;
    const F32* h = state.mBindHalf.getF32ptr();
    const U32 palette = (U32)mPalette.size();
    for (U32 joint = 0; joint < MAX_PALETTE; ++joint)
    {
        if (state.mJoints[joint >> 6] & (1ull << (joint & 63)))
        {
            if (joint >= palette)
            {
                return false;
            }
            const LLMatrix4a& m = mPalette[joint];
            LLVector4a center;
            m.affineTransform(c, center);
            LLVector4a ax, ay, az;
            ax.setAbs(m.mMatrix[0]);
            ay.setAbs(m.mMatrix[1]);
            az.setAbs(m.mMatrix[2]);
            ax.mul(h[0]);
            ay.mul(h[1]);
            az.mul(h[2]);
            LLVector4a half;
            half.setAdd(ax, ay);
            half.add(az);
            LLVector4a lo, hi;
            lo.setSub(center, half);
            hi.setAdd(center, half);
            if (any)
            {
                box_min.setMin(box_min, lo);
                box_max.setMax(box_max, hi);
            }
            else
            {
                box_min = lo;
                box_max = hi;
                any = true;
            }
        }
    }
    if (!any || !box_min.isFinite3() || !box_max.isFinite3())
    {
        return false;
    }

    LLVector4a center;
    center.setAdd(box_min, box_max);
    center.mul(0.5f);
    LLVector4a half;
    half.setSub(box_max, box_min);
    half.mul(0.5f);
    LLVector4a slack = half;
    slack.mul(BOX_SLACK_SHARE);
    LLVector4a fixed_slack;
    fixed_slack.splat(BOX_SLACK);
    half.add(slack);
    half.add(fixed_slack);
    // LLLineSegmentBoxIntersect takes half sizes
    return !LLLineSegmentBoxIntersect(start.getF32ptr(), end.getF32ptr(), center.getF32ptr(), half.getF32ptr());
}
