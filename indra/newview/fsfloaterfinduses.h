/**
 * @file fsfloaterfinduses.h
 * @brief Lists the faces of objects in the region that use an inventory texture or material
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

#ifndef FS_FLOATERFINDUSES_H
#define FS_FLOATERFINDUSES_H

#include "llfloater.h"
#include "v3dmath.h"

#include <vector>

class LLScrollListCtrl;
class LLTextBox;
class LLViewerObject;

// <FS:Trish> Find uses nearby. From a texture or material in inventory, lists the faces of the objects
// in the agent's region that use it, nearest first, so a builder can see where it went and get back to
// them. Only the viewer's own copy of the region's objects is searched, with the asset id of the
// user's own item, so nothing is asked of the server and no id the user couldn't see is shown.
class FSFloaterFindUses : public LLFloater
{
public:
    FSFloaterFindUses(const LLSD& key);

    bool postBuild() override;

    // Search for the asset of this inventory item (a texture or a material), opening the window
    static void findUses(const LLUUID& item_id);

private:
    enum EChannel
    {
        CHANNEL_DIFFUSE,
        CHANNEL_NORMAL,
        CHANNEL_SPECULAR,
        CHANNEL_MATERIAL,       // the face's GLTF material is the one searched for
        CHANNEL_IN_MATERIAL,    // the texture is one of the maps of the face's GLTF material
        CHANNEL_OVERRIDE        // the texture is in the face's GLTF material overrides
    };

    struct Use
    {
        LLUUID      mObjectID;
        S32         mFace = 0;
        EChannel    mChannel = CHANNEL_DIFFUSE;
        F32         mDistance = 0.f;
        std::string mObjectName;
    };

    void search(const LLUUID& asset_id, const std::string& name, bool is_material);
    void searchObject(LLViewerObject* object, const LLVector3d& agent_pos);
    std::string objectName(LLViewerObject* object) const;
    std::string channelName(EChannel channel) const;
    void onClickRefresh();
    void onDoubleClick();

    LLUUID              mAssetID;
    std::string         mAssetName;
    bool                mIsMaterial = false;
    std::vector<Use>    mUses;

    LLScrollListCtrl*   mList = nullptr;
    LLTextBox*          mStatus = nullptr;
};
// </FS:Trish>

#endif // FS_FLOATERFINDUSES_H
