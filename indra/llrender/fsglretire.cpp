/**
 * @file fsglretire.cpp
 * @brief Releases bindless texture handles, and the textures behind them, only
 *        once the GPU has finished every frame that could sample them.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
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

#include "linden_common.h"

#include "fsglretire.h"
#include "fsglstream.h"

#include "llgl.h"
#include "llimagegl.h"
#include "llrender.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <vector>

namespace
{
    // A fence that never comes back (it could not be made, or the test failed) would keep what it covers
    // forever; after this many frames that is released anyway and counted as released early. The GPU is
    // never that far behind.
    constexpr U32 MAX_WAIT_FRAMES = 1000;

    struct Retiree
    {
        U64 mHandle = 0;        // a handle (with mSampler), or 0 for a texture
        U32 mSampler = 0;
        U32 mTexture = 0;
        U32 mSerial = 0;        // the fence that covers it
        U32 mFrame = 0;         // sFrame when retired
        bool mResident = false;
    };

    // Main thread (the mutex only guards against a stray call from another thread)
    std::mutex sMutex;
    std::deque<Retiree> sQueue;             // in retire order, so serials never go down
    void (*sNonResident)(U64) = nullptr;
    U32 sNextSerial = 1;                    // the fence that will cover what is retired now
    U32 sLastFence = 0;                     // the last fence recorded
    U32 sKnownDone = 0;                     // the highest fence known to have signalled
    bool sNeedFence = false;                // something was retired since the last fence
    U32 sFrame = 0;
    U32 sStatHandles = 0;
    U32 sStatTextures = 0;
    U32 sStatEarly = 0;

    // The GL thread (the render thread, or the main thread without it) stores the highest fence that has
    // signalled; the main thread reads it a frame later
    std::atomic<U32> sDone{ 0 };

    // GL thread: the fences recorded and not yet seen signalled, oldest first
    std::deque<std::pair<U32, GLsync>> sFences;

    // serials only go up; differences keep that true across a wrap
    bool reached(U32 done, U32 serial)
    {
        return (S32)(done - serial) >= 0;
    }

    void store_done(U32 serial)
    {
        U32 current = sDone.load(std::memory_order_relaxed);
        while (!reached(current, serial) &&
               !sDone.compare_exchange_weak(current, serial, std::memory_order_release, std::memory_order_relaxed))
        {
        }
    }

    // GL thread, in stream order after the frame's commands
    void gl_fence(U64 serial)
    {
        GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (fence)
        {
            sFences.emplace_back((U32)serial, fence);
        }
    }

    // GL thread: tests the fences oldest first without waiting. A fence that signalled means the GPU is
    // done with everything recorded before it, so the earlier ones are done too.
    void gl_poll(U64)
    {
        while (!sFences.empty())
        {
            const GLenum status = glClientWaitSync(sFences.front().second, 0, 0);
            if (status == GL_TIMEOUT_EXPIRED)
            {
                return;
            }
            if (status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED)
            {
                store_done(sFences.front().first);
            }
            // GL_WAIT_FAILED: a broken fence clears nothing; a later one does
            glDeleteSync(sFences.front().second);
            sFences.pop_front();
        }
    }

    // GL thread: waits for the GPU, then everything up to serial is done
    void gl_finish(U64 serial)
    {
        glFinish();
        for (auto& fence : sFences)
        {
            glDeleteSync(fence.second);
        }
        sFences.clear();
        store_done((U32)serial);
    }

    // GL thread: the fences are not needed (nothing waits on them)
    void gl_drop_fences(U64 serial)
    {
        for (auto& fence : sFences)
        {
            glDeleteSync(fence.second);
        }
        sFences.clear();
        store_done((U32)serial);
    }

    // Main thread, without sMutex: records the release of each, in order (a texture's handles were retired
    // before it, so they go first)
    void release(const std::vector<Retiree>& ready)
    {
        bool textures = false;
        for (const Retiree& item : ready)
        {
            if (item.mTexture)
            {
                LLImageGLMemory::free_tex_images(1, &item.mTexture);
                glDeleteTextures(1, &item.mTexture);
                textures = true;
                continue;
            }
            if (item.mResident && item.mHandle && sNonResident)
            {
                FSGLStream::recordCall(sNonResident, item.mHandle);
            }
            if (item.mSampler)
            {
                glDeleteSamplers(1, &item.mSampler);
            }
        }
        if (textures)
        {   // a deleted texture that was bound is unbound and its name can come back: bindFast must not trust it
            gGL.invalidateFastBinds();
        }
    }
}

void FSGLRetire::setNonResidentFunc(void (*fn)(U64 handle))
{
    std::lock_guard<std::mutex> lock(sMutex);
    sNonResident = fn;
}

void FSGLRetire::retireHandle(U64 handle, bool resident, U32 sampler)
{
    if (!handle && !sampler)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(sMutex);
    Retiree item;
    item.mHandle = handle;
    item.mSampler = sampler;
    item.mResident = resident;
    item.mSerial = sNextSerial;
    item.mFrame = sFrame;
    sQueue.push_back(item);
    sNeedFence = true;
    ++sStatHandles;
}

void FSGLRetire::retireTexture(U32 name)
{
    if (!name)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(sMutex);
    Retiree item;
    item.mTexture = name;
    item.mSerial = sNextSerial;
    item.mFrame = sFrame;
    sQueue.push_back(item);
    sNeedFence = true;
    ++sStatTextures;
}

void FSGLRetire::frame()
{
    std::vector<Retiree> ready;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        ++sFrame;
        if (sQueue.empty() && !sNeedFence && reached(sKnownDone, sLastFence))
        {
            return; // the usual case: nothing retired, nothing in flight
        }
        if (sNeedFence)
        {
            sLastFence = sNextSerial++;
            sNeedFence = false;
            FSGLStream::recordCall(&gl_fence, sLastFence);
        }
        const U32 done = sDone.load(std::memory_order_acquire);
        if (reached(done, sKnownDone))
        {
            sKnownDone = done;
        }
        if (!reached(sKnownDone, sLastFence))
        {   // the result is seen at a later frame
            FSGLStream::recordCall(&gl_poll, 0);
        }
        while (!sQueue.empty())
        {
            const Retiree& front = sQueue.front();
            if (!reached(sKnownDone, front.mSerial))
            {
                if (sFrame - front.mFrame < MAX_WAIT_FRAMES)
                {
                    break;
                }
                ++sStatEarly;
            }
            ready.push_back(front);
            sQueue.pop_front();
        }
    }
    if (!ready.empty())
    {
        release(ready);
    }
}

void FSGLRetire::finish()
{
    std::vector<Retiree> ready;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        const U32 done = sDone.load(std::memory_order_acquire);
        if (reached(done, sKnownDone))
        {
            sKnownDone = done;
        }
        if (sQueue.empty())
        {
            if (!reached(sKnownDone, sLastFence))
            {   // fences still out, nothing waiting on them
                FSGLStream::recordCall(&gl_drop_fences, sLastFence);
                sKnownDone = sLastFence;
            }
            sNeedFence = false;
            return;
        }
        // everything recorded so far, these included, is done after the glFinish; what is released below is
        // recorded after it
        sLastFence = sNextSerial++;
        sNeedFence = false;
        FSGLStream::recordCall(&gl_finish, sLastFence);
        sKnownDone = sLastFence;
        ready.assign(sQueue.begin(), sQueue.end());
        sQueue.clear();
    }
    release(ready);
}

FSGLRetire::Stats FSGLRetire::takeStats()
{
    std::lock_guard<std::mutex> lock(sMutex);
    Stats stats;
    stats.mHandlesRetired = sStatHandles;
    stats.mTexturesRetired = sStatTextures;
    stats.mWaiting = (U32)sQueue.size();
    stats.mReleasedEarly = sStatEarly;
    sStatHandles = sStatTextures = sStatEarly = 0;
    return stats;
}
