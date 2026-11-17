/**
 * @file fsrecentappliedassets.h
 * @brief The textures and materials last applied to faces, for one click re-apply
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

#ifndef FS_RECENTAPPLIEDASSETS_H
#define FS_RECENTAPPLIEDASSETS_H

#include "llsingleton.h"
#include "lluuid.h"

#include <boost/signals2.hpp>
#include <string>
#include <vector>

class LLViewerInventoryItem;

// <FS:Trish> Recent textures and materials. Builders apply the same few textures and materials over
// and over, and finding them again in inventory or the picker each time is slow. This keeps the last
// ones applied to faces (from the texture panel's picker or dropped from inventory), most recent
// first, one entry per asset, and saves them per account so the list survives a relog.
// Only what was applied from an inventory item is kept, and only by its item id: asset ids are
// never shown or saved, since the viewer must not hand out the ids of textures the user has no
// full permissions on. The asset is looked up in inventory when it is needed.
class FSRecentAppliedAssets : public LLSingleton<FSRecentAppliedAssets>
{
    LLSINGLETON(FSRecentAppliedAssets);
    LOG_CLASS(FSRecentAppliedAssets);

public:
    enum EKind
    {
        KIND_TEXTURE = 0,
        KIND_MATERIAL = 1
    };

    struct Entry
    {
        LLUUID      mItemID;    // the inventory item it was applied from
        EKind       mKind = KIND_TEXTURE;
        std::string mName;      // the item's name when it was applied
    };
    typedef std::vector<Entry> entries_t;

    static constexpr size_t MAX_ENTRIES = 24;

    // A texture or material the user applied. Nothing is recorded without an inventory item.
    void record(EKind kind, const LLUUID& asset_id, const LLUUID& item_id);
    // A texture or material item dropped on a face
    void recordItem(const LLViewerInventoryItem* item);

    const entries_t& getEntries();
    const Entry* findEntry(const LLUUID& item_id);
    void clear();

    // The name to show: the item's current name when it is still in inventory
    std::string getDisplayName(const Entry& entry) const;
    // The entry's asset, from its item; null when the item has left inventory
    static LLUUID getAssetID(const Entry& entry);

    // True when the current selection can take a texture or material of this kind
    static bool canApply(EKind kind);

    enum EApplyResult
    {
        APPLY_OK,
        APPLY_NOT_EDITABLE,
        APPLY_ITEM_GONE,
        APPLY_FAILED
    };
    // Apply an entry to the selected faces, the same way the texture panel does
    EApplyResult apply(const LLUUID& item_id);

    typedef boost::signals2::signal<void()> changed_signal_t;
    boost::signals2::connection setChangedCallback(const changed_signal_t::slot_type& cb) { return mChangedSignal.connect(cb); }

private:
    bool isRecordable(EKind kind, const LLUUID& asset_id) const;
    void load();
    void save();

    entries_t           mEntries;
    std::string         mFilename;  // the file the entries were loaded from, empty before login
    changed_signal_t    mChangedSignal;
};
// </FS:Trish>

#endif // FS_RECENTAPPLIEDASSETS_H
