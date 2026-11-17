/**
 * @file fsvolumeprebuild.cpp
 * @brief Builds the shapes of prims for a new level of detail on worker threads ahead of the switch
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
#include "fsvolumeprebuild.h"

#include "llframetimer.h"
#include "llthread.h"
#include "llviewercontrol.h"
#include "llvolume.h"
#include "llvolumemgr.h"
#include "llvovolume.h"
#include "workqueue.h"

#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <thread>

namespace
{
    enum : S32
    {
        JOB_QUEUED,
        JOB_RUNNING,
        JOB_DONE,
        JOB_CANCELLED
    };

    struct Job
    {
        Job(const LLVolumeParams& params, S32 lod, U32 frame)
            : mParams(params), mLOD(lod), mFrame(frame)
        {
        }

        const LLVolumeParams mParams;
        const S32 mLOD;
        const U32 mFrame; // frame of the request

        // QUEUED -> RUNNING by whichever thread builds it (a worker, or the main thread when it gets there
        // first), RUNNING -> DONE once mVolume is set; QUEUED -> CANCELLED when the main thread drops it.
        std::atomic<S32> mState{ JOB_QUEUED };

        // Written only by the thread that moved mState to RUNNING, read by the main thread after it saw
        // DONE. Nobody else knows the volume until then, so its (non atomic) reference count is only ever
        // touched by one thread at a time.
        LLPointer<LLVolume> mVolume;
    };
    typedef std::shared_ptr<Job> job_ptr_t;

    // Keyed by LOD and the parameters inside the job, which outlives its entry
    typedef std::pair<S32, const LLVolumeParams*> key_t;
    struct KeyLess
    {
        bool operator()(const key_t& a, const key_t& b) const
        {
            if (a.first != b.first)
            {
                return a.first < b.first;
            }
            return *a.second < *b.second;
        }
    };
    typedef std::map<key_t, job_ptr_t, KeyLess> job_map_t;

    // Main thread only
    job_map_t sJobs;                // the jobs refLOD and calcLOD can still take
    std::deque<job_ptr_t> sOrder;   // every job, in request order, until it expires

    // A job nobody took within this many frames is dropped (the object went away, or wants another LOD)
    constexpr U32 EXPIRE_FRAMES = 120;
    // Past this many waiting jobs the stock path builds the shapes
    constexpr size_t MAX_JOBS = 4096;

    // Builds the shape exactly as LLVolumeLODGroup::refLOD does, unless another thread got the job first
    void build(Job& job)
    {
        S32 expected = JOB_QUEUED;
        if (!job.mState.compare_exchange_strong(expected, JOB_RUNNING, std::memory_order_acq_rel))
        {
            return;
        }

        LL_PROFILE_ZONE_SCOPED_CATEGORY_VOLUME;
        try
        {
            job.mVolume = new LLVolume(job.mParams, LLVolumeLODGroup::getVolumeScaleFromDetail(job.mLOD));
        }
        catch (...)
        {
            // Out of memory: the main thread builds it the stock way, and fails there as before
            job.mVolume = nullptr;
        }
        job.mState.store(JOB_DONE, std::memory_order_release);
    }

    // Main thread: the job's shape, built here if no worker has started it, else after waiting for the
    // worker that has. Null if it could not be built.
    LLPointer<LLVolume> finish(Job& job)
    {
        build(job);
        S32 state = job.mState.load(std::memory_order_acquire);
        if (state == JOB_RUNNING)
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_VOLUME("fs prebuild wait");
            do
            {
                std::this_thread::yield();
                state = job.mState.load(std::memory_order_acquire);
            } while (state == JOB_RUNNING);
        }
        if (state != JOB_DONE)
        {
            return nullptr;
        }
        LLPointer<LLVolume> volume = job.mVolume;
        job.mVolume = nullptr;
        return volume;
    }

    // LLVolumeLODGroup::sFSPrebuiltLOD: refLOD is about to build this shape
    LLPointer<LLVolume> takePrebuilt(const LLVolumeParams& params, S32 lod)
    {
        // The map is the main thread's; refLOD runs there anyway
        if (!on_main_thread() || sJobs.empty())
        {
            return nullptr;
        }
        job_map_t::iterator it = sJobs.find(key_t(lod, &params));
        if (it == sJobs.end())
        {
            return nullptr;
        }

        LL_PROFILE_ZONE_SCOPED_CATEGORY_VOLUME;
        job_ptr_t job = it->second;
        sJobs.erase(it);
        return finish(*job);
    }

    // Main thread, the job has left the map: a worker that hasn't started it skips it, a built shape
    // nobody took is freed
    void dropJob(Job& job)
    {
        S32 expected = JOB_QUEUED;
        job.mState.compare_exchange_strong(expected, JOB_CANCELLED, std::memory_order_acq_rel);
        if (job.mState.load(std::memory_order_acquire) == JOB_DONE)
        {
            LLVolume::fsRelease(job.mVolume);
        }
        // Still running: the worker finishes it and its last reference frees it there
    }
}

//static
bool FSVolumePrebuild::lodReady(LLVOVolume* volobj, S32 lod)
{
    static LLCachedControl<bool> enabled(gSavedSettings, "FSPerfThreadedGeometry", false);
    if (!enabled || !volobj || lod < 0 || lod >= LLVolumeLODGroup::NUM_LODS)
    {
        return true;
    }

    LLVolume* volume = volobj->getVolume();
    if (!volume)
    {
        return true;
    }

    // Unique (flexible) shapes have no group; a LOD the group has already is no work
    LLVolumeLODGroup* group = volume->fsLODGroup();
    if (!group || group->fsHasLOD(lod))
    {
        return true;
    }

    // Plain prims only: sculpts are built from their texture, meshes on the mesh threads
    const LLVolumeParams& params = volume->getParams();
    if (params.getSculptType() != LL_SCULPT_TYPE_NONE || params.getSculptID().notNull() ||
        params.getPathParams().getCurveType() == LL_PCODE_PATH_FLEXIBLE)
    {
        return true;
    }

    // What is being edited, and HUDs, update in the same frame
    if (volobj->isSelected() || volobj->isHUDAttachment())
    {
        return true;
    }
    LLViewerObject* root = volobj->getRootEdit();
    if (root && root->isSelected())
    {
        return true;
    }

    LL_PROFILE_ZONE_SCOPED_CATEGORY_VOLUME;
    const U32 frame = LLFrameTimer::getFrameCount();
    job_map_t::iterator it = sJobs.find(key_t(lod, &params));
    if (it != sJobs.end())
    {
        // Built already, or requested in an earlier frame: switch now. refLOD takes the shape, and finishes
        // it itself if no worker got to it, so nothing is ever more than a frame late.
        return it->second->mFrame != frame || it->second->mState.load(std::memory_order_acquire) == JOB_DONE;
    }

    if (sJobs.size() >= MAX_JOBS)
    {
        return true;
    }
    LL::WorkQueue::ptr_t queue = LL::WorkQueue::getInstance("General");
    if (!queue)
    {
        return true;
    }

    job_ptr_t job = std::make_shared<Job>(params, lod, frame);
    if (!queue->post([job]() { build(*job); }))
    {
        // Pool closed (logging out): the stock path
        return true;
    }

    LLVolumeLODGroup::sFSPrebuiltLOD = takePrebuilt;
    sJobs.emplace(key_t(lod, &job->mParams), job);
    sOrder.push_back(job);
    return false;
}

//static
void FSVolumePrebuild::onFrame()
{
    if (sOrder.empty())
    {
        return;
    }

    const U32 frame = LLFrameTimer::getFrameCount();
    while (!sOrder.empty() && frame - sOrder.front()->mFrame > EXPIRE_FRAMES)
    {
        job_ptr_t job = std::move(sOrder.front());
        sOrder.pop_front();
        job_map_t::iterator it = sJobs.find(key_t(job->mLOD, &job->mParams));
        if (it != sJobs.end() && it->second == job)
        {
            sJobs.erase(it);
        }
        dropJob(*job);
    }

    if (sOrder.empty())
    {
        // Nothing left to take: refLOD goes back to the stock path until the next request
        LLVolumeLODGroup::sFSPrebuiltLOD = nullptr;
    }
}

//static
void FSVolumePrebuild::cleanup()
{
    LLVolumeLODGroup::sFSPrebuiltLOD = nullptr;
    sJobs.clear();
    for (job_ptr_t& job : sOrder)
    {
        dropJob(*job);
    }
    sOrder.clear();
}
