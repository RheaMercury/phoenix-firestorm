/**
 * @file fsfloaterrecentassets.h
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

#ifndef FS_FLOATERRECENTASSETS_H
#define FS_FLOATERRECENTASSETS_H

#include "llfloater.h"
#include "llpointer.h"

class LLButton;
class LLFetchedGLTFMaterial;
class LLIconCtrl;
class LLScrollListCtrl;
class LLTextBox;

// <FS:Trish> Recent textures and materials (FSRecentAppliedAssets), with a picture of the selected one.
// Apply, or a double-click, puts it on the selected faces.
class FSFloaterRecentAssets : public LLFloater
{
public:
    FSFloaterRecentAssets(const LLSD& key);
    ~FSFloaterRecentAssets() override;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

private:
    void refreshList();
    void updatePreview();
    void onClickApply();
    void onClickClear();

    LLScrollListCtrl*   mList = nullptr;
    LLIconCtrl*         mPreview = nullptr;
    LLTextBox*          mStatus = nullptr;
    LLButton*           mApplyBtn = nullptr;

    // a material's picture is its base color texture, known once the material has loaded
    LLPointer<LLFetchedGLTFMaterial> mPendingMaterial;

    boost::signals2::connection mChangedConnection;
};
// </FS:Trish>

#endif // FS_FLOATERRECENTASSETS_H
