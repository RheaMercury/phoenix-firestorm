/**
 * @file fsfloaterrecentassets.cpp
 * @brief Lists the textures and materials last applied to faces and applies them again
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

#include "fsfloaterrecentassets.h"

#include "fsrecentappliedassets.h"
#include "llbutton.h"
#include "llfetchedgltfmaterial.h"
#include "llgltfmateriallist.h"
#include "lliconctrl.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "lltextbox.h"

namespace
{
    constexpr char MATERIAL_ICON[] = "Inv_Material";
}

FSFloaterRecentAssets::FSFloaterRecentAssets(const LLSD& key)
:   LLFloater(key)
{
}

FSFloaterRecentAssets::~FSFloaterRecentAssets()
{
    mChangedConnection.disconnect();
}

bool FSFloaterRecentAssets::postBuild()
{
    mList = getChild<LLScrollListCtrl>("entries");
    mPreview = getChild<LLIconCtrl>("preview");
    mStatus = getChild<LLTextBox>("status");
    mApplyBtn = getChild<LLButton>("apply");

    mList->setCommitOnSelectionChange(true);
    mList->setCommitCallback([this](LLUICtrl*, const LLSD&) { updatePreview(); });
    mList->setDoubleClickCallback([this]() { onClickApply(); });

    mApplyBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickApply(); });
    getChild<LLUICtrl>("clear")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickClear(); });

    mChangedConnection = FSRecentAppliedAssets::instance().setChangedCallback([this]() { refreshList(); });
    return true;
}

void FSFloaterRecentAssets::onOpen(const LLSD& key)
{
    mStatus->setText(LLStringUtil::null);
    refreshList();
}

void FSFloaterRecentAssets::draw()
{
    // The selection changes without telling this window, and checking it is cheap
    LLScrollListItem* item = mList->getFirstSelected();
    const FSRecentAppliedAssets::Entry* entry = item ? FSRecentAppliedAssets::instance().findEntry(item->getUUID()) : nullptr;
    mApplyBtn->setEnabled(entry && FSRecentAppliedAssets::canApply(entry->mKind));

    if (mPendingMaterial.notNull() && !mPendingMaterial->isFetching())
    {
        updatePreview();
    }

    LLFloater::draw();
}

void FSFloaterRecentAssets::refreshList()
{
    const LLUUID selected = mList->getCurrentID();
    mList->deleteAllItems();

    for (const FSRecentAppliedAssets::Entry& entry : FSRecentAppliedAssets::instance().getEntries())
    {
        LLSD row;
        row["value"] = entry.mItemID;
        LLSD& columns = row["columns"];
        columns[0]["column"] = "name";
        columns[0]["value"] = FSRecentAppliedAssets::instance().getDisplayName(entry);
        columns[1]["column"] = "kind";
        columns[1]["value"] = entry.mKind == FSRecentAppliedAssets::KIND_MATERIAL ? getString("kind_material") : getString("kind_texture");
        mList->addElement(row);
    }

    if (selected.isNull() || !mList->selectByID(selected))
    {
        mList->selectFirstItem();
    }
    updatePreview();
}

void FSFloaterRecentAssets::updatePreview()
{
    mPendingMaterial = nullptr;

    LLScrollListItem* item = mList->getFirstSelected();
    const FSRecentAppliedAssets::Entry* entry = item ? FSRecentAppliedAssets::instance().findEntry(item->getUUID()) : nullptr;
    const LLUUID asset_id = entry ? FSRecentAppliedAssets::getAssetID(*entry) : LLUUID::null;
    if (asset_id.isNull())
    {
        // nothing selected, or its item has left inventory
        const bool material = entry && entry->mKind == FSRecentAppliedAssets::KIND_MATERIAL;
        mPreview->setValue(material ? LLSD(MATERIAL_ICON) : LLSD());
        return;
    }

    if (entry->mKind == FSRecentAppliedAssets::KIND_TEXTURE)
    {
        mPreview->setValue(asset_id);
        return;
    }

    // A material has no picture of its own: show its base color texture once it has loaded, the
    // material icon until then or when it has none.
    LLFetchedGLTFMaterial* material = gGLTFMaterialList.getMaterial(asset_id);
    if (material && material->isFetching())
    {
        mPendingMaterial = material;
        mPreview->setValue(MATERIAL_ICON);
        return;
    }
    LLUUID base_color;
    if (material && material->isLoaded())
    {
        base_color = material->mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR];
    }
    if (base_color.notNull())
    {
        mPreview->setValue(base_color);
    }
    else
    {
        mPreview->setValue(MATERIAL_ICON);
    }
}

void FSFloaterRecentAssets::onClickApply()
{
    LLScrollListItem* item = mList->getFirstSelected();
    if (!item)
    {
        return;
    }

    switch (FSRecentAppliedAssets::instance().apply(item->getUUID()))
    {
        case FSRecentAppliedAssets::APPLY_OK:
            mStatus->setText(LLStringUtil::null);
            break;
        case FSRecentAppliedAssets::APPLY_NOT_EDITABLE:
            mStatus->setText(getString("not_editable"));
            break;
        case FSRecentAppliedAssets::APPLY_ITEM_GONE:
            mStatus->setText(getString("item_gone"));
            break;
        case FSRecentAppliedAssets::APPLY_FAILED:
        default:
            mStatus->setText(getString("apply_failed"));
            break;
    }
}

void FSFloaterRecentAssets::onClickClear()
{
    FSRecentAppliedAssets::instance().clear();
    mStatus->setText(LLStringUtil::null);
}
