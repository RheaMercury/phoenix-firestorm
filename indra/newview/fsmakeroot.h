/**
 * @file fsmakeroot.h
 * @brief Make a linked part the root of its linkset by relinking it first
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, Trish_sl
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
 * $/LicenseInfo$
 */

#ifndef FS_MAKEROOT_H
#define FS_MAKEROOT_H

#include "llsingleton.h"
#include "lltimer.h"
#include "lluuid.h"

class LLViewerObject;

class FSMakeRoot : public LLSingleton<FSMakeRoot>
{
    LLSINGLETON(FSMakeRoot);
    ~FSMakeRoot();

public:
    // The selected linked part, if it can be made the root of its linkset now
    LLViewerObject* getCandidate(std::string* reason = nullptr) const;
    bool canMakeRoot() const { return getCandidate() != nullptr; }
    // Asks for confirmation, then unlinks the linkset and relinks it with the selected part first
    void makeRoot();
    bool isBusy() const { return mState != STATE_IDLE; }

private:
    enum EState
    {
        STATE_IDLE,
        STATE_WAIT_UNLINK,
        STATE_WAIT_LINK,
    };

    void start(const LLUUID& new_root_id);
    static void onIdle(void* data);
    void update();
    void finish(bool success, const std::string& reason = std::string());

    EState              mState = STATE_IDLE;
    LLUUID              mNewRootID;
    std::vector<LLUUID> mPrimIDs;   // every prim of the linkset, new root first
    LLFrameTimer        mTimer;
};

// Deletes selected linked parts, leaving the rest of their linksets in place. The simulator only
// deletes whole objects, so the parts are unlinked first, then deleted as objects of their own the
// usual way (to the trash, with the usual confirmations for locked or no copy objects).
class FSDeleteLinkPart : public LLSingleton<FSDeleteLinkPart>
{
    LLSINGLETON(FSDeleteLinkPart);
    ~FSDeleteLinkPart();

public:
    // The selected linked parts (Edit linked on), if they can be deleted from their linksets now
    std::vector<LLViewerObject*> getCandidates(std::string* reason = nullptr) const;
    bool canDelete() const { return !getCandidates().empty(); }
    // Asks for confirmation, then unlinks and deletes the selected parts
    void deleteParts();
    bool isBusy() const { return mWaiting; }

private:
    void start(const std::vector<LLUUID>& part_ids);
    static void onIdle(void* data);
    void update();
    void finish(const std::string& failure = std::string());

    bool                mWaiting = false;   // for the region to unlink the parts
    std::vector<LLUUID> mPartIDs;
    LLFrameTimer        mTimer;
};

#endif // FS_MAKEROOT_H
