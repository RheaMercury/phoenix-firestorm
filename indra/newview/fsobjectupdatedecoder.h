/**
 * @file fsobjectupdatedecoder.h
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

#ifndef FS_OBJECTUPDATEDECODER_H
#define FS_OBJECTUPDATEDECODER_H

#include "llvocache.h"

#include <deque>
#include <memory>

class LLDataPacker;
class LLDataPackerBinaryBuffer;
class LLPrimitive;
class LLVolumeParams;

// FSPerfThreadedObjectUpdates
//
// Most objects of a region are created from its object cache: full updates the simulator marks as
// cacheable, cache hits and entries read from disk all become LLVOCacheEntry data, and
// LLViewerRegion::createVisibleObjects turns the visible ones into objects, in priority order, under a
// time budget. While it creates one entry, the entries coming up next are decoded on the General
// thread pool, from copies of their bytes: the walk to the volume parameters, the volume parameters
// themselves and the texture entry block, which is the part of the decode with real cost (a 4 KB
// buffer, eleven run length coded fields). LLVOVolume::processUpdateMessage then takes those results at
// the points where it would have parsed them, and applies them exactly as before.
//
// Nothing is deferred: every object is still created and updated by the stock code, at the stock point
// in the frame, in the stock order. A decode is only used when the entry's bytes are still the ones that
// were decoded and the data packer stands exactly where the worker's walk said the block starts;
// otherwise, or when the worker hasn't got to it yet, the main thread parses it itself as before.
// Workers only ever see their own copies of the bytes: no object, region or cache entry, no message
// system, no global list.
class FSObjectUpdateDecoder
{
public:
    struct Chunk;
    struct Job;

    // One LLViewerRegion::createVisibleObjects call. Inactive (and costs nothing) with the setting off.
    class Batch
    {
    public:
        typedef LLVOCacheEntry::vocache_entry_priority_list_t::iterator iterator_t;

        Batch();
        ~Batch();

        bool active() const { return mActive; }

        // Called for each entry the loop looks at, before creating it: keeps the entries after it that
        // still wait for creation submitted to the workers.
        void lookAhead(iterator_t current, iterator_t end);

        // The decode of this entry, when it is done and still matches its bytes; drops the entries
        // submitted before it, which the loop has passed.
        Job* take(const LLVOCacheEntry* entry, const LLDataPackerBinaryBuffer* dp, std::shared_ptr<Chunk>& hold);

    private:
        void add(const LLVOCacheEntry* entry, S32 pos);
        void post();

        struct Ref
        {
            std::shared_ptr<Chunk> mChunk;
            Job* mJob;
            S32 mPos; // the entry's place in the loop
        };

        bool mActive = false;
        Batch* mPrevious = nullptr;
        std::shared_ptr<std::atomic<bool>> mCancelled;
        std::shared_ptr<Chunk> mPending;
        std::deque<Ref> mQueue;
        const LLVOCacheEntry* mLast = nullptr;
        S32 mCreated = 0;
        iterator_t mAhead;
        bool mAheadSet = false;
        S32 mPos = 0;
        S32 mAheadPos = 0;
    };

    // In LLViewerObjectList::processObjectUpdateFromCache, around the update of one entry: makes its
    // decode, if any, what LLVOVolume::processUpdateMessage takes from this data packer.
    class ApplyScope
    {
    public:
        ApplyScope(const LLVOCacheEntry* entry, LLDataPackerBinaryBuffer* dp)
        {
            if (sBatch) // never set with the setting off
            {
                begin(entry, dp);
            }
        }
        ~ApplyScope()
        {
            if (mSet)
            {
                end();
            }
        }

    private:
        void begin(const LLVOCacheEntry* entry, LLDataPackerBinaryBuffer* dp);
        void end();

        bool mSet = false;
        Job* mPrevJob = nullptr;
        LLDataPackerBinaryBuffer* mPrevDP = nullptr;
        std::shared_ptr<Chunk> mHold;
    };

    // LLVOVolume::processUpdateMessage: false when there is nothing decoded for this data packer at its
    // current position, and the caller parses as before. On true the packer has been moved past the block,
    // exactly as the parse would have left it.
    static bool takeVolumeParams(LLDataPacker* dp, LLVolumeParams& params, bool& ok)
    {
        return sCurrentJob && takeVolumeParamsImpl(dp, params, ok);
    }
    static bool applyTE(LLPrimitive* prim, LLDataPacker* dp, S32& result)
    {
        return sCurrentJob && applyTEImpl(prim, dp, result);
    }

private:
    static bool takeVolumeParamsImpl(LLDataPacker* dp, LLVolumeParams& params, bool& ok);
    static bool applyTEImpl(LLPrimitive* prim, LLDataPacker* dp, S32& result);
    static void decodeChunk(const std::shared_ptr<Chunk>& chunk);

    static Batch* sBatch;
    static Job* sCurrentJob;
    static LLDataPackerBinaryBuffer* sCurrentDP;
};

#endif // FS_OBJECTUPDATEDECODER_H
