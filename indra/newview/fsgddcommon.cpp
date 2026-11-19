/**
 * @file fsgddcommon.cpp
 * @brief The GPU-driven passes' upload ring (FSPerfGPUDrivenUploadRing) and shared matrix tables
 * (FSPerfGPUDrivenSharedMatrices)
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

#include "fsgddcommon.h"

#include "fsglstream.h"
#include "llviewercontrol.h"

#include <cstring>

LLTrace::BlockTimerStatHandle FTM_FS_GDD_BUILD("FS GDD Build");
LLTrace::BlockTimerStatHandle FTM_FS_GDD_UPLOAD("FS GDD Upload");

U64 gFSGDDStatModelsComputed = 0;
U64 gFSGDDStatModelsFound = 0;

//-----------------------------------------------------------------------------
// The upload ring. Three slots, one per frame in turn, each a buffer the frame's uploads take consecutive
// slices of. A slot is written again three frames later:
//  - persistent (OpenGL 4.4, the main thread makes the GL calls): the buffers are mapped once, coherent, and
//    written with memcpy; a fence after each frame's commands, waited on (rarely: three frames is far) before the
//    slot is written again.
//  - sub data (the render thread replays the GL calls, or no glBufferStorage): glBufferSubData into the slice. The
//    data is copied into the command stream when recorded and lands in order, so no fence is needed; a buffer the
//    GPU may still read three frames later is the driver's to keep.
// A slot that runs out of room mid frame gets a buffer twice the size; the old one stays (its slices are still read
// by the frame's draws, and may still be written by writeAt) until the slot comes round again.
namespace
{
    constexpr U32 RING_SLOTS = 3;
    constexpr size_t RING_FIRST_BYTES = 4u << 20;
    constexpr size_t RING_MAX_BYTES = 256u << 20;

    enum ERingMode
    {
        RING_NONE,
        RING_PERSISTENT,
        RING_SUB_DATA
    };

    struct RingBuffer
    {
        GLuint mName = 0;
        U8* mMapped = nullptr;
    };

    struct RingSlot
    {
        RingBuffer mBuffer;
        size_t mCapacity = 0;
        size_t mUsed = 0;
        GLsync mFence = nullptr;
        std::vector<RingBuffer> mRetired;   // outgrown: deleted when the slot comes round again
    };

    struct RingSlice
    {
        GLuint mName = 0;
        size_t mOffset = 0;
        size_t mSize = 0;
        U8* mMapped = nullptr;
    };

    RingSlot sSlots[RING_SLOTS];
    U32 sSlot = 0;
    U32 sRingFrame = 0;
    U32 sRingGeneration = 1;            // changes at each release, so slices of a released ring are never written
    bool sRingStarted = false;
    ERingMode sRingMode = RING_NONE;
    bool sPersistentFailed = false;     // a mapping failed: sub data from then on
    size_t sAlign = 256;

    // statistics, every 10 seconds while either setting is on
    U64 sStatRingSlices = 0;
    U64 sStatRingBytes = 0;
    U64 sStatOwnUploads = 0;
    U64 sStatOwnBytes = 0;
    U64 sStatTailWrites = 0;
    U64 sStatGrowths = 0;
    U64 sStatFenceWaits = 0;

    bool persistent_supported()
    {
        return gGLManager.mGLVersion >= 4.39f &&
               glBufferStorage != nullptr &&
               glMapBufferRange != nullptr &&
               glFenceSync != nullptr &&
               glClientWaitSync != nullptr &&
               glDeleteSync != nullptr;
    }

    // With the render thread recording, a mapped write would land before the commands recorded ahead of it run:
    // the stream's order only holds for GL calls
    ERingMode wanted_mode()
    {
        if (FSGLStream::isRecording() || sPersistentFailed || !persistent_supported())
        {
            return RING_SUB_DATA;
        }
        return RING_PERSISTENT;
    }

    void free_ring_buffer(RingBuffer& buffer)
    {
        if (buffer.mName)
        {
            glDeleteBuffers(1, &buffer.mName);  // unmaps it too
        }
        buffer = RingBuffer();
    }

    void release_ring()
    {
        for (RingSlot& slot : sSlots)
        {
            if (slot.mFence)
            {
                glDeleteSync(slot.mFence);
                slot.mFence = nullptr;
            }
            free_ring_buffer(slot.mBuffer);
            for (RingBuffer& retired : slot.mRetired)
            {
                free_ring_buffer(retired);
            }
            slot.mRetired.clear();
            slot.mCapacity = 0;
            slot.mUsed = 0;
        }
        sSlot = 0;
        sRingStarted = false;
        sRingMode = RING_NONE;
        if (++sRingGeneration == 0)
        {
            sRingGeneration = 1;
        }
    }

    bool create_ring_buffer(RingBuffer& buffer, size_t bytes)
    {
        glGenBuffers(1, &buffer.mName);
        glBindBuffer(GL_COPY_WRITE_BUFFER, buffer.mName);
        if (sRingMode == RING_PERSISTENT)
        {
            const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
            glBufferStorage(GL_COPY_WRITE_BUFFER, (GLsizeiptr)bytes, nullptr, flags);
            buffer.mMapped = (U8*)glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, (GLsizeiptr)bytes, flags);
            glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
            if (!buffer.mMapped)
            {
                LL_WARNS("Rendering") << "FSPerfGPUDrivenUploadRing: a " << (bytes >> 20) << " MB buffer could not be mapped; "
                                      << "the ring writes with glBufferSubData from now on" << LL_ENDL;
                free_ring_buffer(buffer);
                sPersistentFailed = true;
                return false;
            }
            return true;
        }
        glBufferData(GL_COPY_WRITE_BUFFER, (GLsizeiptr)bytes, nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        return true;
    }

    // Waits until the GPU is done with what the fence follows
    void wait_fence(GLsync fence)
    {
        GLenum status = glClientWaitSync(fence, 0, 0);
        if (status == GL_TIMEOUT_EXPIRED)
        {
            ++sStatFenceWaits;
            for (U32 tries = 0; tries < 10 && status == GL_TIMEOUT_EXPIRED; ++tries)
            {
                status = glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 100000000ull);   // 100 ms
            }
        }
        if (status == GL_WAIT_FAILED || status == GL_TIMEOUT_EXPIRED)
        {   // the fence can't tell: make sure the GPU is done before the slot is written again
            glFinish();
        }
    }

    // The first upload of a frame moves to the next slot
    void begin_ring_frame()
    {
        const U32 frame = LLFrameTimer::getFrameCount();
        if (sRingStarted && frame == sRingFrame)
        {
            return;
        }
        if (sRingStarted)
        {
            RingSlot& done = sSlots[sSlot];
            if (sRingMode == RING_PERSISTENT)
            {   // after every command that reads the slot's slices
                if (done.mFence)
                {
                    glDeleteSync(done.mFence);
                }
                done.mFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            }
            sSlot = (sSlot + 1) % RING_SLOTS;
        }
        sRingStarted = true;
        sRingFrame = frame;

        RingSlot& slot = sSlots[sSlot];
        if (slot.mFence)
        {
            wait_fence(slot.mFence);
            glDeleteSync(slot.mFence);
            slot.mFence = nullptr;
        }
        for (RingBuffer& retired : slot.mRetired)
        {
            free_ring_buffer(retired);
        }
        slot.mRetired.clear();
        slot.mUsed = 0;
    }

    bool ring_alloc(size_t bytes, RingSlice& out)
    {
        const ERingMode mode = wanted_mode();
        if (mode != sRingMode)
        {   // first use, or the render thread started or stopped: the old buffers can't be used the new way
            release_ring();
            sRingMode = mode;
            GLint align = 0;
            glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &align);
            sAlign = llmax((size_t)llmax(align, 16), (size_t)16);
            if (sAlign & (sAlign - 1))
            {   // not a power of two: unheard of, but the rounding below needs one
                sAlign = 256;
            }
        }
        begin_ring_frame();

        const size_t size = (llmax(bytes, (size_t)16) + sAlign - 1) & ~(sAlign - 1);
        RingSlot& slot = sSlots[sSlot];
        size_t offset = (slot.mUsed + sAlign - 1) & ~(sAlign - 1);
        if (!slot.mBuffer.mName || offset + size > slot.mCapacity)
        {
            size_t capacity = llmax(RING_FIRST_BYTES, slot.mCapacity * 2);
            while (capacity < size && capacity <= RING_MAX_BYTES)
            {
                capacity *= 2;
            }
            if (capacity > RING_MAX_BYTES)
            {
                return false;
            }
            RingBuffer grown;
            if (!create_ring_buffer(grown, capacity))
            {
                return false;
            }
            if (slot.mBuffer.mName)
            {
                slot.mRetired.push_back(slot.mBuffer);
                ++sStatGrowths;
            }
            slot.mBuffer = grown;
            slot.mCapacity = capacity;
            offset = 0;
        }
        slot.mUsed = offset + size;
        out.mName = slot.mBuffer.mName;
        out.mOffset = offset;
        out.mSize = size;
        out.mMapped = slot.mBuffer.mMapped ? slot.mBuffer.mMapped + offset : nullptr;
        ++sStatRingSlices;
        sStatRingBytes += bytes;
        return true;
    }

    // A slice from this frame of the ring as it is now, which its buffer still holds
    bool ring_slice_current(U32 frame, U32 generation)
    {
        return sRingStarted && generation == sRingGeneration && frame == sRingFrame;
    }

    void log_stats()
    {
        static LLCachedControl<bool> ring(gSavedSettings, "FSPerfGPUDrivenUploadRing", false);
        static LLCachedControl<bool> shared(gSavedSettings, "FSPerfGPUDrivenSharedMatrices", false);
        static LLFrameTimer timer;
        if ((!ring && !shared) || timer.getElapsedTimeF32() < 10.f)
        {
            return;
        }
        timer.reset();
        size_t ring_bytes = 0;
        for (const RingSlot& slot : sSlots)
        {
            ring_bytes += slot.mCapacity;
        }
        const char* mode = sRingMode == RING_PERSISTENT ? "persistent mapped" : sRingMode == RING_SUB_DATA ? "sub data" : "unused";
        LL_INFOS("FSGPUDrivenUpload") << "GPU-driven uploads, last 10 s: ring (" << mode << ", " << (ring_bytes >> 10) << " KB in "
                                      << RING_SLOTS << " slots) " << sStatRingSlices << " slices, " << (sStatRingBytes >> 10)
                                      << " KB, " << sStatGrowths << " growths, " << sStatFenceWaits << " fence waits; own buffers "
                                      << sStatOwnUploads << " uploads, " << (sStatOwnBytes >> 10) << " KB; matrix tables: "
                                      << gFSGDDStatModelsComputed << " models computed, " << gFSGDDStatModelsFound
                                      << " found already there, " << sStatTailWrites << " shared table appends" << LL_ENDL;
        sStatRingSlices = sStatRingBytes = sStatOwnUploads = sStatOwnBytes = sStatTailWrites = sStatGrowths = sStatFenceWaits = 0;
        gFSGDDStatModelsComputed = gFSGDDStatModelsFound = 0;
    }
}

//-----------------------------------------------------------------------------
// FSGDDBuffer

void FSGDDBuffer::allocate(GLenum target, size_t capacity, const void* data, size_t bytes)
{
    LL_RECORD_BLOCK_TIME(FTM_FS_GDD_UPLOAD);
    log_stats();
    static LLCachedControl<bool> use_ring(gSavedSettings, "FSPerfGPUDrivenUploadRing", false);
    if (use_ring)
    {
        RingSlice slice;
        if (ring_alloc(capacity, slice))
        {
            mName = slice.mName;
            mOffset = slice.mOffset;
            mCapacity = slice.mSize;
            mMapped = slice.mMapped;
            mRingFrame = sRingFrame;
            mRingGeneration = sRingGeneration;
            glBindBuffer(target, mName);
            if (bytes)
            {
                if (mMapped)
                {
                    memcpy(mMapped, data, bytes);
                }
                else
                {
                    glBufferSubData(target, (GLintptr)mOffset, (GLsizeiptr)bytes, data);
                }
            }
            return;
        }
    }

    // the own buffer, new storage: what the previous pass's draws read stays theirs
    if (!mOwn)
    {
        glGenBuffers(1, &mOwn);
    }
    glBindBuffer(target, mOwn);
    const size_t size = llmax(capacity, (size_t)16);
    if (bytes == capacity)
    {
        glBufferData(target, size, bytes ? data : nullptr, GL_STREAM_DRAW);
    }
    else
    {
        glBufferData(target, size, nullptr, GL_STREAM_DRAW);
        if (bytes)
        {
            glBufferSubData(target, 0, (GLsizeiptr)bytes, data);
        }
    }
    mName = mOwn;
    mOffset = 0;
    mCapacity = size;
    mMapped = nullptr;
    ++sStatOwnUploads;
    sStatOwnBytes += bytes;
}

bool FSGDDBuffer::writeAt(GLenum target, size_t offset, const void* data, size_t bytes)
{
    if (!mName || offset + bytes > mCapacity)
    {
        return false;
    }
    if (mName != mOwn && !ring_slice_current(mRingFrame, mRingGeneration))
    {
        return false;
    }
    LL_RECORD_BLOCK_TIME(FTM_FS_GDD_UPLOAD);
    ++sStatTailWrites;
    glBindBuffer(target, mName);
    if (bytes)
    {
        if (mMapped)
        {
            memcpy(mMapped + offset, data, bytes);
        }
        else
        {
            glBufferSubData(target, (GLintptr)(mOffset + offset), (GLsizeiptr)bytes, data);
        }
    }
    return true;
}

void FSGDDBuffer::bindBase(GLenum target, GLuint index) const
{
    if (mName == mOwn)
    {
        glBindBufferBase(target, index, mOwn);
    }
    else
    {
        glBindBufferRange(target, index, mName, (GLintptr)mOffset, (GLsizeiptr)mCapacity);
    }
}

void FSGDDBuffer::release()
{
    if (mOwn)
    {
        glDeleteBuffers(1, &mOwn);
    }
    mOwn = 0;
    mName = 0;
    mOffset = 0;
    mCapacity = 0;
    mMapped = nullptr;
}

//-----------------------------------------------------------------------------
// Shared matrix tables

namespace
{
    constexpr U32 SHARED_TABLES = 8;    // the camera, shadow maps, probe faces, water: the least recently used goes

    struct SharedTable
    {
        FSGDDMatrixStore mStore;
        glm::mat4 mProj = glm::mat4(1.f);
        glm::mat4 mView = glm::mat4(1.f);
        U32 mFrame = 0;
        U32 mCull = 0;
        U64 mLastUse = 0;
        bool mFull = false;
        bool mValid = false;
    };

    SharedTable sTables[SHARED_TABLES];
    U32 sCull = 0;
    U64 sUse = 0;
}

void fsGDDNewCull()
{
    ++sCull;
}

FSGDDMatrixStore* fsGDDSharedMatrixStore(const glm::mat4& proj, const glm::mat4& view, bool full)
{
    const U32 frame = LLFrameTimer::getFrameCount();
    SharedTable* oldest = &sTables[0];
    for (SharedTable& table : sTables)
    {
        if (table.mValid && table.mFrame == frame && table.mCull == sCull && table.mFull == full &&
            !memcmp(&table.mProj, &proj, sizeof(glm::mat4)) && !memcmp(&table.mView, &view, sizeof(glm::mat4)))
        {
            table.mLastUse = ++sUse;
            return &table.mStore;
        }
        // an unused table, else the least recently used (tables of earlier frames and culls are older than any of now)
        if (oldest->mValid && (!table.mValid || table.mLastUse < oldest->mLastUse))
        {
            oldest = &table;
        }
    }
    // A new table. Its buffer keeps its storage: the next allocate gives it new storage (or a new ring slice),
    // and the draws of its old contents keep reading what they had.
    SharedTable& table = *oldest;
    table.mStore.clear();
    table.mStore.mMatrices.push_back(glm::mat4(1.f));  // texture matrix 0: identity
    table.mProj = proj;
    table.mView = view;
    table.mFrame = frame;
    table.mCull = sCull;
    table.mFull = full;
    table.mValid = true;
    table.mLastUse = ++sUse;
    return &table.mStore;
}

void fsGDDCommonReleaseGL()
{
    release_ring();
    for (SharedTable& table : sTables)
    {
        table.mStore.mBuffer.release();
        table.mStore.clear();
        table.mValid = false;
    }
}
