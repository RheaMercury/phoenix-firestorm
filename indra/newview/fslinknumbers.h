/**
 * @file fslinknumbers.h
 * @brief Link numbers of the prims of a linkset, confirmed by the region through the LSL bridge (FIRE-4292)
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, Trish_sl
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
 * $/LicenseInfo$
 */


#ifndef FS_LINKNUMBERS_H
#define FS_LINKNUMBERS_H

#include "llsingleton.h"
#include "lltimer.h"
#include "lluuid.h"

#include <unordered_map>
#include <vector>

class LLSpinCtrl;
class LLViewerObject;

// The object update protocol never says which link number a prim has. The
// viewer keeps a linkset's children in the order their updates arrived and
// numbered them that way, which is often wrong (FIRE-4292). LSL can tell:
// llGetObjectDetails(prim, [OBJECT_LINK_NUMBER]) works on any prim in the
// region. So for the linkset being edited, the build floater asks the LSL
// bridge for the true numbers, a batch of prims per HTTP request, and keeps
// them per linkset until the linkset changes. Until the answer is there (or
// with no bridge, on OpenSim, or with FSBuildVerifiedLinkNumbers off) the old
// arrival-order guess is used.
class FSLinkNumbers : public LLSingleton<FSLinkNumbers>
{
    LLSINGLETON(FSLinkNumbers);
    ~FSLinkNumbers() = default;

public:
    enum EState
    {
        GUESSED,    // arrival order; nobody is asking the region
        CHECKING,   // arrival order, while the bridge is being asked
        VERIFIED    // the region's own number
    };

    // The link number of a prim as scripts see it: 0 for a prim on its own, 1 for the root.
    // link_count is the number of prims (seated avatars are not counted). Asks the bridge
    // when the prim is selected and its linkset's numbers aren't known yet.
    // Returns false for an avatar or an object without a root.
    static bool getLinkNumber(LLViewerObject* object, S32& link_num, S32& link_count, EState* state = nullptr);

    // The prim with that link number in any_link's linkset, or nullptr
    static LLViewerObject* findLink(LLViewerObject* any_link, S32 link_num, EState* state = nullptr);

    // From LLViewerObject::addChild()/removeChild(): numbers known for these linksets are stale
    static void onLinksetChanged(const LLUUID& parent_id, const LLUUID& child_id);

    // The build floater's link number spinner: range, value and enabled state from the selection
    static void refreshSpinner(LLSpinCtrl* spinner);
    static void onSpinnerCommit(LLSpinCtrl* spinner);

private:
    struct Linkset
    {
        std::unordered_map<LLUUID, S32> mLinks; // prim -> number from the region
        std::vector<LLUUID> mAsked;             // the prims asked about, in the viewer's order
        U32 mGeneration = 0;                    // answers to older requests are dropped
        S32 mPendingBatches = 0;
        U32 mAttempts = 0;
        bool mComplete = false;                 // every prim answered, numbers 1..N
        LLTimer mTimer;                         // since the last request
    };

    static void collectPrims(LLViewerObject* root, std::vector<LLViewerObject*>& prims);
    static bool canAsk(LLViewerObject* root);

    // The region's numbers for these prims, or nullptr when they aren't all known
    const Linkset* verified(LLViewerObject* root, const std::vector<LLViewerObject*>& prims) const;
    EState request(LLViewerObject* root, const std::vector<LLViewerObject*>& prims);
    void onReply(const LLUUID& root_id, U32 generation, const std::vector<LLUUID>& batch, const std::string& answer);
    void forget(const LLUUID& root_id);

    std::unordered_map<LLUUID, Linkset> mLinksets;  // by root prim
    LLUUID mInFlightRoot;                           // one linkset is asked about at a time
    U32 mNextGeneration = 0;
};

#endif // FS_LINKNUMBERS_H
