/**
 * @file fsgddarena.cpp
 * @brief Vertex arenas for GPU-driven draws (FSPerfGPUDrivenDraws, FSPerfGPUDrivenPBR)
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

#include "fsgddarena.h"

#include "llframetimer.h" // shrink interval
#include "llgl.h"
#include "fsglstream.h"
#include "llviewercontrol.h"
#include "llviewertexture.h" // sFreeVRAMMegabytes

namespace
{
    constexpr U32 FS_GDD_COPIES_PER_FRAME = 2048;   // vertex buffers copied into an arena per frame, at most
    constexpr U32 FS_GDD_EVICT_AFTER = 600;         // frames an entry may go unused before it is dropped
    // An arena only grows, doubling, so after a crowd or a big region has left it holds video memory
    // it no longer uses. Holding at least this much and using less than a third of it at a sweep, it is dropped
    // and refills at the size it needs (copies on the GPU, at most FS_GDD_COPIES_PER_FRAME a frame; what isn't
    // back yet is drawn the old way meanwhile). It grows back in one step to what was in use and a quarter more
    // (FS_GDD_REFILL_SHARE), not by doubling up from the first size: each growth copies the buffer, and the
    // doublings back to a few hundred MB were hitches in a row. Refilled that way, it is well over a third used,
    // so it isn't dropped again at the next sweep (doubling left it about half used, and below half was too
    // close: a shadow arena using 86 of 176 MB was dropped, grew back to 176 MB and was dropped again every 40 s).
    // It drops at most once every FS_GDD_SHRINK_INTERVAL seconds.
    constexpr U64 FS_GDD_SHRINK_MIN_BYTES = 64ull << 20;
    constexpr U64 FS_GDD_SHRINK_USED_SHARE = 3;
    constexpr F64 FS_GDD_SHRINK_INTERVAL = 30.0;
    constexpr U32 FS_GDD_REFILL_SHARE = 4;          // the refill size is what was in use plus a quarter of it
    // glGetError waits for the render thread to run everything queued before it (with the render thread on,
    // a frame's worth of commands and the driver's lock, 300 ms at times). A growth already waits for this
    // much video memory free beyond its size (allocate); only with less than this free is a failed allocation
    // likely enough to check for it at once.
    constexpr F32 FS_GDD_GROW_CHECK_FREE_MB = 1024.f;
    // The first growth of an empty arena: so many vertices and indices at least (allocate). A refill never
    // takes less, so an arena is only dropped when it holds at least twice that, or one that uses little of
    // its first allocation would be dropped and refilled to the same size at every sweep.
    constexpr U32 FS_GDD_FIRST_VERTS = 1u << 20;
    constexpr U32 FS_GDD_FIRST_INDICES = 3u << 20;

    // A growth that isn't checked at once (FS_GDD_GROW_CHECK_FREE_MB) is checked in stream order instead,
    // without waiting: the render thread (or the main thread, without one) reads the new buffer's size, and
    // a buffer the driver gave no storage sets its arena's bit here. The arena drops everything at its next
    // frame (beginFrame) and doesn't grow for FS_GDD_GROW_BLOCK_SECONDS.
    std::atomic<U32> sGrowFailedSlots{ 0 };
    constexpr F64 FS_GDD_GROW_BLOCK_SECONDS = 60.0;

    // value: buffer name << 32 | arena slot << 24 | expected size in KB (up to 16 GB)
    void verify_grown(U64 value)
    {
        const GLuint buffer = (GLuint)(value >> 32);
        const U32 slot = (U32)(value >> 24) & 0xff;
        const U64 expected = (value & 0xffffff) << 10;
        if (!buffer || !glGetNamedBufferParameteriv || slot >= 32)
        {
            return;
        }
        GLint size = 0;
        glGetNamedBufferParameteriv(buffer, GL_BUFFER_SIZE, &size);
        if ((U64)(U32)size < expected)
        {
            sGrowFailedSlots.fetch_or(1u << slot, std::memory_order_relaxed);
        }
    }
}

//-----------------------------------------------------------------------------
// FSGDDRangeAllocator

void FSGDDRangeAllocator::reset()
{
    mFree.clear();
    mFreeBySize.clear();
    mCapacity = 0;
    mUsed = 0;
}

void FSGDDRangeAllocator::grow(U32 capacity)
{
    if (capacity > mCapacity)
    {
        insertFree(mCapacity, capacity - mCapacity);
        mCapacity = capacity;
    }
}

// Best fit: the smallest free range that holds the request, lowest offset among equal sizes.
// A first fit scan of mFree cost a few percent of the main thread once the arenas had
// fragmented into thousands of small holes.
bool FSGDDRangeAllocator::allocate(U32 size, U32& offset)
{
    auto fit = mFreeBySize.lower_bound(std::make_pair(size, 0u));
    if (fit == mFreeBySize.end())
    {
        return false;
    }
    offset = fit->second;
    const U32 rest = fit->first - size;
    removeFree(mFree.find(offset));
    if (rest)
    {
        addFree(offset + size, rest);
    }
    mUsed += size;
    return true;
}

void FSGDDRangeAllocator::addFree(U32 offset, U32 size)
{
    mFree.emplace(offset, size);
    mFreeBySize.emplace(size, offset);
}

void FSGDDRangeAllocator::removeFree(std::map<U32, U32>::iterator it)
{
    mFreeBySize.erase(std::make_pair(it->second, it->first));
    mFree.erase(it);
}

void FSGDDRangeAllocator::release(U32 offset, U32 size)
{
    if (size)
    {
        mUsed -= size;
        insertFree(offset, size);
    }
}

void FSGDDRangeAllocator::insertFree(U32 offset, U32 size)
{
    auto next = mFree.lower_bound(offset);
    if (next != mFree.begin())
    {
        auto prev = std::prev(next);
        if (prev->first + prev->second == offset)
        {
            offset = prev->first;
            size += prev->second;
            removeFree(prev);
        }
    }
    if (next != mFree.end() && offset + size == next->first)
    {
        size += next->second;
        removeFree(next);
    }
    addFree(offset, size);
}

//-----------------------------------------------------------------------------
// FSGDDArena

// static
const char* FSGDDArena::failName(U32 reason)
{
    static const char* names[FAIL_COUNT] = { "no buffer", "32 bit indices", "mapped", "missing attributes",
                                             "copy budget", "arena full" };
    return reason < FAIL_COUNT ? names[reason] : "?";
}

FSGDDArena::FSGDDArena(const char* name, const std::vector<U32>& types, const char* budget_setting, S32 default_mb, U32 index_share,
                       U32 optional_mask)
    : mName(name)
    , mTypes(types)
    , mBudgetSetting(budget_setting)
    , mDefaultMB(default_mb)
    , mIndexShare(llmax(index_share, 2u))
{
    static U32 s_next_slot = 0;
    mSlot = s_next_slot++; // past LLVertexBuffer::FS_ARENA_SLOTS: no cache, the map alone
    // every arena should have a slot: raise FS_ARENA_SLOTS when adding one
    llassert(mSlot < LLVertexBuffer::FS_ARENA_SLOTS);
    mVBs.resize(mTypes.size(), 0);
    mVBBytes.resize(mTypes.size(), 0);
    for (U32 type : mTypes)
    {
        if (!(optional_mask & (1u << type)))
        {
            mTypeMask |= 1u << type;
        }
        mStride += LLVertexBuffer::sTypeSize[type];
    }
}

void FSGDDArena::limits(U32 frame, U32& max_verts, U32& max_indices)
{
    if (mLimitsFrame == frame)
    {   // the setting is looked up by name, so once per frame, not per allocation
        max_verts = mMaxVerts;
        max_indices = mMaxIndices;
        return;
    }
    mLimitsFrame = frame;
    S32 mb = mDefaultMB;
    if (LLControlVariable* control = gSavedSettings.getControl(mBudgetSetting).get())
    {
        mb = control->getValue().asInteger();
    }
    const U64 bytes = (U64)llclamp(mb, 16, 2048) << 20;
    const U64 index_bytes = bytes / mIndexShare;
    max_verts = (U32)llmin((U64)U32_MAX / 64, (bytes - index_bytes) / mStride);
    max_indices = (U32)llmin((U64)U32_MAX / 4, index_bytes / sizeof(U16));
    mMaxVerts = max_verts;
    mMaxIndices = max_indices;
}

U32 FSGDDArena::vertexMB() const
{
    return (U32)(((U64)mVerts.used() * mStride) >> 20);
}

U32 FSGDDArena::indexMB() const
{
    return (U32)(((U64)mIndices.used() * sizeof(U16)) >> 20);
}

void FSGDDArena::clearCounts()
{
    mUploads = 0;
    mFallbacks = 0;
    for (U32& count : mFails)
    {
        count = 0;
    }
}

void FSGDDArena::fail(U32 reason, U32* out)
{
    ++mFallbacks;
    ++mFails[reason];
    if (out)
    {
        *out = reason;
    }
}

bool FSGDDArena::growBuffer(GLuint& buffer, U64& buffer_bytes, U32 old_bytes, U32 new_bytes)
{
    GLuint grown = 0;
    glGenBuffers(1, &grown);
    if (!grown)
    {
        return false;
    }
    glBindBuffer(GL_COPY_WRITE_BUFFER, grown);
    // Checked only when video memory is short: the check waits for the render thread
    const bool check = LLViewerTexture::sFreeVRAMMegabytes < (F32)(new_bytes >> 20) + FS_GDD_GROW_CHECK_FREE_MB;
    // clear older errors first, so the check below sees only this allocation's GL_OUT_OF_MEMORY
    if (check)
    {
        clear_glerror();
    }
    glBufferData(GL_COPY_WRITE_BUFFER, new_bytes, nullptr, GL_STATIC_DRAW);
    bool failed = check && glGetError() != GL_NO_ERROR;
    if (!check && mSlot < 32)
    {   // checked without waiting; without the render thread that is right here
        FSGLStream::recordCall(&verify_grown, ((U64)grown << 32) | ((U64)mSlot << 24) | (U64)(new_bytes >> 10));
        if (!FSGLStream::isRecording())
        {
            failed = (sGrowFailedSlots.fetch_and(~(1u << mSlot), std::memory_order_relaxed) & (1u << mSlot)) != 0;
        }
    }
    if (failed)
    {
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        glDeleteBuffers(1, &grown);
        return false;
    }
    if (buffer && old_bytes)
    {   // keep what is in there, the entries' offsets stay valid
        glBindBuffer(GL_COPY_READ_BUFFER, buffer);
        glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, old_bytes);
        glBindBuffer(GL_COPY_READ_BUFFER, 0);
    }
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
    if (buffer)
    {
        glDeleteBuffers(1, &buffer);
    }
    buffer = grown;
    setBufferBytes(buffer_bytes, new_bytes);
    return true;
}

// What the arenas hold counts as vertex memory (LLVertexBuffer::getBytesAllocated), so the
// texture budget sees it: with FSTextureUseLiveVRAM it would otherwise pass for another program's memory,
// measured only while there is room, and an arena growing while the GPU is full went unnoticed.
void FSGDDArena::setBufferBytes(U64& buffer_bytes, U64 bytes)
{
    LLVertexBuffer::fsAddExternalBytes((S64)bytes - (S64)buffer_bytes);
    mGLBytes = mGLBytes - buffer_bytes + bytes;
    buffer_bytes = bytes;
}

U32 FSGDDArena::allocatedMB() const
{
    return (U32)(mGLBytes >> 20);
}

U64 FSGDDArena::usedBytes() const
{
    return (U64)mVerts.used() * mStride + (U64)mIndices.used() * sizeof(U16);
}

void FSGDDArena::evict(U32 frame, U32 unused_frames)
{
    for (auto it = mEntries.begin(); it != mEntries.end(); )
    {
        if (frame - it->second.mLastFrame >= unused_frames)
        {
            mVerts.release(it->second.mVertex, it->second.mNumVerts);
            mIndices.release(it->second.mIndex, it->second.mNumIndices);
            it = mEntries.erase(it);
            ++mEpoch;
        }
        else
        {
            ++it;
        }
    }
}

bool FSGDDArena::allocate(U32 verts, U32 indices, U32 frame, U32& vertex_offset, U32& index_offset)
{
    if (mFullFrame == frame)
    {   // full and nothing to drop: don't search again for every buffer this frame
        return false;
    }
    U32 max_verts = 0;
    U32 max_indices = 0;
    limits(frame, max_verts, max_indices);

    for (U32 attempt = 0; attempt < 3; ++attempt)
    {
        // both tried, so a failure grows only the allocator that is short
        const bool verts_fit = mVerts.allocate(verts, vertex_offset);
        const bool indices_fit = mIndices.allocate(indices, index_offset);
        if (verts_fit && indices_fit)
        {
            return true;
        }
        if (verts_fit)
        {
            mVerts.release(vertex_offset, verts);
        }
        if (indices_fit)
        {
            mIndices.release(index_offset, indices);
        }

        if (attempt == 0)
        {   // grow, doubling, up to the limit; every attribute buffer by the same number of vertices
            // and after a drop, straight back to the size it had in use (mRefillVerts)
            const U32 want_verts = llmin(max_verts, llmax(llmax(mVerts.capacity() * 2, mRefillVerts), llmax(verts * 4, FS_GDD_FIRST_VERTS)));
            const U32 want_indices = llmin(max_indices, llmax(llmax(mIndices.capacity() * 2, mRefillIndices), llmax(indices * 4, FS_GDD_FIRST_INDICES)));
            // Growing allocates the bigger buffer beside the old one and copies into it, and
            // the arenas are not part of the texture budget. With video memory already at its target the
            // driver has to evict to make room: on Linux NVIDIA that blocked inside the allocation for
            // 10-30 s, once past the 60 s watchdog. Only grow into real headroom; what doesn't fit is
            // drawn the old way (as when the arena is at its limit).
            const bool grow_verts = !verts_fit && want_verts > mVerts.capacity();
            const bool grow_indices = !indices_fit && want_indices > mIndices.capacity();
            U64 grow_bytes = grow_indices ? (U64)want_indices * sizeof(U16) : 0;
            if (grow_verts)
            {
                for (U32 type : mTypes)
                {
                    grow_bytes += (U64)want_verts * LLVertexBuffer::sTypeSize[type];
                }
            }
            constexpr F32 GROW_HEADROOM_MB = 256.f;
            const bool room = (F32)(grow_bytes >> 20) + GROW_HEADROOM_MB <= LLViewerTexture::sFreeVRAMMegabytes &&
                              LLFrameTimer::getElapsedSeconds() >= mGrowBlockedUntil; // nor after a growth failed
            if (!room)
            {   // no room to refill in one step: grow by doubling from here, as far as there is room
                mRefillVerts = 0;
                mRefillIndices = 0;
            }
            if (grow_verts && room)
            {
                bool grown = true;
                for (size_t i = 0; i < mTypes.size() && grown; ++i)
                {
                    const U32 size = LLVertexBuffer::sTypeSize[mTypes[i]];
                    grown = growBuffer(mVBs[i], mVBBytes[i], mVerts.capacity() * size, want_verts * size);
                }
                if (grown)
                {
                    mVerts.grow(want_verts);
                    mRefillVerts = 0;
                }
                // else: the buffers that did grow hold the old range at the old offsets, still consistent
            }
            if (grow_indices && room &&
                growBuffer(mIB, mIBBytes, mIndices.capacity() * (U32)sizeof(U16), want_indices * (U32)sizeof(U16)))
            {
                mIndices.grow(want_indices);
                mRefillIndices = 0;
            }
        }
        else if (attempt == 1)
        {   // at the limit: drop what no pass used in this frame or the last one (entries used this frame are
            // still referenced by commands not yet drawn). Once per frame: a second walk this frame would
            // find nothing new to drop
            if (mEvictFrame == frame)
            {
                break;
            }
            mEvictFrame = frame;
            evict(frame, 2);
        }
    }
    mFullFrame = frame;
    return false;
}

void FSGDDArena::beginFrame(U32 frame)
{
    if (frame == mFrame)
    {
        return;
    }
    mFrame = frame;
    mCopies = 0;
    // A growth the driver gave no storage (checked in stream order, see verify_grown): what the
    // arena holds can't be trusted. Drop it all and draw the old way until growing is allowed again.
    if (mSlot < 32 && (sGrowFailedSlots.load(std::memory_order_relaxed) & (1u << mSlot)))
    {
        sGrowFailedSlots.fetch_and(~(1u << mSlot), std::memory_order_relaxed);
        LL_WARNS("FSGPUDriven") << mName << ": the driver could not make room to grow it (out of video memory); dropped, "
                                << "its draws go the old way for " << (S32)FS_GDD_GROW_BLOCK_SECONDS << " s" << LL_ENDL;
        releaseGL();
        mRefillVerts = 0;
        mRefillIndices = 0;
        mGrowBlockedUntil = LLFrameTimer::getElapsedSeconds() + FS_GDD_GROW_BLOCK_SECONDS;
    }
    if (frame - mLastSweep >= 120)
    {   // forget the copies of buffers that stopped being drawn (or were freed)
        mLastSweep = frame;
        evict(frame, FS_GDD_EVICT_AFTER);
        // and give back what the arena grew to but no longer uses. Here, before this frame's first
        // lookup: no command of this frame refers to it yet, and the driver keeps the buffers the last
        // frame's commands read alive until they are drawn.
        const U64 first_bytes = (U64)FS_GDD_FIRST_VERTS * mStride + (U64)FS_GDD_FIRST_INDICES * sizeof(U16);
        const F64 now = LLFrameTimer::getElapsedSeconds();
        if (mGLBytes >= FS_GDD_SHRINK_MIN_BYTES && mGLBytes >= 2 * first_bytes &&
            usedBytes() * FS_GDD_SHRINK_USED_SHARE < mGLBytes && now - mLastShrink >= FS_GDD_SHRINK_INTERVAL &&
            // with the texture bias at its cap, textures take what the arena gives back at once, and it
            // can't grow again (no headroom): what doesn't fit is drawn the old way. Keep it until there is room.
            !LLViewerTexture::sFSBiasAtCap)
        {
            mLastShrink = now;
            LL_INFOS("FSGPUDriven") << mName << ": " << allocatedMB() << " MB allocated, " << (U32)(usedBytes() >> 20)
                                    << " MB in use; dropped, it refills at the size it needs" << LL_ENDL;
            const U32 used_verts = mVerts.used();
            const U32 used_indices = mIndices.used();
            releaseGL();
            mRefillVerts = used_verts + used_verts / FS_GDD_REFILL_SHARE;
            mRefillIndices = used_indices + used_indices / FS_GDD_REFILL_SHARE;
        }
    }
}

FSGDDArenaEntry* FSGDDArena::peek(const LLVertexBuffer* vb)
{
    FSGDDArenaEntry* entry = static_cast<FSGDDArenaEntry*>(vb->getFSArenaEntry(mSlot, mEpoch));
    if (!entry)
    {
        auto it = mEntries.find(vb);
        entry = it != mEntries.end() ? &it->second : nullptr;
    }
    if (!entry || entry->mSerial != vb->getFSContentSerial() ||
        entry->mNumVerts != vb->getNumVerts() || entry->mNumIndices != vb->getNumIndices())
    {
        return nullptr;
    }
    return entry;
}

FSGDDArenaEntry* FSGDDArena::find(LLVertexBuffer* vb, U32 frame, U32* reason)
{
    if (!vb || !vb->getFSGLBuffer() || !vb->getFSGLIndices() || !vb->getNumVerts() || !vb->getNumIndices())
    {
        fail(FAIL_NO_BUFFER, reason);
        return nullptr;
    }
    if (vb->getFSIndicesType() != GL_UNSIGNED_SHORT)
    {
        fail(FAIL_INDEX32, reason);
        return nullptr;
    }
    if (vb->isFSMapped())
    {
        fail(FAIL_MAPPED, reason);
        return nullptr;
    }
    if ((vb->getTypeMask() & mTypeMask) != mTypeMask)
    {
        fail(FAIL_ATTRIBUTES, reason);
        return nullptr;
    }

    const U32 serial = vb->getFSContentSerial();
    // The entry the buffer remembers (map nodes don't move; evicting moves the epoch on, and dropping a
    // changed copy below clears the pointer of the one buffer that holds it)
    if (FSGDDArenaEntry* cached = static_cast<FSGDDArenaEntry*>(vb->getFSArenaEntry(mSlot, mEpoch)))
    {
        if (cached->mSerial == serial && cached->mNumVerts == vb->getNumVerts() && cached->mNumIndices == vb->getNumIndices())
        {
            cached->mLastFrame = frame;
            return cached;
        }
    }
    auto it = mEntries.find(vb);
    if (it != mEntries.end())
    {
        FSGDDArenaEntry& entry = it->second;
        if (entry.mSerial == serial && entry.mNumVerts == vb->getNumVerts() && entry.mNumIndices == vb->getNumIndices())
        {
            entry.mLastFrame = frame;
            vb->setFSArenaEntry(mSlot, &entry, mEpoch);
            return &entry;
        }
        // changed since it was copied (or another buffer at the same address). Only this buffer can
        // remember the node (it is the key), so clear its pointer rather than moving the epoch on, which
        // would make every buffer look its entry up in the map again.
        mVerts.release(entry.mVertex, entry.mNumVerts);
        mIndices.release(entry.mIndex, entry.mNumIndices);
        mEntries.erase(it);
        vb->setFSArenaEntry(mSlot, nullptr, mEpoch);
    }

    if (mCopies >= FS_GDD_COPIES_PER_FRAME)
    {
        fail(FAIL_COPY_BUDGET, reason);
        return nullptr;
    }

    FSGDDArenaEntry entry;
    entry.mSerial = serial;
    entry.mNumVerts = vb->getNumVerts();
    entry.mNumIndices = vb->getNumIndices();
    entry.mLastFrame = frame;
    for (U32 type : mTypes)
    {
        entry.mTypeMask |= vb->getTypeMask() & (1u << type);
    }
    if (!allocate(entry.mNumVerts, entry.mNumIndices, frame, entry.mVertex, entry.mIndex))
    {
        LL_WARNS_ONCE("Rendering") << mName << ": the arena is full (" << vertexMB() << " MB vertices, " << indexMB()
                                   << " MB indices, limit " << mBudgetSetting << "), what doesn't fit is drawn the old way"
                                   << LL_ENDL;
        fail(FAIL_FULL, reason);
        return nullptr;
    }

    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("gdd arena copy");
        glBindBuffer(GL_COPY_READ_BUFFER, vb->getFSGLBuffer());
        for (size_t i = 0; i < mTypes.size(); ++i)
        {
            const LLVertexBuffer::AttributeType type = (LLVertexBuffer::AttributeType)mTypes[i];
            if (!(entry.mTypeMask & (1u << type)))
            {   // optional and not in this buffer: its range stays as it was, nothing reads it
                continue;
            }
            const U32 size = LLVertexBuffer::sTypeSize[type];
            glBindBuffer(GL_COPY_WRITE_BUFFER, mVBs[i]);
            glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, vb->getOffset(type),
                                (GLintptr)entry.mVertex * size, (GLsizeiptr)entry.mNumVerts * size);
        }
        glBindBuffer(GL_COPY_READ_BUFFER, vb->getFSGLIndices());
        glBindBuffer(GL_COPY_WRITE_BUFFER, mIB);
        glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0,
                            (GLintptr)entry.mIndex * sizeof(U16), (GLsizeiptr)entry.mNumIndices * sizeof(U16));
        glBindBuffer(GL_COPY_READ_BUFFER, 0);
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
    }
    ++mCopies;
    ++mUploads;
    FSGDDArenaEntry* stored = &(mEntries[vb] = entry);
    vb->setFSArenaEntry(mSlot, stored, mEpoch);
    return stored;
}

void FSGDDArena::bindForDraw(U32 attrib_mask)
{
    // the attributes and index buffer go into the global vertex array object, never a buffer's own
    LLVertexBuffer::fsUseDefaultVAO();
    for (size_t i = 0; i < mTypes.size(); ++i)
    {
        const U32 type = mTypes[i];
        if (!(attrib_mask & (1u << type)))
        {
            continue;
        }
        const GLsizei stride = LLVertexBuffer::sTypeSize[type];
        glBindBuffer(GL_ARRAY_BUFFER, mVBs[i]);
        if (type == LLVertexBuffer::TYPE_VERTEX && (attrib_mask & LLVertexBuffer::MAP_TEXTURE_INDEX))
        {   // the texture index is the w of the position, as setupVertexBuffer reads it
            glVertexAttribIPointer(LLVertexBuffer::TYPE_TEXTURE_INDEX, 1, GL_UNSIGNED_INT, stride, (const void*)12);
        }
        switch (type)
        {   // as LLVertexBuffer::setupVertexBuffer
        case LLVertexBuffer::TYPE_VERTEX:
        case LLVertexBuffer::TYPE_NORMAL:
            glVertexAttribPointer(type, 3, GL_FLOAT, GL_FALSE, stride, nullptr);
            break;
        case LLVertexBuffer::TYPE_TANGENT:
        case LLVertexBuffer::TYPE_WEIGHT4:  // the rigged arena (FSPerfGPUDrivenRigged)
            glVertexAttribPointer(type, 4, GL_FLOAT, GL_FALSE, stride, nullptr);
            break;
        case LLVertexBuffer::TYPE_TEXCOORD0:
        case LLVertexBuffer::TYPE_TEXCOORD1:
        case LLVertexBuffer::TYPE_TEXCOORD2:
        case LLVertexBuffer::TYPE_TEXCOORD3:
            glVertexAttribPointer(type, 2, GL_FLOAT, GL_FALSE, stride, nullptr);
            break;
        case LLVertexBuffer::TYPE_COLOR:
        case LLVertexBuffer::TYPE_EMISSIVE:
            glVertexAttribPointer(type, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, nullptr);
            break;
        default:
            llassert(false);
            break;
        }
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mIB);
}

bool FSGDDArena::verifyEntry(const LLVertexBuffer* vb, FSGDDArenaEntry& entry)
{
    entry.mVerified = true;
    bool ok = true;
    for (size_t i = 0; i < mTypes.size() && ok; ++i)
    {
        const LLVertexBuffer::AttributeType type = (LLVertexBuffer::AttributeType)mTypes[i];
        if (!(entry.mTypeMask & (1u << type)))
        {
            continue;
        }
        const U32 size = LLVertexBuffer::sTypeSize[type];
        std::vector<U8> src((size_t)entry.mNumVerts * size);
        std::vector<U8> dst(src.size());
        glBindBuffer(GL_COPY_READ_BUFFER, vb->getFSGLBuffer());
        glGetBufferSubData(GL_COPY_READ_BUFFER, vb->getOffset(type), src.size(), src.data());
        glBindBuffer(GL_COPY_READ_BUFFER, mVBs[i]);
        glGetBufferSubData(GL_COPY_READ_BUFFER, (GLintptr)entry.mVertex * size, dst.size(), dst.data());
        ok = src == dst;
    }

    std::vector<U16> src_idx(entry.mNumIndices);
    std::vector<U16> dst_idx(entry.mNumIndices);
    glBindBuffer(GL_COPY_READ_BUFFER, vb->getFSGLIndices());
    glGetBufferSubData(GL_COPY_READ_BUFFER, 0, src_idx.size() * sizeof(U16), src_idx.data());
    glBindBuffer(GL_COPY_READ_BUFFER, mIB);
    glGetBufferSubData(GL_COPY_READ_BUFFER, (GLintptr)entry.mIndex * sizeof(U16), dst_idx.size() * sizeof(U16), dst_idx.data());
    glBindBuffer(GL_COPY_READ_BUFFER, 0);
    ok = ok && src_idx == dst_idx;
    U16 max_index = 0;
    for (U16 i : dst_idx)
    {
        max_index = llmax(max_index, i);
    }
    ok = ok && max_index < entry.mNumVerts;
    return ok;
}

void FSGDDArena::releaseGL()
{
    // the buffers deleted, and no longer counted as vertex memory
    for (size_t i = 0; i < mVBs.size(); ++i)
    {
        if (mVBs[i])
        {
            glDeleteBuffers(1, &mVBs[i]);
            mVBs[i] = 0;
        }
        setBufferBytes(mVBBytes[i], 0);
    }
    setBufferBytes(mIBBytes, 0);
    if (mIB)
    {
        glDeleteBuffers(1, &mIB);
        mIB = 0;
    }
    mVerts.reset();
    mIndices.reset();
    mEntries.clear();
    ++mEpoch;
    mCopies = 0;
    mFullFrame = U32_MAX;
    mEvictFrame = U32_MAX;
    mLimitsFrame = U32_MAX;
}
