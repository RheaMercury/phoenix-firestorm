/**
 * @file fsrecentappliedassets.cpp
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

#include "llviewerprecompiledheaders.h"

#include "fsrecentappliedassets.h"

#include "llagent.h"
#include "llavatarappearancedefines.h"
#include "lldir.h"
#include "llinventorymodel.h"
#include "lllocalbitmaps.h"
#include "lllocalgltfmaterials.h"
#include "llmaterialeditor.h"
#include "llsdserialize.h"
#include "llselectmgr.h"
#include "llviewerinventory.h"
#include "llviewerobject.h"

namespace
{
    constexpr char FILENAME[] = "recent_applied_assets.xml";
}

FSRecentAppliedAssets::FSRecentAppliedAssets()
{
}

bool FSRecentAppliedAssets::isRecordable(EKind kind, const LLUUID& asset_id) const
{
    if (asset_id.isNull())
    {
        return false;
    }

    if (kind == KIND_MATERIAL)
    {
        // the blank material is a button in the picker, and a local material exists only on this computer
        return asset_id != BLANK_MATERIAL_ASSET_ID && !LLLocalGLTFMaterialMgr::getInstance()->isLocal(asset_id);
    }

    // The picker's own buttons (default, blank, transparent) are one click away already, the alpha
    // gradient is what the panel itself applies for planar media, baked ids belong to avatars, and a
    // local texture exists only on this computer.
    if (asset_id == DEFAULT_OBJECT_TEXTURE || asset_id == IMG_DEFAULT || asset_id == IMG_WHITE
        || asset_id == IMG_INVISIBLE || asset_id == IMG_TRANSPARENT
        || asset_id == IMG_ALPHA_GRAD || asset_id == IMG_ALPHA_GRAD_2D)
    {
        return false;
    }
    if (LLAvatarAppearanceDefines::LLAvatarAppearanceDictionary::isBakedImageId(asset_id))
    {
        return false;
    }
    return !LLLocalBitmapMgr::getInstance()->isLocal(asset_id);
}

void FSRecentAppliedAssets::record(EKind kind, const LLUUID& asset_id, const LLUUID& item_id)
{
    if (!isRecordable(kind, asset_id))
    {
        return;
    }

    // Only from an inventory item: an asset picked by its id has nothing to name it by but the id,
    // and keeping that would hand it out.
    const LLViewerInventoryItem* item = item_id.notNull() ? gInventory.getItem(item_id) : nullptr;
    if (!item || item->getAssetUUID() != asset_id)
    {
        return;
    }
    // A no copy item leaves inventory for the object it is applied to, so it could never be
    // applied again from here.
    if (!item->getPermissions().allowOperationBy(PERM_COPY, gAgent.getID()))
    {
        return;
    }

    load();

    Entry entry;
    entry.mItemID = item_id;
    entry.mKind = kind;
    entry.mName = item->getName();

    // one entry per asset (and per item), the most recent first
    mEntries.erase(std::remove_if(mEntries.begin(), mEntries.end(),
                                  [&](const Entry& e) { return e.mItemID == item_id || getAssetID(e) == asset_id; }),
                   mEntries.end());
    mEntries.insert(mEntries.begin(), entry);
    if (mEntries.size() > MAX_ENTRIES)
    {
        mEntries.resize(MAX_ENTRIES);
    }

    save();
    mChangedSignal();
}

void FSRecentAppliedAssets::recordItem(const LLViewerInventoryItem* item)
{
    if (!item)
    {
        return;
    }
    const LLAssetType::EType type = item->getType();
    if (type == LLAssetType::AT_TEXTURE)
    {
        record(KIND_TEXTURE, item->getAssetUUID(), item->getUUID());
    }
    else if (type == LLAssetType::AT_MATERIAL)
    {
        record(KIND_MATERIAL, item->getAssetUUID(), item->getUUID());
    }
}

const FSRecentAppliedAssets::entries_t& FSRecentAppliedAssets::getEntries()
{
    load();
    return mEntries;
}

const FSRecentAppliedAssets::Entry* FSRecentAppliedAssets::findEntry(const LLUUID& item_id)
{
    load();
    for (const Entry& entry : mEntries)
    {
        if (entry.mItemID == item_id)
        {
            return &entry;
        }
    }
    return nullptr;
}

void FSRecentAppliedAssets::clear()
{
    load();
    mEntries.clear();
    save();
    mChangedSignal();
}

std::string FSRecentAppliedAssets::getDisplayName(const Entry& entry) const
{
    // follow renames
    if (const LLViewerInventoryItem* item = gInventory.getItem(entry.mItemID))
    {
        return item->getName();
    }
    return entry.mName;
}

// static
LLUUID FSRecentAppliedAssets::getAssetID(const Entry& entry)
{
    const LLViewerInventoryItem* item = gInventory.getItem(entry.mItemID);
    return item ? item->getAssetUUID() : LLUUID::null;
}

// static
bool FSRecentAppliedAssets::canApply(EKind kind)
{
    // the same test the texture panel uses to enable its pickers
    LLObjectSelectionHandle selection = LLSelectMgr::getInstance()->getSelection();
    if (selection->isEmpty())
    {
        return false;
    }
    LLViewerObject* object = selection->getFirstObject();
    if (!object || object->getPCode() != LL_PCODE_VOLUME || !object->permModify() || object->isPermanentEnforced())
    {
        return false;
    }
    if (!LLSelectMgr::getInstance()->selectGetModify())
    {
        return false;
    }
    return kind == KIND_TEXTURE || LLMaterialEditor::capabilitiesAvailable();
}

FSRecentAppliedAssets::EApplyResult FSRecentAppliedAssets::apply(const LLUUID& item_id)
{
    const Entry* found = findEntry(item_id);
    if (!found)
    {
        return APPLY_FAILED;
    }
    // copied, recording it again below reorders the list
    const Entry entry = *found;

    if (!canApply(entry.mKind))
    {
        return APPLY_NOT_EDITABLE;
    }

    // Like the panel, hand the selection manager the item id, so that it applies the item's
    // permission rules; once the item has left inventory there is nothing to apply.
    const LLUUID asset_id = getAssetID(entry);
    if (asset_id.isNull())
    {
        return APPLY_ITEM_GONE;
    }

    LLSelectMgr* select_mgr = LLSelectMgr::getInstance();
    // as the panel does when the picker's OK is pressed
    select_mgr->saveSelectedObjectTextures();
    const bool applied = entry.mKind == KIND_TEXTURE ? select_mgr->selectionSetImage(entry.mItemID)
                                                     : select_mgr->selectionSetGLTFMaterial(entry.mItemID);
    if (!applied)
    {
        return APPLY_FAILED;
    }

    record(entry.mKind, asset_id, entry.mItemID);
    return APPLY_OK;
}

void FSRecentAppliedAssets::load()
{
    // The per account folder is only known once logged in, so the file is read on first use after
    // that, and again should the account change.
    if (gDirUtilp->getLindenUserDir().empty())
    {
        return;
    }
    const std::string filename = gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, FILENAME);
    if (filename == mFilename)
    {
        return;
    }
    mFilename = filename;
    mEntries.clear();

    if (!LLFile::isfile(filename))
    {
        return;
    }
    llifstream file(filename.c_str());
    if (!file.is_open())
    {
        LL_WARNS() << "Can't open " << filename << LL_ENDL;
        return;
    }
    LLSD data;
    LLSDSerialize::fromXMLDocument(data, file);
    file.close();

    // A file from before entries were kept by item id only also holds asset ids, and entries picked
    // by asset id: those are dropped, and the file is written again without them.
    bool rewrite = false;
    for (LLSD::array_const_iterator it = data.beginArray(); it != data.endArray(); ++it)
    {
        const LLSD& row = *it;
        rewrite |= row.has("asset_id");
        Entry entry;
        entry.mItemID = row["item_id"].asUUID();
        entry.mKind = row["kind"].asString() == "material" ? KIND_MATERIAL : KIND_TEXTURE;
        entry.mName = row["name"].asString();
        if (entry.mItemID.isNull() || mEntries.size() >= MAX_ENTRIES)
        {
            rewrite = true;
            continue;
        }
        mEntries.push_back(entry);
    }
    if (rewrite)
    {
        save();
    }
}

void FSRecentAppliedAssets::save()
{
    if (mFilename.empty())
    {
        return;
    }

    LLSD data(LLSD::emptyArray());
    for (const Entry& entry : mEntries)
    {
        LLSD row;
        row["item_id"] = entry.mItemID;
        row["kind"] = entry.mKind == KIND_MATERIAL ? "material" : "texture";
        row["name"] = entry.mName;
        data.append(row);
    }

    llofstream file(mFilename.c_str());
    if (!file.is_open())
    {
        LL_WARNS() << "Can't save " << mFilename << LL_ENDL;
        return;
    }
    LLSDSerialize::toPrettyXML(data, file);
    file.close();
}
