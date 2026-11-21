/**
 * @file fsinventorypositionhistory.h
 * @brief Back/forward history of positions in an inventory panel (FIRE-32887)
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
 * The Phoenix Firestorm Project, Inc., 1831 Oakwood Drive, Fairmont, Minnesota 56031-3225 USA
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */

#ifndef FS_INVENTORY_POSITION_HISTORY_H
#define FS_INVENTORY_POSITION_HISTORY_H

#include "llframetimer.h"
#include "lluuid.h"

#include <deque>
#include <string>
#include <vector>

class LLFolderView;
class LLInventoryPanel;

// Browser-like back/forward history for the normal (multi-folder) inventory tree.
// A position is the selection plus the scroll position of the folder view.
//
// An entry is recorded
//  - right before something else moves the selection (new item received, box
//    contents, upload landing, "Show in inventory", "Show original", ...), via
//    noteJump(), which LLInventoryPanel::setSelectionByID() calls, and
//  - when the user settles on a selection in another folder for a moment, so
//    clicking around inside one folder doesn't flood the history.
class FSInventoryPositionHistory
{
    LOG_CLASS(FSInventoryPositionHistory);

public:
    explicit FSInventoryPositionHistory(LLInventoryPanel* panel);

    static bool isEnabled();

    // The selection of the panel is about to be moved to target_id by code
    void noteJump(const LLUUID& target_id);

    // Called every frame the panel draws
    void update();

    bool canGoBack() const;
    bool canGoForward() const;
    bool goBack();
    bool goForward();

    // Name of the item Back/Forward would select (empty if unknown)
    std::string getBackName() const;
    std::string getForwardName() const;

private:
    struct Position
    {
        std::vector<LLUUID> mItems;     // selected items, primary selection first
        LLUUID              mFolderID;  // parent folder of the primary selection
        S32                 mScrollPos{ 0 };    // vertical scroll position
        S32                 mItemOffset{ 0 };   // primary item's distance from the top of the visible area
        bool                mHasOffset{ false };
        bool                mInTrash{ false };  // primary selection was in the trash already
        bool                mValid{ false };
    };
    typedef std::deque<Position> pos_list_t;

    LLFolderView* getRoot() const;
    LLUUID getPrimaryID() const;
    bool capture(Position& pos) const;
    void settle(const Position& pos);
    void pushEntry(pos_list_t& list, const Position& pos);
    LLUUID resolveTarget(const Position& pos, bool check_filter) const;
    const Position* findReachable(const pos_list_t& list, LLUUID* target_id) const;
    bool isAwayFromCurrent() const;
    bool navigate(pos_list_t& from, pos_list_t& to);
    bool restore(const Position& pos);
    bool clearFilterIfAllowed();
    void finishRestore(bool restored);

    LLInventoryPanel*   mPanel;
    pos_list_t          mBack;
    pos_list_t          mForward;
    Position            mCurrent;

    LLUUID              mLastSeenID;
    LLFrameTimer        mSettleTimer;
    bool                mSettlePending{ false };

    bool                mInRestoreCall{ false };
    bool                mRestoring{ false };
    bool                mRestoreExtrasDone{ false };
    S32                 mRestoreStableFrames{ 0 };
    LLUUID              mRestoreID;
    Position            mRestorePos;
    LLFrameTimer        mRestoreTimer;
};

#endif // FS_INVENTORY_POSITION_HISTORY_H
