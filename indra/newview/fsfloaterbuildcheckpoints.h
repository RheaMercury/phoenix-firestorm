/**
 * @file fsfloaterbuildcheckpoints.h
 * @brief Named snapshots of the transforms of selected objects, restored in one click
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

#ifndef FS_FLOATERBUILDCHECKPOINTS_H
#define FS_FLOATERBUILDCHECKPOINTS_H

#include "llfloater.h"

class LLScrollListCtrl;

// <FS:Trish> Build checkpoints. Named snapshots of the position, rotation and scale of every prim of
// the selected linksets, kept per account in build_checkpoints.xml. Restore puts the objects that are
// still around and modifiable back, sending the same MultipleObjectUpdate the build tools send, and
// records it in the build undo history (FSBuildHistory) so Ctrl+Z takes it back.
class FSFloaterBuildCheckpoints : public LLFloater
{
public:
    FSFloaterBuildCheckpoints(const LLSD& key);

    bool postBuild() override;

    // Opens the window and asks for a name for a checkpoint of the current selection
    static void saveSelection();

private:
    void load();
    void save() const;
    void refreshList(const LLUUID& select_id = LLUUID::null);
    void updateButtons();
    void setStatus(const std::string& text);
    LLUUID selectedId() const;
    S32 findIndex(const LLUUID& id) const;

    void onClickSave();
    void onClickRestore();
    void onClickSelect();
    void onClickRename();
    void onClickDelete();

    void addCheckpoint(std::string name, LLSD checkpoint);
    void restoreCheckpoint(const LLUUID& id);
    void renameCheckpoint(const LLUUID& id, std::string name);
    void deleteCheckpoint(const LLUUID& id);

    // array of maps: id, name, date, region, region_handle, objects (array of maps, see captureSelection)
    LLSD                mCheckpoints;
    LLScrollListCtrl*   mList = nullptr;
};
// </FS:Trish>

#endif // FS_FLOATERBUILDCHECKPOINTS_H
