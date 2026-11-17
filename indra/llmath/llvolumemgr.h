/**
 * @file llvolumemgr.h
 * @brief LLVolumeMgr class.
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#ifndef LL_LLVOLUMEMGR_H
#define LL_LLVOLUMEMGR_H

#include <map>

#include "llvolume.h"
#include "llpointer.h"
#include "llthread.h"

class LLVolumeParams;
class LLVolumeLODGroup;

class LLVolumeLODGroup
{
    LOG_CLASS(LLVolumeLODGroup);

public:
    enum
    {
        NUM_LODS = 4
    };

    LLVolumeLODGroup(const LLVolumeParams &params);
    ~LLVolumeLODGroup();
    bool cleanupRefs();

    static S32 getDetailFromTan(const F32 tan_angle);
    static void getDetailProximity(const F32 tan_angle, F32 &to_lower, F32& to_higher);
    static F32 getVolumeScaleFromDetail(const S32 detail);
    static S32 getVolumeDetailFromScale(F32 scale);

    LLVolume* refLOD(const S32 detail);
    bool derefLOD(LLVolume *volumep);
    void fsReleaseVolumes(); // <FS:Trish/> FSPerfBackgroundVolumeFree, before the group is deleted
    // <FS:Trish> FSPerfThreadedGeometry
    // Whether this LOD's shape exists already (refLOD would not build it). Main thread.
    bool fsHasLOD(S32 lod) const { return lod >= 0 && lod < NUM_LODS && mVolumeLODs[lod].notNull(); }
    // When set, refLOD asks it for a shape built ahead of need (on a worker) before building one itself;
    // it returns null when it has none. Called on the main thread only, like refLOD.
    typedef LLPointer<LLVolume> (*fs_prebuilt_fn_t)(const LLVolumeParams& params, S32 lod);
    static fs_prebuilt_fn_t sFSPrebuiltLOD;
    // </FS:Trish>
    S32 getNumRefs() const { return mRefs; }

    const LLVolumeParams* getVolumeParams() const { return &mVolumeParams; };

    F32 dump();
    friend std::ostream& operator<<(std::ostream& s, const LLVolumeLODGroup& volgroup);

protected:
    // <FS:Trish> its entry in LLVolumeMgr::mVolumeLODGroups, erased without a lookup (unrefVolume)
    friend class LLVolumeMgr;
    std::map<const LLVolumeParams*, LLVolumeLODGroup*, LLVolumeParams::compare>::iterator mFSMapIter;
    bool mFSInMap = false;
    // </FS:Trish>

    LLVolumeParams mVolumeParams;

    S32 mRefs;
    S32 mLODRefs[NUM_LODS];
    LLPointer<LLVolume> mVolumeLODs[NUM_LODS];
    static F32 mDetailThresholds[NUM_LODS];
    static F32 mDetailScales[NUM_LODS];
    S32     mAccessCount[NUM_LODS];
};

class LLVolumeMgr
{
public:
    LLVolumeMgr();
    virtual ~LLVolumeMgr();
    bool cleanup();         // Cleanup all volumes being managed, returns true if no dangling references

    virtual LLVolumeLODGroup* getGroup( const LLVolumeParams& volume_params ) const;

    // whatever calls getVolume() never owns the LLVolume* and
    // cannot keep references for long since it may be deleted
    // later.  For best results hold it in an LLPointer<LLVolume>.
    virtual LLVolume *refVolume(const LLVolumeParams &volume_params, const S32 detail);
    virtual void unrefVolume(LLVolume *volumep);

    void dump();

    // manually call this for mutex magic
    void useMutex();

    friend std::ostream& operator<<(std::ostream& s, const LLVolumeMgr& volume_mgr);

protected:
    void insertGroup(LLVolumeLODGroup* volgroup);
    // Overridden in llphysics/abstract/utils/llphysicsvolumemanager.h
    virtual LLVolumeLODGroup* createNewGroup(const LLVolumeParams& volume_params);

protected:
    typedef std::map<const LLVolumeParams*, LLVolumeLODGroup*, LLVolumeParams::compare> volume_lod_group_map_t;
    volume_lod_group_map_t mVolumeLODGroups;

    LLMutex* mDataMutex;
};

#endif // LL_LLVOLUMEMGR_H
