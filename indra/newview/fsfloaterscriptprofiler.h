/**
 * @file fsfloaterscriptprofiler.h
 * @brief Script time and memory of objects and avatars, refreshed through the LSL bridge
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

#ifndef FS_FLOATERSCRIPTPROFILER_H
#define FS_FLOATERSCRIPTPROFILER_H

#include "llfloater.h"
#include "lltimer.h"

#include <deque>
#include <map>

class LLScrollListCtrl;
class LLTextBox;

// <FS:Trish> Script profiler. "Script Info" asks the LSL bridge for an object's script count, memory and
// time (llGetObjectDetails) and prints the answer once in nearby chat. This window keeps a list of
// objects and avatars (an avatar's figures are the totals of its attachments) and asks again for each
// in turn, so their script time can be watched and compared. The server averages script time over a
// long window, so refreshing every few seconds is enough. One request is in flight at a time; its
// answer is taken from the bridge's chat (FSLSLBridge::lslToViewer) instead of being printed.
class FSFloaterScriptProfiler : public LLFloater
{
public:
    FSFloaterScriptProfiler(const LLSD& key);

    bool postBuild() override;
    void draw() override;

    // Add objects or avatars to the list, opening the window
    static void addTargets(const uuid_vec_t& ids);
    // Add the selected objects (their linksets) or avatar
    static void addSelection();

    // From FSLSLBridge::lslToViewer: an answer to "getScriptInfo". fields are its values, field_count
    // their number (27 for the extended answer, else an error). True when it answers the profiler's
    // request, which then isn't printed in chat.
    static bool handleBridgeReply(const LLSD& fields, S32 field_count);

private:
    struct Entry
    {
        std::string mName;
        bool        mIsAvatar = false;
        bool        mKnown = false;     // an answer arrived
        bool        mMissing = false;   // the last request found nothing (gone or out of range)
        S32         mRunning = 0;
        S32         mTotal = 0;
        F32         mMemoryKB = 0.f;
        F32         mTimeMs = 0.f;
        F32         mPeakMs = 0.f;
        std::deque<F32> mHistory;       // the last answers' time, oldest first
    };

    void addIds(const uuid_vec_t& ids);
    void queueAll();
    void sendNext();
    bool onReply(const LLSD& fields, S32 field_count);
    void refreshList();

    void onClickAddSelection();
    void onClickAddAvatars();
    void onClickRemove();
    void onClickClear();
    void onClickRefresh();
    void onDoubleClick();

    std::map<LLUUID, Entry> mEntries;
    std::deque<LLUUID>      mQueue;
    LLUUID                  mInFlight;
    LLTimer                 mInFlightTimer;
    LLTimer                 mCycleTimer;
    bool                    mListDirty = false;

    LLScrollListCtrl*       mList = nullptr;
    LLTextBox*              mStatus = nullptr;
};
// </FS:Trish>

#endif // FS_FLOATERSCRIPTPROFILER_H
