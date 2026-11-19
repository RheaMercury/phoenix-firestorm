/**
 * @file fsobjectupdatedecoder.cpp
 * @brief Decodes cached object updates on worker threads ahead of their creation on the main thread
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

#include "fsobjectupdatedecoder.h"

#include "lldatapacker.h"
#include "llpartdata.h"
#include "llprimitive.h"
#include "llviewercontrol.h"
#include "llvolume.h"
#include "llvolumemessage.h"
#include "workqueue.h"

#include <atomic>
#include <cstring>
#include <vector>

namespace
{
    // Entries kept submitted ahead of the one being created, and how many go to a worker at once. Decoding
    // is many times quicker than creating an object, so a small window keeps the workers ahead; it is
    // refilled when half of it has been used, so posting stays a small part of the main thread's work.
    constexpr size_t LOOKAHEAD = 48;
    constexpr size_t CHUNK_SIZE = 12;
    // Objects this call must have created before anything is submitted: a call that creates one or two
    // gains nothing, and entries that wait every frame without ever being created (derendered ones, say)
    // would otherwise be copied and decoded again every frame.
    constexpr S32 MIN_CREATED = 2;

    // Same as MAX_OBJECT_PARAMS_SIZE in llviewerobject.cpp: the buffer the extra parameters are read into
    constexpr S32 PARAMS_BLOCK_SIZE = 1024;

    enum : U8
    {
        JOB_QUEUED = 0,
        JOB_BUSY,       // a worker is decoding it
        JOB_DONE,
        JOB_SKIPPED     // the main thread got there first and parsed it itself
    };

    LLTrace::BlockTimerStatHandle FTM_FS_OBJECT_DECODE_SUBMIT("FS Object Decode Submit");
}

struct FSObjectUpdateDecoder::Job
{
    const LLVOCacheEntry* mEntry = nullptr; // only compared, never followed
    std::vector<U8> mBytes;
    std::atomic<U8> mState{ JOB_QUEUED };

    // Written by the worker before JOB_DONE, then only used by the main thread
    bool mUsable = false;
    S32 mVolumeStart = -1;
    S32 mVolumeEnd = -1;
    bool mVolumeOK = false;
    LLVolumeParams mVolumeParams;
    S32 mTEStart = -1;
    S32 mTEEnd = -1;
    S32 mTEStatus = 0;
    std::unique_ptr<LLTEContents> mTE;
};

struct FSObjectUpdateDecoder::Chunk
{
    std::vector<std::unique_ptr<Job>> mJobs;
    std::shared_ptr<std::atomic<bool>> mCancelled;
};

FSObjectUpdateDecoder::Batch* FSObjectUpdateDecoder::sBatch = nullptr;
FSObjectUpdateDecoder::Job* FSObjectUpdateDecoder::sCurrentJob = nullptr;
LLDataPackerBinaryBuffer* FSObjectUpdateDecoder::sCurrentDP = nullptr;

namespace
{
    // Walks a cache entry's data the way LLViewerObjectList::processObjectUpdateFromCache,
    // LLViewerObject::processUpdateMessage (OUT_FULL_CACHED) and LLVOVolume::processUpdateMessage read it,
    // with the same data packer calls, up to the end of the texture entry block, and keeps the volume
    // parameters and the parsed texture entry. Only volumes are decoded; anything unusual is left to the
    // main thread. Uses nothing but the bytes.
    bool decode_entry(FSObjectUpdateDecoder::Job& job)
    {
        if (job.mBytes.empty())
        {
            return false;
        }
        LLDataPackerBinaryBuffer dp(job.mBytes.data(), (S32)job.mBytes.size());

        // processObjectUpdateFromCache
        LLUUID id;
        U32 local_id = 0;
        LLPCode pcode = 0;
        dp.unpackUUID(id, "ID");
        dp.unpackU32(local_id, "LocalID");
        dp.unpackU8(pcode, "PCode");
        if (pcode != LL_PCODE_VOLUME)
        {
            return false;
        }

        // LLViewerObject::processUpdateMessage, compressed and cached full update
        U8 state = 0;
        dp.unpackU8(state, "State");
        U32 crc = 0;
        dp.unpackU32(crc, "CRC");
        U8 material = 0;
        dp.unpackU8(material, "Material");
        U8 click_action = 0;
        dp.unpackU8(click_action, "ClickAction");
        LLVector3 vec;
        dp.unpackVector3(vec, "Scale");
        dp.unpackVector3(vec, "Pos");
        dp.unpackVector3(vec, "Rot");

        U32 value = 0;
        dp.unpackU32(value, "SpecialCode");
        LLUUID owner_id;
        dp.unpackUUID(owner_id, "Owner");

        if (value & 0x80)
        {
            dp.unpackVector3(vec, "Omega");
        }
        if (value & 0x20)
        {
            U32 parent_id = 0;
            dp.unpackU32(parent_id, "ParentID");
        }

        if (value & 0x2)
        {
            U8 tree_data = 0;
            dp.unpackU8(tree_data, "TreeData");
        }
        else if (value & 0x1)
        {
            U32 size = 0;
            dp.unpackU32(size, "ScratchPadSize");
            if (size > (U32)dp.getBufferSize())
            {
                return false; // can't be right; the main thread deals with it as before
            }
            std::vector<U8> scratch_pad(size + 1);
            S32 sp_size = 0;
            dp.unpackBinaryData(scratch_pad.data(), (S32)size, sp_size, "PartData");
        }

        if (value & 0x4)
        {
            std::string text;
            dp.unpackString(text, "Text");
            LLColor4U coloru;
            dp.unpackBinaryDataFixed(coloru.mV, 4, "Color");
        }

        if (value & 0x200)
        {
            std::string media_url;
            dp.unpackString(media_url, "MediaURL");
        }

        if (value & 0x8)
        {
            // LLViewerPartSourceScript::unpackPSS, legacy: the same reads whether the source exists or not
            LLPartSysData part_sys_data;
            part_sys_data.unpackLegacy(dp);
        }

        U8 num_parameters = 0;
        dp.unpackU8(num_parameters, "num_params");
        U8 param_block[PARAMS_BLOCK_SIZE];
        for (U8 param = 0; param < num_parameters; ++param)
        {
            U16 param_type = 0;
            S32 param_size = 0;
            dp.unpackU16(param_type, "param_type");
            dp.unpackBinaryData(param_block, PARAMS_BLOCK_SIZE, param_size, "param_data");
        }

        if (value & 0x10)
        {
            LLUUID sound_uuid;
            F32 gain = 0.f;
            U8 sound_flags = 0;
            F32 cutoff = 0.f;
            dp.unpackUUID(sound_uuid, "SoundUUID");
            dp.unpackF32(gain, "SoundGain");
            dp.unpackU8(sound_flags, "SoundFlags");
            dp.unpackF32(cutoff, "SoundRadius");
        }

        if (value & 0x100)
        {
            std::string name_value_list;
            dp.unpackString(name_value_list, "NV");
        }

        // LLVOVolume::processUpdateMessage: the volume parameters, then right after them the texture entry
        job.mVolumeStart = dp.getCurrentSize();
        job.mVolumeOK = LLVolumeMessage::unpackVolumeParams(&job.mVolumeParams, dp);
        job.mVolumeEnd = dp.getCurrentSize();

        job.mTEStart = job.mVolumeEnd;
        job.mTE = std::make_unique<LLTEContents>();
        job.mTEStatus = LLPrimitive::fsParseTEMessage(dp, *job.mTE);
        job.mTEEnd = dp.getCurrentSize();

        return true;
    }
}

// static
void FSObjectUpdateDecoder::decodeChunk(const std::shared_ptr<Chunk>& chunk)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_NETWORK;
    for (auto& job : chunk->mJobs)
    {
        if (chunk->mCancelled->load(std::memory_order_relaxed))
        {
            return;
        }
        U8 expected = JOB_QUEUED;
        if (!job->mState.compare_exchange_strong(expected, JOB_BUSY, std::memory_order_acq_rel))
        {
            continue;
        }
        job->mUsable = decode_entry(*job);
        job->mState.store(JOB_DONE, std::memory_order_release);
    }
}

FSObjectUpdateDecoder::Batch::Batch()
{
    static LLCachedControl<bool> threaded_object_updates(gSavedSettings, "FSPerfThreadedObjectUpdates", false);
    if (!threaded_object_updates)
    {
        return;
    }
    mActive = true;
    mCancelled = std::make_shared<std::atomic<bool>>(false);
    mPrevious = sBatch;
    sBatch = this;
}

FSObjectUpdateDecoder::Batch::~Batch()
{
    if (!mActive)
    {
        return;
    }
    // Whatever the workers haven't started is of no use any more. A chunk still being decoded keeps
    // itself alive and only touches its own copies.
    mCancelled->store(true, std::memory_order_relaxed);
    sBatch = mPrevious;
}

void FSObjectUpdateDecoder::Batch::lookAhead(iterator_t current, iterator_t end)
{
    // Count the previous entry if the loop turned it into an object
    if (mLast && mLast->getState() >= LLVOCacheEntry::WAITING)
    {
        ++mCreated;
    }
    mLast = (*current)->getState() < LLVOCacheEntry::WAITING ? *current : nullptr;
    if (mCreated < MIN_CREATED)
    {
        ++mPos;
        return;
    }

    // Drop what the loop has gone past without creating it from cache, so the window keeps moving
    while (!mQueue.empty() && mQueue.front().mPos < mPos)
    {
        U8 expected = JOB_QUEUED;
        mQueue.front().mJob->mState.compare_exchange_strong(expected, JOB_SKIPPED, std::memory_order_acq_rel);
        mQueue.pop_front();
    }
    if (!mAheadSet || mAheadPos <= mPos)
    {
        mAhead = current;
        ++mAhead;
        mAheadPos = mPos + 1;
        mAheadSet = true;
    }
    if (mQueue.size() < LOOKAHEAD / 2)
    {
        LL_RECORD_BLOCK_TIME(FTM_FS_OBJECT_DECODE_SUBMIT);
        for (; mAhead != end && mQueue.size() < LOOKAHEAD; ++mAhead, ++mAheadPos)
        {
            LLVOCacheEntry* entry = *mAhead;
            // The ones LLViewerRegion::addNewObject will turn into an object from cache
            if (entry->getState() < LLVOCacheEntry::WAITING && entry->getEntry() && !entry->getEntry()->hasDrawable())
            {
                add(entry, mAheadPos);
            }
        }
        post();
    }
    ++mPos;
}

void FSObjectUpdateDecoder::Batch::add(const LLVOCacheEntry* entry, S32 pos)
{
    LLDataPackerBinaryBuffer* dp = const_cast<LLVOCacheEntry*>(entry)->getDP();
    if (!dp || !dp->getBuffer() || dp->getBufferSize() <= 0)
    {
        return;
    }
    if (!mPending)
    {
        mPending = std::make_shared<Chunk>();
        mPending->mCancelled = mCancelled;
        mPending->mJobs.reserve(CHUNK_SIZE);
    }
    auto job = std::make_unique<Job>();
    job->mEntry = entry;
    job->mBytes.assign(dp->getBuffer(), dp->getBuffer() + dp->getBufferSize());
    mQueue.push_back({ mPending, job.get(), pos });
    mPending->mJobs.push_back(std::move(job));
    if (mPending->mJobs.size() >= CHUNK_SIZE)
    {
        post();
    }
}

void FSObjectUpdateDecoder::Batch::post()
{
    if (!mPending)
    {
        return;
    }
    // Nothing is added to a chunk once it is posted
    std::shared_ptr<Chunk> chunk = std::move(mPending);
    mPending.reset();
    // a pool of their own when it was made at startup (LLAppViewer::initGeneralThread), else "General"
    LL::WorkQueue::ptr_t queue = LL::WorkQueue::getInstance("FSWorker");
    if (!queue)
    {
        queue = LL::WorkQueue::getInstance("General");
    }
    if (queue)
    {
        // When the pool is gone (logging out) or refuses it, the jobs stay queued and the main thread
        // parses those entries itself as before
        queue->post([chunk]() { decodeChunk(chunk); });
    }
}

FSObjectUpdateDecoder::Job* FSObjectUpdateDecoder::Batch::take(const LLVOCacheEntry* entry, const LLDataPackerBinaryBuffer* dp,
                                                               std::shared_ptr<Chunk>& hold)
{
    for (size_t i = 0; i < mQueue.size(); ++i)
    {
        if (mQueue[i].mJob->mEntry != entry)
        {
            continue;
        }

        // The loop has gone past the entries submitted before this one
        for (size_t k = 0; k < i; ++k)
        {
            U8 expected = JOB_QUEUED;
            mQueue.front().mJob->mState.compare_exchange_strong(expected, JOB_SKIPPED, std::memory_order_acq_rel);
            mQueue.pop_front();
        }

        Ref ref = mQueue.front();
        mQueue.pop_front();
        Job* job = ref.mJob;

        U8 state = JOB_QUEUED;
        if (job->mState.compare_exchange_strong(state, JOB_SKIPPED, std::memory_order_acq_rel))
        {
            return nullptr; // the workers didn't get to it: parsed here as before
        }
        if (state != JOB_DONE || !job->mUsable)
        {
            return nullptr; // still being decoded, or left to the main thread
        }
        // Only when the entry still holds exactly the bytes that were decoded
        if (!dp || dp->getBufferSize() != (S32)job->mBytes.size()
            || memcmp(dp->getBuffer(), job->mBytes.data(), job->mBytes.size()) != 0)
        {
            return nullptr;
        }
        hold = ref.mChunk;
        return job;
    }
    return nullptr;
}

void FSObjectUpdateDecoder::ApplyScope::begin(const LLVOCacheEntry* entry, LLDataPackerBinaryBuffer* dp)
{
    mSet = true;
    mPrevJob = sCurrentJob;
    mPrevDP = sCurrentDP;
    // Nested updates of other entries see their own decode or none, never this one's
    sCurrentJob = sBatch->take(entry, dp, mHold);
    sCurrentDP = sCurrentJob ? dp : nullptr;
}

void FSObjectUpdateDecoder::ApplyScope::end()
{
    sCurrentJob = mPrevJob;
    sCurrentDP = mPrevDP;
}

// static
bool FSObjectUpdateDecoder::takeVolumeParamsImpl(LLDataPacker* dp, LLVolumeParams& params, bool& ok)
{
    Job* job = sCurrentJob;
    if (!sCurrentDP || dp != sCurrentDP || job->mVolumeStart < 0 || sCurrentDP->getCurrentSize() != job->mVolumeStart)
    {
        return false;
    }
    LL_PROFILE_ZONE_SCOPED_CATEGORY_NETWORK;
    params = job->mVolumeParams;
    ok = job->mVolumeOK;
    sCurrentDP->shift(job->mVolumeEnd);
    job->mVolumeStart = -1; // used once
    return true;
}

// static
bool FSObjectUpdateDecoder::applyTEImpl(LLPrimitive* prim, LLDataPacker* dp, S32& result)
{
    Job* job = sCurrentJob;
    if (!sCurrentDP || dp != sCurrentDP || job->mTEStart < 0 || sCurrentDP->getCurrentSize() != job->mTEStart)
    {
        return false;
    }
    LL_PROFILE_ZONE_SCOPED_CATEGORY_NETWORK;
    sCurrentDP->shift(job->mTEEnd);
    job->mTEStart = -1; // used once
    if (job->mTEStatus != 1 || !job->mTE)
    {
        result = job->mTEStatus; // empty block (0) or TEM_INVALID, as unpackTEMessage returns them
        return true;
    }
    // The faces unpackTEMessage(LLDataPacker&) would have parsed and set, in the same order
    job->mTE->face_count = llmin((U32)prim->getNumTEs(), (U32)LLTEContents::MAX_TES);
    result = prim->applyParsedTEMessage(*job->mTE);
    return true;
}
