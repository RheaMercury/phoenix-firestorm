/**
 * @file fsfloaterbuildcost.h
 * @brief Build cost tools: a linkset's cost prim by prim, and what nearby objects and avatars cost to render
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

#ifndef FS_FLOATERBUILDCOST_H
#define FS_FLOATERBUILDCOST_H

#include "llfloater.h"
#include "llaccountingcostmanager.h"
#include "llselectmgr.h"
#include "lltimer.h"
#include "v3dmath.h"

#include <map>
#include <memory>
#include <set>
#include <vector>

class LLCheckBoxCtrl;
class LLListContextMenu;
class LLMessageSystem;
class LLScrollListCtrl;
class LLSpinCtrl;
class LLTextBox;
class LLViewerObject;
class LLVOAvatar;

// <FS:Trish> Linkset cost breakdown. The build window shows a linkset's land impact, download, physics
// and server weights as totals only. This window lists every prim of the selected linkset with its
// own share (the ObjectCost capability's per prim answer), its download weight (worked out by the
// viewer), its triangles and vertices at the level of detail drawn now and its textures, most costly
// first, so a builder can find the prim that weighs. The server's answers arrive later: until then a
// figure is shown as pending, and the list is read again every second while the window is open.
// The server weight is only known for the whole linkset (the ResourceCostSelected capability).
class FSFloaterLinksetCost : public LLFloater, public LLAccountingCostObserver
{
public:
    FSFloaterLinksetCost(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;
    void draw() override;

    // LLAccountingCostObserver, the server weight of the linkset. Called from a coroutine.
    void onWeightsUpdate(const SelectionCost& selection_cost) override;
    void setErrorStatus(S32 status, const std::string& reason) override;

protected:
    void generateTransactionID() override;

private:
    enum EServerState
    {
        SERVER_NONE,        // not asked, or the region can't tell
        SERVER_PENDING,
        SERVER_KNOWN
    };

    LLViewerObject* selectedRoot() const;
    void refreshList();
    void requestServerCost(LLViewerObject* root);
    std::string primName(LLViewerObject* prim, S32 link_num);
    void onClickSelectPrim();
    void onClickRefresh();

    LLObjectSelectionHandle mSelection;     // keeps the selection the window was opened on
    LLUUID                  mRootID;
    std::map<LLUUID, std::string> mNames;   // prim names, kept once their selection has gone
    LLTimer                 mRefreshTimer;

    EServerState            mServerState = SERVER_NONE;
    F32                     mServerCost = 0.f;
    bool                    mServerDirty = false;   // an answer arrived (from the coroutine)

    LLScrollListCtrl*       mList = nullptr;
    LLTextBox*              mTotals = nullptr;
    LLTextBox*              mStatus = nullptr;
    LLUICtrl*               mSelectPrimBtn = nullptr;
};

// What's heavy here. Lists the linksets and avatars (with their attachments) near the agent with what
// they cost this viewer to draw: texture memory (the unique textures they use, at full resolution and
// as loaded now), triangles at the level of detail drawn now, unique textures and, for avatars, their
// complexity. The scan runs when asked and is spread over frames with a time budget, so a region of
// tens of thousands of objects doesn't hitch. Selecting an entry lists its five biggest textures and puts
// a beacon on it; the names of the objects are asked for after the scan, and a right click edits them.
class FSFloaterHeavyNearby : public LLFloater
{
public:
    FSFloaterHeavyNearby(const LLSD& key);
    ~FSFloaterHeavyNearby();

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;
    void draw() override;

    // ObjectProperties from the simulator: the names of the objects asked for
    static void processObjectProperties(LLMessageSystem* msg);
    // the right click menu's actions on the selected entry
    void onMenuAction(const std::string& action);
    bool onMenuEnable(const std::string& action);

    struct TextureInfo
    {
        LLUUID  mID;
        S32     mWidth = 0;
        S32     mHeight = 0;
        S64     mFullBytes = 0;     // estimated, at full resolution with mipmaps
        S64     mLoadedBytes = 0;   // in GL memory now
    };

private:
    struct Entry
    {
        LLUUID      mID;
        bool        mIsAvatar = false;
        std::string mName;
        F32         mDistance = 0.f;
        S32         mPrims = 0;
        U32         mTriangles = 0;
        S32         mTextures = 0;
        S64         mFullBytes = 0;
        S64         mLoadedBytes = 0;
        S32         mComplexity = -1;   // avatars only
        std::vector<TextureInfo> mTopTextures;
    };

    void startScan();
    bool scanStep(U32 generation);      // true when this scan is over
    void scanObject(LLViewerObject* object);
    void measureAvatar(LLVOAvatar* avatar, Entry& entry);
    void finishScan();
    void refreshList();
    void refreshTextures();
    std::string objectName(LLViewerObject* object) const;
    const Entry* findEntry(const LLUUID& id) const;
    void onDoubleClick();
    void requestNames();
    void receiveName(const LLUUID& id, const std::string& name);

    std::vector<Entry>  mEntries;       // the last finished scan
    std::vector<Entry>  mScanEntries;   // the scan running
    U32                 mScanGeneration = 0;
    bool                mScanning = false;
    S32                 mScanIndex = 0;
    LLVector3d          mScanCenter;
    F32                 mScanRange = 0.f;
    bool                mScanRegionOnly = true;
    std::map<LLUUID, std::string> mNames;   // object names the simulator sent, kept across scans
    std::set<LLUUID>    mNamesAsked;        // asked for and not answered yet

    LLScrollListCtrl*   mList = nullptr;
    LLScrollListCtrl*   mTextureList = nullptr;
    LLSpinCtrl*         mRange = nullptr;
    LLUICtrl*           mRegionOnly = nullptr;
    LLCheckBoxCtrl*     mBeacon = nullptr;
    LLTextBox*          mStatus = nullptr;
    std::unique_ptr<LLListContextMenu> mMenu;
};
// </FS:Trish>

#endif // FS_FLOATERBUILDCOST_H
