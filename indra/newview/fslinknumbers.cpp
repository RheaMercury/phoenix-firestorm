/**
 * @file fslinknumbers.cpp
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

#include "llviewerprecompiledheaders.h"

#include "fslinknumbers.h"

#include "fslslbridge.h"
#include "llagent.h"
#include "llcorehttputil.h"
#include "lleventtimer.h"
#include "llfloatertools.h"
#include "llframetimer.h"
#include "llselectmgr.h"
#include "llspinctrl.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerregion.h"
#include "rlvactions.h"
#include "rlvhandler.h"

#if OPENSIM
#include "llviewernetwork.h"
#endif

// The bridge's HTTP-in body is limited to 2 KB: a root key and 48 prim keys fit with room to spare
static const size_t PRIMS_PER_REQUEST = 48;
// A request that never answers (bridge gone, region change) frees the way after this long
static const F32 REPLY_TIMEOUT_SECONDS = 20.f;
// After an incomplete answer, wait before asking again, and give up after a few tries
static const F32 RETRY_DELAY_SECONDS = 10.f;
static const U32 MAX_ATTEMPTS = 3;
// Linksets remembered at once; edited linksets are few
static const size_t MAX_LINKSETS = 64;

// While a number shows "(checking)", the floater is refreshed again once the answer is due,
// so a lost answer or a retry doesn't leave it waiting for some unrelated refresh
class FSLinkNumbersRefreshTimer : public LLEventTimer
{
public:
    explicit FSLinkNumbersRefreshTimer(F32 seconds) : LLEventTimer(seconds)
    {
        sPending = this;
        sDue = LLFrameTimer::getElapsedSeconds() + seconds;
    }
    ~FSLinkNumbersRefreshTimer()
    {
        if (sPending == this)
        {
            sPending = nullptr;
        }
    }

    bool tick() override
    {
        if (sPending == this)
        {
            sPending = nullptr; // a refresh asked for from here on gets a timer of its own
        }
        if (gFloaterTools)
        {
            gFloaterTools->dirty();
        }
        return true; // one shot
    }

    // One refresh at the soonest deadline asked for: a sooner one (a retry, another linkset's shorter wait)
    // replaces a pending later one. The replaced timer stays to fire once more, which only refreshes again.
    static void schedule(F32 seconds)
    {
        seconds = llmax(seconds, 0.5f);
        if (!sPending || LLFrameTimer::getElapsedSeconds() + seconds < sDue - 0.25)
        {
            new FSLinkNumbersRefreshTimer(seconds);
        }
    }

private:
    static FSLinkNumbersRefreshTimer* sPending;
    static F64 sDue;
};
FSLinkNumbersRefreshTimer* FSLinkNumbersRefreshTimer::sPending = nullptr;
F64 FSLinkNumbersRefreshTimer::sDue = 0.0;

FSLinkNumbers::FSLinkNumbers()
{
}

// static
void FSLinkNumbers::collectPrims(LLViewerObject* root, std::vector<LLViewerObject*>& prims)
{
    prims.clear();
    if (!root)
    {
        return;
    }
    prims.push_back(root);
    for (const LLPointer<LLViewerObject>& child : root->getChildren())
    {
        // Seated avatars are children too, but they come after every prim in link order
        if (child && !child->isAvatar())
        {
            prims.push_back(child.get());
        }
    }
}

// static
bool FSLinkNumbers::canAsk(LLViewerObject* root)
{
    static LLCachedControl<bool> verified_link_numbers(gSavedSettings, "FSBuildVerifiedLinkNumbers", true);
    if (!verified_link_numbers || !root || root->isDead())
    {
        return false;
    }
#if OPENSIM
    if (LLGridManager::getInstance()->isInOpenSim())
    {
        return false;
    }
#endif
    // The bridge is worn by our avatar and only looks up prims in its own region
    if (!root->getRegion() || root->getRegion() != gAgent.getRegion())
    {
        return false;
    }
    return FSLSLBridge::instanceExists() && FSLSLBridge::instance().canUseBridge();
}

const FSLinkNumbers::Linkset* FSLinkNumbers::verified(LLViewerObject* root, const std::vector<LLViewerObject*>& prims) const
{
    auto it = mLinksets.find(root->getID());
    if (it == mLinksets.end() || !it->second.mComplete || it->second.mAsked.size() != prims.size())
    {
        return nullptr;
    }
    for (LLViewerObject* prim : prims)
    {
        if (!it->second.mLinks.count(prim->getID()))
        {
            return nullptr;
        }
    }
    return &it->second;
}

// static
bool FSLinkNumbers::getLinkNumber(LLViewerObject* object, S32& link_num, S32& link_count, EState* state)
{
    link_num = 0;
    link_count = 0;
    if (state)
    {
        *state = GUESSED;
    }
    LLViewerObject* root = object ? object->getRootEdit() : nullptr;
    if (!root || object->isAvatar() || root->isAvatar())
    {
        return false;
    }

    std::vector<LLViewerObject*> prims;
    collectPrims(root, prims);
    link_count = (S32)prims.size();
    if (link_count == 1)
    {
        link_num = 0; // a prim on its own is always link zero
        if (state)
        {
            *state = VERIFIED;
        }
        return true;
    }

    S32 guess = 0;
    for (size_t i = 0; i < prims.size(); ++i)
    {
        if (prims[i] == object)
        {
            guess = (S32)i + 1;
            break;
        }
    }
    if (guess == 0)
    {
        return false;
    }
    link_num = guess;

    FSLinkNumbers& self = instance();
    if (const Linkset* linkset = self.verified(root, prims))
    {
        link_num = linkset->mLinks.at(object->getID());
        if (state)
        {
            *state = VERIFIED;
        }
        return true;
    }

    // Only for what the user is editing
    if (object->isSelected() && canAsk(root))
    {
        const EState asked = self.request(root, prims);
        if (state)
        {
            *state = asked;
        }
    }
    return true;
}

// static
LLViewerObject* FSLinkNumbers::findLink(LLViewerObject* any_link, S32 link_num, EState* state)
{
    if (state)
    {
        *state = GUESSED;
    }
    LLViewerObject* root = any_link ? any_link->getRootEdit() : nullptr;
    if (!root || root->isAvatar())
    {
        return nullptr;
    }
    std::vector<LLViewerObject*> prims;
    collectPrims(root, prims);
    if (prims.size() == 1)
    {
        return (link_num == 0 || link_num == 1) ? root : nullptr;
    }

    if (const Linkset* linkset = instance().verified(root, prims))
    {
        for (LLViewerObject* prim : prims)
        {
            if (linkset->mLinks.at(prim->getID()) == link_num)
            {
                if (state)
                {
                    *state = VERIFIED;
                }
                return prim;
            }
        }
        return nullptr;
    }

    if (link_num >= 1 && link_num <= (S32)prims.size())
    {
        return prims[link_num - 1];
    }
    return nullptr;
}

FSLinkNumbers::EState FSLinkNumbers::request(LLViewerObject* root, const std::vector<LLViewerObject*>& prims)
{
    const LLUUID root_id = root->getID();

    auto found = mLinksets.find(root_id);
    if (found != mLinksets.end() && found->second.mComplete)
    {
        // Answered before, but the prims are not the ones asked about any more
        forget(root_id);
        found = mLinksets.end();
    }
    if (found == mLinksets.end() && mLinksets.size() >= MAX_LINKSETS)
    {
        // Mostly linksets taken, deleted or left behind: start over
        mLinksets.clear();
        mInFlightRoot.setNull();
    }

    Linkset& linkset = mLinksets[root_id];
    if (linkset.mPendingBatches > 0)
    {
        if (linkset.mTimer.getElapsedTimeF32() < REPLY_TIMEOUT_SECONDS)
        {
            FSLinkNumbersRefreshTimer::schedule(REPLY_TIMEOUT_SECONDS - linkset.mTimer.getElapsedTimeF32() + 0.5f);
            return CHECKING;
        }
        linkset.mPendingBatches = 0; // the answer got lost
        if (mInFlightRoot == root_id)
        {
            mInFlightRoot.setNull();
        }
    }
    if (linkset.mAttempts >= MAX_ATTEMPTS)
    {
        return GUESSED;
    }
    if (linkset.mAttempts > 0 && linkset.mTimer.getElapsedTimeF32() < RETRY_DELAY_SECONDS)
    {
        FSLinkNumbersRefreshTimer::schedule(RETRY_DELAY_SECONDS - linkset.mTimer.getElapsedTimeF32() + 0.5f);
        return CHECKING;
    }

    // One linkset at a time, so clicking through many objects doesn't flood the bridge
    if (mInFlightRoot.notNull() && mInFlightRoot != root_id)
    {
        auto other = mLinksets.find(mInFlightRoot);
        if (other != mLinksets.end() && other->second.mPendingBatches > 0
            && other->second.mTimer.getElapsedTimeF32() < REPLY_TIMEOUT_SECONDS)
        {
            // Its answer refreshes the floater; this is in case it never comes
            FSLinkNumbersRefreshTimer::schedule(REPLY_TIMEOUT_SECONDS - other->second.mTimer.getElapsedTimeF32() + 0.5f);
            return CHECKING;
        }
        mInFlightRoot.setNull();
    }

    linkset.mGeneration = ++mNextGeneration;
    linkset.mLinks.clear();
    linkset.mAsked.clear();
    for (LLViewerObject* prim : prims)
    {
        linkset.mAsked.push_back(prim->getID());
    }
    ++linkset.mAttempts;
    linkset.mTimer.reset();
    linkset.mPendingBatches = 0;

    const U32 generation = linkset.mGeneration;
    const std::string prefix = "getLinkNumbers|" + root_id.asString() + "|";
    for (size_t start = 0; start < linkset.mAsked.size(); start += PRIMS_PER_REQUEST)
    {
        const size_t end = llmin(start + PRIMS_PER_REQUEST, linkset.mAsked.size());
        std::vector<LLUUID> batch(linkset.mAsked.begin() + start, linkset.mAsked.begin() + end);
        std::string message = prefix;
        for (size_t i = 0; i < batch.size(); ++i)
        {
            if (i)
            {
                message += ",";
            }
            message += batch[i].asString();
        }

        // Replies come back on the main thread; the singleton may be gone by then
        const bool sent = FSLSLBridge::instance().viewerToLSL(message, [root_id, generation, batch](const LLSD& result)
        {
            if (!FSLinkNumbers::instanceExists())
            {
                return;
            }
            std::string answer;
            if (result.isMap() && result.has(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_CONTENT))
            {
                answer = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_CONTENT].asString();
            }
            else if (result.isString())
            {
                answer = result.asString();
            }
            FSLinkNumbers::instance().onReply(root_id, generation, batch, answer);
        });
        if (!sent)
        {
            // The bridge went away; batches already sent are answered or time out
            break;
        }
        ++linkset.mPendingBatches;
    }

    if (linkset.mPendingBatches == 0)
    {
        return GUESSED;
    }
    mInFlightRoot = root_id;
    FSLinkNumbersRefreshTimer::schedule(REPLY_TIMEOUT_SECONDS + 0.5f);
    return CHECKING;
}

void FSLinkNumbers::onReply(const LLUUID& root_id, U32 generation, const std::vector<LLUUID>& batch, const std::string& answer)
{
    auto it = mLinksets.find(root_id);
    if (it == mLinksets.end() || it->second.mGeneration != generation)
    {
        return; // the linkset changed since, or was forgotten
    }
    Linkset& linkset = it->second;
    if (linkset.mPendingBatches > 0)
    {
        --linkset.mPendingBatches;
    }

    // "3, 1, 2, -1": one number per prim asked about, in the same order
    std::vector<S32> numbers;
    bool parsed = true;
    size_t pos = 0;
    while (pos <= answer.size())
    {
        size_t comma = answer.find(',', pos);
        if (comma == std::string::npos)
        {
            comma = answer.size();
        }
        std::string token = answer.substr(pos, comma - pos);
        LLStringUtil::trim(token);
        S32 value = 0;
        if (token.empty() || !LLStringUtil::convertToS32(token, value))
        {
            parsed = false;
            break;
        }
        numbers.push_back(value);
        pos = comma + 1;
    }

    if (!parsed || numbers.size() != batch.size())
    {
        // A bridge without this command answers "ok": don't ask it again about this linkset
        LL_DEBUGS("FSLinkNumbers") << "Unexpected link number answer: " << answer << LL_ENDL;
        linkset.mAttempts = MAX_ATTEMPTS;
    }
    else
    {
        for (size_t i = 0; i < batch.size(); ++i)
        {
            // -1: the region doesn't see this prim in that linkset (it changed, or left the region)
            if (numbers[i] >= 1)
            {
                linkset.mLinks[batch[i]] = numbers[i];
            }
        }
    }

    if (linkset.mPendingBatches > 0)
    {
        return;
    }
    if (mInFlightRoot == root_id)
    {
        mInFlightRoot.setNull();
    }

    // Complete when every prim answered and the numbers are exactly 1..N
    const size_t count = linkset.mAsked.size();
    bool complete = linkset.mLinks.size() == count;
    if (complete)
    {
        std::vector<bool> seen(count + 1, false);
        for (const LLUUID& id : linkset.mAsked)
        {
            auto link = linkset.mLinks.find(id);
            if (link == linkset.mLinks.end() || link->second < 1 || link->second > (S32)count || seen[link->second])
            {
                complete = false;
                break;
            }
            seen[link->second] = true;
        }
    }
    linkset.mComplete = complete;
    if (!complete)
    {
        linkset.mLinks.clear();
    }

    if (gFloaterTools)
    {
        gFloaterTools->dirty();
    }
}

void FSLinkNumbers::forget(const LLUUID& root_id)
{
    if (mLinksets.erase(root_id) && mInFlightRoot == root_id)
    {
        mInFlightRoot.setNull();
    }
}

// static
void FSLinkNumbers::onLinksetChanged(const LLUUID& parent_id, const LLUUID& child_id)
{
    if (!instanceExists())
    {
        return; // nothing was asked yet: costs nothing while regions load
    }
    FSLinkNumbers& self = instance();
    if (self.mLinksets.empty())
    {
        return;
    }
    self.forget(parent_id);
    self.forget(child_id); // it may have been the root of a linkset of its own
}

// static
void FSLinkNumbers::refreshSpinner(LLSpinCtrl* spinner)
{
    if (!spinner || !spinner->getVisible())
    {
        return;
    }

    // One linkset: a single prim of it with Edit linked, or the whole linkset
    LLObjectSelectionHandle selection = LLSelectMgr::getInstance()->getSelection();
    LLViewerObject* object = selection->getFirstObject();
    LLViewerObject* root = object ? object->getRootEdit() : nullptr;
    bool one_linkset = root && !root->isAvatar();
    for (LLObjectSelection::iterator it = selection->begin(); one_linkset && it != selection->end(); ++it)
    {
        LLViewerObject* selected = (*it)->getObject();
        one_linkset = selected && selected->getRootEdit() == root;
    }

    S32 link_num = 0;
    S32 link_count = 0;
    EState state = GUESSED;
    const bool have_links = one_linkset && getLinkNumber(object, link_num, link_count, &state) && link_count > 1;
    // Greyed while the bridge is asked: a number typed now would pick a prim by the guessed order
    spinner->setEnabled(have_links && state != CHECKING);
    if (!have_links)
    {
        spinner->setMinValue(0.f);
        spinner->setMaxValue(0.f);
        spinner->setValue(0.f);
        spinner->setTentative(false);
        return;
    }

    if (selection->getObjectCount() != 1)
    {
        // Several prims or the whole linkset: show the root
        link_num = 1;
        state = VERIFIED;
    }
    spinner->setMinValue(1.f);
    spinner->setMaxValue((F32)link_count);
    spinner->setValue((F32)link_num);
    // While the bridge is being asked, the number shown is still the guess
    spinner->setTentative(state == CHECKING);
}

// static
void FSLinkNumbers::onSpinnerCommit(LLSpinCtrl* spinner)
{
    if (!spinner)
    {
        return;
    }
    LLObjectSelectionHandle selection = LLSelectMgr::getInstance()->getSelection();
    LLViewerObject* object = selection->getFirstObject();
    if (!object)
    {
        return;
    }
    LLViewerObject* target = findLink(object, ll_round(spinner->get()));
    if (!target || target->isDead())
    {
        return;
    }
    if (rlv_handler_t::isEnabled() && !RlvActions::canEdit(target))
    {
        return;
    }
    if (target->isSelected() && selection->getObjectCount() == 1)
    {
        return; // already the one selected
    }

    // As Edit linked does: select only that prim of the linkset
    if (!gSavedSettings.getBOOL("EditLinkedParts"))
    {
        gSavedSettings.setBOOL("EditLinkedParts", true);
    }
    LLSelectMgr::getInstance()->deselectAll();
    LLSelectMgr::getInstance()->selectObjectOnly(target);
}
