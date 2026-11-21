/**
 * @file fsinventorypositionhistory.cpp
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

#include "llviewerprecompiledheaders.h"

#include "fsinventorypositionhistory.h"

#include "llfolderview.h"
#include "llfolderviewitem.h"
#include "llfolderviewmodelinventory.h"
#include "llinventoryfilter.h"
#include "llinventorymodel.h"
#include "llinventorypanel.h"
#include "llpanelmaininventory.h"
#include "llscrollbar.h"
#include "llscrollcontainer.h"
#include "llviewercontrol.h"
#include "rlvactions.h"

namespace
{
    constexpr size_t MAX_ENTRIES = 20;          // per direction
    constexpr size_t MAX_SELECTED_ITEMS = 64;   // remembered per position
    constexpr F32 SETTLE_SECONDS = 1.f;         // a selection has to stay this long to become a position
    constexpr F32 RESTORE_TIMEOUT = 10.f;       // give up waiting for a restored selection to show up
    constexpr S32 RESTORE_STABLE_FRAMES = 2;    // frames without rearranging before the scroll is restored

    LLUUID get_view_uuid(LLFolderViewItem* item)
    {
        LLFolderViewModelItemInventory* vmi = item ? static_cast<LLFolderViewModelItemInventory*>(item->getViewModelItem()) : nullptr;
        return vmi ? vmi->getUUID() : LLUUID::null;
    }

    bool is_in_trash(const LLUUID& id)
    {
        const LLUUID trash_id = gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH);
        return trash_id.notNull() && (id == trash_id || gInventory.isObjectDescendentOf(id, trash_id));
    }

    bool is_inventory_hidden()
    {
        return RlvActions::isRlvEnabled() && RlvActions::hasBehaviour(RLV_BHVR_SHOWINV);
    }

    // Distance of the top of an item from the top of the folder view
    S32 get_item_doc_top(LLFolderView* root, LLFolderViewItem* item)
    {
        LLRect item_rect;
        item->localRectToOtherView(item->getLocalRect(), &item_rect, root);
        return root->getRect().getHeight() - item_rect.mTop;
    }
}

FSInventoryPositionHistory::FSInventoryPositionHistory(LLInventoryPanel* panel)
:   mPanel(panel)
{
}

// static
bool FSInventoryPositionHistory::isEnabled()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "FSInventoryPositionHistory", true);
    return enabled;
}

LLFolderView* FSInventoryPositionHistory::getRoot() const
{
    return mPanel ? mPanel->getRootFolder() : nullptr;
}

LLUUID FSInventoryPositionHistory::getPrimaryID() const
{
    LLFolderView* root = getRoot();
    if (!root)
    {
        return LLUUID::null;
    }
    LLFolderViewItem* item = root->getCurSelectedItem();
    if (!item || item == root)
    {
        return LLUUID::null;
    }
    return get_view_uuid(item);
}

bool FSInventoryPositionHistory::capture(Position& pos) const
{
    pos = Position();

    LLFolderView* root = getRoot();
    if (!root)
    {
        return false;
    }

    LLScrollContainer* scroller = root->getScrollContainer();
    pos.mScrollPos = scroller ? scroller->getDocPosVertical() : 0;

    LLFolderViewItem* primary = root->getCurSelectedItem();
    const LLUUID primary_id = (primary && primary != root) ? get_view_uuid(primary) : LLUUID::null;
    if (primary_id.notNull())
    {
        pos.mItems.push_back(primary_id);

        const LLInventoryObject* obj = gInventory.getObject(primary_id);
        pos.mFolderID = obj ? obj->getParentUUID() : LLUUID::null;
        pos.mInTrash = is_in_trash(primary_id);

        if (scroller)
        {
            pos.mItemOffset = get_item_doc_top(root, primary) - pos.mScrollPos;
            pos.mHasOffset = true;
        }

        for (LLFolderViewItem* item : root->getSelectionList())
        {
            if (pos.mItems.size() >= MAX_SELECTED_ITEMS)
            {
                break;
            }
            if (item && item != root && item != primary)
            {
                const LLUUID id = get_view_uuid(item);
                if (id.notNull())
                {
                    pos.mItems.push_back(id);
                }
            }
        }
    }

    // A position without a selection is still worth going back to if it was scrolled
    pos.mValid = !pos.mItems.empty() || pos.mScrollPos > 0;
    return pos.mValid;
}

void FSInventoryPositionHistory::pushEntry(pos_list_t& list, const Position& pos)
{
    if (!pos.mValid)
    {
        return;
    }

    // Several jumps into the same folder in a row (e.g. a batch of received items) collapse into one entry
    if (!list.empty() && list.back().mFolderID == pos.mFolderID)
    {
        list.back() = pos;
        return;
    }

    list.push_back(pos);
    while (list.size() > MAX_ENTRIES)
    {
        list.pop_front();
    }
}

void FSInventoryPositionHistory::settle(const Position& pos)
{
    if (!pos.mValid)
    {
        return;
    }

    // Moving around inside the same folder only refreshes the current position
    if (!mCurrent.mValid || pos.mFolderID == mCurrent.mFolderID)
    {
        mCurrent = pos;
        return;
    }

    pushEntry(mBack, mCurrent);
    mForward.clear();
    mCurrent = pos;
}

void FSInventoryPositionHistory::noteJump(const LLUUID& target_id)
{
    if (mInRestoreCall || !isEnabled() || !getRoot())
    {
        return;
    }

    if (mRestoring)
    {
        finishRestore(false);
    }

    Position here;
    if (capture(here))
    {
        if (!here.mItems.empty() && here.mItems.front() == target_id)
        {
            // Already there, nothing moves
            return;
        }
        settle(here);
    }

    if (mCurrent.mValid)
    {
        pushEntry(mBack, mCurrent);
        mForward.clear();
    }

    // The jump target becomes the current position once it has settled
    mCurrent = Position();
    mSettlePending = false;
    mLastSeenID = getPrimaryID();
}

void FSInventoryPositionHistory::update()
{
    LLFolderView* root = getRoot();
    if (!root || !isEnabled())
    {
        return;
    }

    const LLUUID current_id = getPrimaryID();

    if (mRestoring)
    {
        if (mRestoreTimer.getElapsedTimeF32() > RESTORE_TIMEOUT)
        {
            finishRestore(false);
            return;
        }

        if (current_id != mRestoreID)
        {
            // Still waiting for the item to show up, unless something else took over the selection meanwhile
            if (mRestoreID.isNull() || mPanel->getPendingSelectionID() != mRestoreID)
            {
                finishRestore(false);
            }
            mRestoreStableFrames = 0;
            return;
        }

        if (!mRestoreExtrasDone)
        {
            mRestoreExtrasDone = true;
            mRestoreStableFrames = 0;
            if (mRestoreID.notNull() && !mRestorePos.mItems.empty() && mRestorePos.mItems.front() == mRestoreID)
            {
                for (size_t i = 1; i < mRestorePos.mItems.size(); ++i)
                {
                    LLFolderViewItem* view = mPanel->getItemByID(mRestorePos.mItems[i]);
                    if (view && view != root && view->getViewModelItem() && view->passedFilter() && !view->isSelected())
                    {
                        if (view->getParentFolder())
                        {
                            view->getParentFolder()->setOpenArrangeRecursively(true, LLFolderViewFolder::RECURSE_UP);
                        }
                        root->changeSelection(view, true);
                    }
                }
            }
            return;
        }

        if (root->needsArrange())
        {
            mRestoreStableFrames = 0;
            return;
        }

        if (++mRestoreStableFrames >= RESTORE_STABLE_FRAMES)
        {
            finishRestore(true);
        }
        return;
    }

    if (current_id != mLastSeenID)
    {
        mLastSeenID = current_id;
        mSettlePending = current_id.notNull();
        mSettleTimer.reset();
        return;
    }

    if (mSettlePending && mSettleTimer.getElapsedTimeF32() >= SETTLE_SECONDS)
    {
        mSettlePending = false;
        Position here;
        if (capture(here))
        {
            settle(here);
        }
    }
}

LLUUID FSInventoryPositionHistory::resolveTarget(const Position& pos, bool check_filter) const
{
    LLFolderView* root = getRoot();
    if (!root || pos.mItems.empty())
    {
        return LLUUID::null;
    }

    // The item itself first; if it is gone (deleted, purged or out of this panel's view), its folder
    const LLUUID candidates[] = { pos.mItems.front(), pos.mFolderID };
    for (const LLUUID& id : candidates)
    {
        if (id.isNull() || !gInventory.getObject(id))
        {
            continue;
        }
        if (!pos.mInTrash && is_in_trash(id))
        {
            // Deleted since; don't follow it into the trash
            continue;
        }
        LLFolderViewItem* view = mPanel->getItemByID(id);
        if (!view || view == root || !view->getViewModelItem())
        {
            continue;
        }
        if (check_filter && !view->passedFilter())
        {
            continue;
        }
        return id;
    }
    return LLUUID::null;
}

const FSInventoryPositionHistory::Position* FSInventoryPositionHistory::findReachable(const pos_list_t& list, LLUUID* target_id) const
{
    for (auto it = list.rbegin(); it != list.rend(); ++it)
    {
        if (it->mItems.empty())
        {
            if (it->mValid)
            {
                if (target_id)
                {
                    target_id->setNull();
                }
                return &(*it);
            }
            continue;
        }

        const LLUUID id = resolveTarget(*it, false);
        if (id.notNull())
        {
            if (target_id)
            {
                *target_id = id;
            }
            return &(*it);
        }
    }
    return nullptr;
}

bool FSInventoryPositionHistory::isAwayFromCurrent() const
{
    // The selection moved to another folder but hasn't settled there yet
    if (!mCurrent.mValid || mRestoring || !mSettlePending)
    {
        return false;
    }
    const LLInventoryObject* obj = gInventory.getObject(getPrimaryID());
    return obj && obj->getParentUUID() != mCurrent.mFolderID;
}

bool FSInventoryPositionHistory::canGoBack() const
{
    if (!isEnabled() || is_inventory_hidden() || !getRoot())
    {
        return false;
    }
    // Not yet settled in another folder: Back returns to the current position
    if (isAwayFromCurrent() && resolveTarget(mCurrent, false).notNull())
    {
        return true;
    }
    return findReachable(mBack, nullptr) != nullptr;
}

bool FSInventoryPositionHistory::canGoForward() const
{
    if (!isEnabled() || is_inventory_hidden() || !getRoot())
    {
        return false;
    }
    // Moving to another folder starts a new branch; going Back first brings Forward back
    if (isAwayFromCurrent())
    {
        return false;
    }
    return findReachable(mForward, nullptr) != nullptr;
}

std::string FSInventoryPositionHistory::getBackName() const
{
    LLUUID id;
    if (isAwayFromCurrent())
    {
        id = resolveTarget(mCurrent, false);
    }
    if (id.isNull())
    {
        findReachable(mBack, &id);
    }
    const LLInventoryObject* obj = id.notNull() ? gInventory.getObject(id) : nullptr;
    return obj ? obj->getName() : LLStringUtil::null;
}

std::string FSInventoryPositionHistory::getForwardName() const
{
    LLUUID id;
    if (!isAwayFromCurrent())
    {
        findReachable(mForward, &id);
    }
    const LLInventoryObject* obj = id.notNull() ? gInventory.getObject(id) : nullptr;
    return obj ? obj->getName() : LLStringUtil::null;
}

bool FSInventoryPositionHistory::goBack()
{
    return navigate(mBack, mForward);
}

bool FSInventoryPositionHistory::goForward()
{
    return navigate(mForward, mBack);
}

bool FSInventoryPositionHistory::navigate(pos_list_t& from, pos_list_t& to)
{
    if (!isEnabled() || is_inventory_hidden() || !getRoot())
    {
        return false;
    }

    if (mRestoring)
    {
        finishRestore(false);
    }

    // Bring the position we are leaving up to date (this may record it if the user just moved to another folder)
    Position here;
    if (capture(here))
    {
        settle(here);
    }
    mSettlePending = false;

    while (!from.empty())
    {
        Position target = from.back();
        from.pop_back();

        if (!restore(target))
        {
            // Nothing left of this position; drop it and try the next one
            continue;
        }

        if (mCurrent.mValid)
        {
            pushEntry(to, mCurrent);
        }
        mCurrent = target;
        return true;
    }
    return false;
}

bool FSInventoryPositionHistory::clearFilterIfAllowed()
{
    // Only the All Items tab of the main inventory gets its filter cleared; other tabs
    // (Recent, Worn, Favorites, custom tabs) are defined by their filter.
    if (!mPanel->getFilter().isNotDefault())
    {
        return false;
    }
    LLPanelMainInventory* main_inventory = mPanel->getParentByType<LLPanelMainInventory>();
    if (!main_inventory
        || main_inventory->isSingleFolderMode()
        || main_inventory->getActivePanel() != mPanel
        || main_inventory->getAllItemsPanel() != mPanel)
    {
        return false;
    }
    main_inventory->resetFilters();
    main_inventory->onFilterEdit(LLStringUtil::null);
    return true;
}

bool FSInventoryPositionHistory::restore(const Position& pos)
{
    LLFolderView* root = getRoot();
    if (!root || !pos.mValid)
    {
        return false;
    }

    LLUUID target_id;
    if (!pos.mItems.empty())
    {
        target_id = resolveTarget(pos, false);
        if (target_id.isNull())
        {
            return false;
        }

        // Clear a filter or search only if it hides the target
        LLFolderViewItem* view = mPanel->getItemByID(target_id);
        if (view && !view->passedFilter() && !clearFilterIfAllowed())
        {
            target_id = resolveTarget(pos, true);
            if (target_id.isNull())
            {
                return false;
            }
        }
    }

    mInRestoreCall = true;
    if (target_id.notNull())
    {
        // Opens the folders above it; waits for it if it isn't shown yet
        mPanel->setSelectionByID(target_id, true);
    }
    else
    {
        root->clearSelection();
    }
    mInRestoreCall = false;

    mRestoring = true;
    mRestoreExtrasDone = false;
    mRestoreStableFrames = 0;
    mRestoreID = target_id;
    mRestorePos = pos;
    mRestoreTimer.reset();
    mSettlePending = false;
    return true;
}

void FSInventoryPositionHistory::finishRestore(bool restored)
{
    mRestoring = false;

    LLFolderView* root = getRoot();
    if (!root)
    {
        return;
    }

    if (!restored)
    {
        // Don't leave a selection request behind that could pop up later
        if (mRestoreID.notNull() && mPanel->getPendingSelectionID() == mRestoreID)
        {
            mPanel->clearSelection();
        }
        return;
    }

    LLScrollContainer* scroller = root->getScrollContainer();
    LLScrollbar* scrollbar = scroller ? scroller->getScrollbar(LLScrollContainer::VERTICAL) : nullptr;
    if (scrollbar)
    {
        S32 doc_pos = mRestorePos.mScrollPos;
        LLFolderViewItem* item = mRestoreID.notNull() ? mPanel->getItemByID(mRestoreID) : nullptr;
        if (item && mRestorePos.mHasOffset && mRestorePos.mItems.front() == mRestoreID)
        {
            // Put the item back where it was in the window, even if things above it changed
            doc_pos = get_item_doc_top(root, item) - mRestorePos.mItemOffset;
        }
        scrollbar->setDocPos(doc_pos);
    }

    mLastSeenID = getPrimaryID();
    mSettlePending = false;
}
