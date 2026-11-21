/**
 * @file fscontentstypefilter.h
 * @brief Item type filter for the Content tab of the build floater
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
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

#ifndef FS_CONTENTSTYPEFILTER_H
#define FS_CONTENTSTYPEFILTER_H

#include "llhandle.h"

class LLMenuButton;
class LLPanel;
class LLPanelObjectInventory;
class LLSD;
class LLTextBox;
class LLView;

// "Show:" menu on the Content tab of the build floater. Narrows the object
// contents to some item types (objects, scripts, notecards, ...) through the
// inventory filter of the contents folder view, so nested folders, selection,
// drag and drop and the context menu keep working on what is shown.
// The choice is kept in FSBuildContentFilterTypes, one bit per entry of the menu.
class FSContentsTypeFilter
{
public:
    FSContentsTypeFilter(LLPanel* panel, LLPanelObjectInventory* inventory);
    ~FSContentsTypeFilter();

    // Refreshes the "N of M items shown" hint. Called a few times a second while the tab is drawn.
    void updateCount();

    // Every type shown
    static constexpr U32 ALL_TYPES = 0xfff;

private:
    void onMenuClick(const LLSD& param);
    bool onMenuCheck(const LLSD& param) const;
    void setMask(U32 mask);
    void apply();
    void updateButtonLabel();
    U32 getMask() const;

    LLPanel* mPanel;
    LLPanelObjectInventory* mInventory;
    LLMenuButton* mButton { nullptr };
    LLTextBox* mCountText { nullptr };
    LLHandle<LLView> mMenuHandle;
};

#endif // FS_CONTENTSTYPEFILTER_H
