/**
 * @file fslinksetinventoryloader.cpp
 * @brief Loads the contents of every link of a linkset, a few links at a time
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

#include "fslinksetinventoryloader.h"

#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llvoinventorylistener.h"

#include <algorithm>

// A link that never answers (lost request, region gone quiet) must not hold
// up the links queued behind it. A late answer is still taken.
static const F32 REQUEST_TIMEOUT_SECONDS = 20.f;

class FSLinksetInventoryLoader::Listener : public LLVOInventoryListener
{
public:
    Listener(FSLinksetInventoryLoader* owner, const LLUUID& link_id)
    :   mOwner(owner),
        mLinkID(link_id)
    {
    }

    ~Listener() override = default;

    void listen(LLViewerObject* object)
    {
        registerVOInventoryListener(object, nullptr);
    }

    void request()
    {
        requestVOInventory();
    }

    void inventoryChanged(LLViewerObject* object,
                          LLInventoryObject::object_list_t* inventory,
                          S32 serial_num,
                          void* user_data) override
    {
        if (object && object->getID() == mLinkID)
        {
            mOwner->onInventoryChanged(mLinkID);
        }
    }

private:
    FSLinksetInventoryLoader* mOwner;
    LLUUID mLinkID;
};

FSLinksetInventoryLoader::FSLinksetInventoryLoader()
{
}

FSLinksetInventoryLoader::~FSLinksetInventoryLoader()
{
    clear();
}

void FSLinksetInventoryLoader::setState(Link& link, EState state)
{
    const bool was_pending = link.mState != DONE;
    const bool is_pending = state != DONE;
    mPending += (is_pending ? 1 : 0) - (was_pending ? 1 : 0);
    link.mState = state;
}

void FSLinksetInventoryLoader::addLink(LLViewerObject* object)
{
    if (!object || object->isDead())
    {
        return;
    }

    const LLUUID& link_id = object->getID();
    removeLink(link_id);

    Link& link = mLinks[link_id];
    link.mState = DONE; // so setState counts it as newly pending
    link.mListener = std::make_unique<Listener>(this, link_id);
    link.mListener->listen(object);
    setState(link, QUEUED);
    mQueue.push_back(link_id);
}

void FSLinksetInventoryLoader::removeLink(const LLUUID& link_id)
{
    auto it = mLinks.find(link_id);
    if (it == mLinks.end())
    {
        return;
    }

    setState(it->second, DONE);
    // The listener's destructor unregisters it from the object, if the object still exists
    mLinks.erase(it);

    // The queue and in-flight entries are dropped lazily in update(), so a
    // removal stays O(1) even while a big linkset is being unlinked.
}

void FSLinksetInventoryLoader::requestAgain(const LLUUID& link_id)
{
    auto it = mLinks.find(link_id);
    if (it == mLinks.end() || it->second.mState != DONE)
    {
        return;
    }

    setState(it->second, QUEUED);
    mQueue.push_front(link_id);
}

void FSLinksetInventoryLoader::clear()
{
    mLinks.clear();
    mQueue.clear();
    mInFlight.clear();
    mChanged.clear();
    mPending = 0;
}

bool FSLinksetInventoryLoader::isLoaded(const LLUUID& link_id) const
{
    auto it = mLinks.find(link_id);
    return it != mLinks.end() && it->second.mEverLoaded;
}

void FSLinksetInventoryLoader::onInventoryChanged(const LLUUID& link_id)
{
    auto it = mLinks.find(link_id);
    if (it == mLinks.end())
    {
        return;
    }

    Link& link = it->second;
    if (link.mState == IN_FLIGHT)
    {
        setState(link, DONE);
    }
    link.mEverLoaded = true;
    if (!link.mChangedQueued)
    {
        link.mChangedQueued = true;
        mChanged.push_back(link_id);
    }
}

void FSLinksetInventoryLoader::update(std::vector<LLUUID>& changed)
{
    // Drop finished, timed out and forgotten requests. At most MAX_IN_FLIGHT entries.
    mInFlight.erase(std::remove_if(mInFlight.begin(), mInFlight.end(),
        [this](const LLUUID& id)
        {
            auto it = mLinks.find(id);
            if (it == mLinks.end() || it->second.mState != IN_FLIGHT)
            {
                return true;
            }
            if (it->second.mRequestTimer.getElapsedTimeF32() > REQUEST_TIMEOUT_SECONDS)
            {
                setState(it->second, DONE);
                return true;
            }
            return false;
        }), mInFlight.end());

    while ((S32)mInFlight.size() < MAX_IN_FLIGHT && !mQueue.empty())
    {
        const LLUUID link_id = mQueue.front();
        mQueue.pop_front();

        auto it = mLinks.find(link_id);
        if (it == mLinks.end() || it->second.mState != QUEUED)
        {
            continue; // removed, or already answered by someone else's request
        }

        Link& link = it->second;
        LLViewerObject* object = gObjectList.findObject(link_id);
        if (!object || object->isDead())
        {
            setState(link, DONE);
            continue;
        }

        setState(link, IN_FLIGHT);
        link.mRequestTimer.reset();
        mInFlight.push_back(link_id);
        // Cached contents answer at once, through onInventoryChanged()
        link.mListener->request();
    }

    if (!mChanged.empty())
    {
        for (const LLUUID& link_id : mChanged)
        {
            auto it = mLinks.find(link_id);
            if (it != mLinks.end())
            {
                it->second.mChangedQueued = false;
                changed.push_back(link_id);
            }
        }
        mChanged.clear();
    }
}
