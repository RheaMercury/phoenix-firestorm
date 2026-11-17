/**
 * @file fsfloaterscriptprofiler.cpp
 * @brief Script time and memory of objects and avatars, refreshed through the LSL bridge
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

#include "fsfloaterscriptprofiler.h"

#include "fslslbridge.h"
#include "llagent.h"
#include "llagentcamera.h"
#include "llfloaterreg.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "llselectmgr.h"
#include "lltextbox.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvoavatar.h"
#include "llworld.h"

namespace
{
    constexpr F32 REQUEST_TIMEOUT = 5.f;    // seconds without an answer before the next request
    constexpr size_t HISTORY_LENGTH = 30;   // answers kept per entry, for the peak
    constexpr S32 EXTENDED_FIELDS = 27;     // fields of the extended answer, the only one that names the object
    constexpr S32 FIELD_INSPECTED_ID = 26;

    FSFloaterScriptProfiler* get_instance(bool open)
    {
        return open ? LLFloaterReg::showTypedInstance<FSFloaterScriptProfiler>("fs_script_profiler")
                    : LLFloaterReg::findTypedInstance<FSFloaterScriptProfiler>("fs_script_profiler");
    }

    bool parse_number(const std::string& text, F64& value)
    {
        if (text.empty())
        {
            return false;
        }
        char* end = nullptr;
        value = strtod(text.c_str(), &end);
        return end && *end == '\0';
    }

    uuid_vec_t selected_ids(const LLScrollListCtrl* list)
    {
        uuid_vec_t ids;
        for (const LLScrollListItem* item : list->getAllSelected())
        {
            ids.push_back(item->getUUID());
        }
        return ids;
    }

    // Sort numeric columns by value, the others as text
    S32 compare_cells(S32 column, const LLScrollListItem* a, const LLScrollListItem* b)
    {
        const LLScrollListCell* cell_a = a->getColumn(column);
        const LLScrollListCell* cell_b = b->getColumn(column);
        if (!cell_a || !cell_b)
        {
            return 0;
        }
        const std::string text_a = cell_a->getValue().asString();
        const std::string text_b = cell_b->getValue().asString();
        F64 value_a = 0.0;
        F64 value_b = 0.0;
        const bool num_a = parse_number(text_a, value_a);
        const bool num_b = parse_number(text_b, value_b);
        if (num_a && num_b)
        {
            return value_a < value_b ? -1 : (value_a > value_b ? 1 : 0);
        }
        if (num_a != num_b)
        {
            return num_a ? 1 : -1; // "-" (no answer) below any figure
        }
        return LLStringUtil::compareDict(text_a, text_b);
    }
}

FSFloaterScriptProfiler::FSFloaterScriptProfiler(const LLSD& key)
:   LLFloater(key)
{
}

bool FSFloaterScriptProfiler::postBuild()
{
    mList = getChild<LLScrollListCtrl>("targets");
    mStatus = getChild<LLTextBox>("status");
    mList->setSortCallback([](S32 column, const LLScrollListItem* a, const LLScrollListItem* b) { return compare_cells(column, a, b); });
    mList->setDoubleClickCallback([this]() { onDoubleClick(); });
    mList->sortByColumn("time", false);

    getChild<LLUICtrl>("add_selection")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickAddSelection(); });
    getChild<LLUICtrl>("add_avatars")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickAddAvatars(); });
    getChild<LLUICtrl>("remove")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickRemove(); });
    getChild<LLUICtrl>("clear")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickClear(); });
    getChild<LLUICtrl>("refresh")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickRefresh(); });
    return true;
}

// static
void FSFloaterScriptProfiler::addTargets(const uuid_vec_t& ids)
{
    if (FSFloaterScriptProfiler* floater = get_instance(true))
    {
        floater->addIds(ids);
    }
}

// static
void FSFloaterScriptProfiler::addSelection()
{
    if (FSFloaterScriptProfiler* floater = get_instance(true))
    {
        floater->onClickAddSelection();
    }
}

// static
bool FSFloaterScriptProfiler::handleBridgeReply(const LLSD& fields, S32 field_count)
{
    FSFloaterScriptProfiler* floater = get_instance(false);
    return floater && floater->onReply(fields, field_count);
}

void FSFloaterScriptProfiler::addIds(const uuid_vec_t& ids)
{
    for (const LLUUID& id : ids)
    {
        if (id.isNull() || mEntries.count(id))
        {
            continue;
        }
        Entry& entry = mEntries[id];
        LLViewerObject* object = gObjectList.findObject(id);
        entry.mIsAvatar = object && object->isAvatar();
        mQueue.push_back(id);
    }
    mListDirty = true;
}

void FSFloaterScriptProfiler::queueAll()
{
    for (const auto& [id, entry] : mEntries)
    {
        if (std::find(mQueue.begin(), mQueue.end(), id) == mQueue.end())
        {
            mQueue.push_back(id);
        }
    }
    mCycleTimer.reset();
}

void FSFloaterScriptProfiler::sendNext()
{
    if (mInFlight.notNull() && mInFlightTimer.getElapsedTimeF32() < REQUEST_TIMEOUT)
    {
        return;
    }
    mInFlight.setNull();
    while (!mQueue.empty())
    {
        const LLUUID id = mQueue.front();
        mQueue.pop_front();
        if (!mEntries.count(id))
        {
            continue;
        }
        // always the extended answer: it is the only one that names the object it is about
        if (FSLSLBridge::instance().viewerToLSL("getScriptInfo|" + id.asString() + "|1"))
        {
            mInFlight = id;
            mInFlightTimer.reset();
        }
        return;
    }
}

bool FSFloaterScriptProfiler::onReply(const LLSD& fields, S32 field_count)
{
    if (mInFlight.isNull())
    {
        return false;
    }
    if (field_count == EXTENDED_FIELDS)
    {
        const LLUUID id = fields[FIELD_INSPECTED_ID].asUUID();
        if (id != mInFlight)
        {
            return false; // a Script Info asked for elsewhere
        }
        Entry& entry = mEntries[id];
        entry.mName = fields[0].asString();
        entry.mRunning = fields[1].asInteger();
        entry.mTotal = fields[2].asInteger();
        entry.mMemoryKB = (F32)fields[3].asReal();
        entry.mTimeMs = (F32)fields[4].asReal();
        entry.mKnown = true;
        entry.mMissing = false;
        entry.mHistory.push_back(entry.mTimeMs);
        while (entry.mHistory.size() > HISTORY_LENGTH)
        {
            entry.mHistory.pop_front();
        }
        entry.mPeakMs = *std::max_element(entry.mHistory.begin(), entry.mHistory.end());
    }
    else if (field_count != 6)
    {
        // the bridge found nothing to inspect: gone, or not in the region
        mEntries[mInFlight].mMissing = true;
    }
    else
    {
        return false; // a short answer, never asked for here
    }
    mInFlight.setNull();
    mListDirty = true;
    sendNext();
    return true;
}

void FSFloaterScriptProfiler::draw()
{
    static LLCachedControl<bool> auto_refresh(gSavedSettings, "FSScriptProfilerAutoRefresh", true);
    static LLCachedControl<F32> interval(gSavedSettings, "FSScriptProfilerInterval", 5.f);

    const bool bridge_ok = FSLSLBridge::instance().canUseBridge();
    if (bridge_ok && auto_refresh && mQueue.empty() && mInFlight.isNull() && !mEntries.empty() &&
        mCycleTimer.getElapsedTimeF32() >= llmax((F32)interval, 1.f))
    {
        queueAll();
    }
    if (bridge_ok)
    {
        sendNext();
    }
    if (mListDirty)
    {
        refreshList();
    }

    if (!bridge_ok)
    {
        mStatus->setText(getString("no_bridge"));
    }
    else
    {
        LLStringUtil::format_map_t args;
        args["COUNT"] = llformat("%d", (S32)mEntries.size());
        args["PENDING"] = llformat("%d", (S32)mQueue.size() + (mInFlight.notNull() ? 1 : 0));
        mStatus->setText(getString("status", args));
    }
    LLFloater::draw();
}

void FSFloaterScriptProfiler::refreshList()
{
    mListDirty = false;
    const uuid_vec_t selected = selected_ids(mList);
    const S32 scroll = mList->getScrollPos();
    mList->deleteAllItems();

    const LLVector3d agent_pos = gAgent.getPositionGlobal();
    for (const auto& [id, entry] : mEntries)
    {
        std::string distance = "-";
        if (LLViewerObject* object = gObjectList.findObject(id))
        {
            distance = llformat("%.0f", (F32)(object->getPositionGlobal() - agent_pos).magVec());
        }
        std::string name = entry.mName;
        if (name.empty())
        {
            name = entry.mIsAvatar ? getString("avatar_pending") : getString("object_pending");
        }
        if (entry.mMissing)
        {
            name += " " + getString("not_found");
        }

        LLSD row;
        row["id"] = id;
        LLSD& columns = row["columns"];
        columns[0]["column"] = "name";
        columns[0]["value"] = name;
        columns[1]["column"] = "kind";
        columns[1]["value"] = entry.mIsAvatar ? getString("kind_avatar") : getString("kind_object");
        columns[2]["column"] = "scripts";
        columns[2]["value"] = entry.mKnown ? llformat("%d/%d", entry.mRunning, entry.mTotal) : std::string("-");
        columns[3]["column"] = "memory";
        columns[3]["value"] = entry.mKnown ? llformat("%.0f", entry.mMemoryKB) : std::string("-");
        columns[4]["column"] = "time";
        columns[4]["value"] = entry.mKnown ? llformat("%.3f", entry.mTimeMs) : std::string("-");
        columns[5]["column"] = "peak";
        columns[5]["value"] = entry.mKnown ? llformat("%.3f", entry.mPeakMs) : std::string("-");
        columns[6]["column"] = "distance";
        columns[6]["value"] = distance;
        mList->addElement(row);
    }
    mList->updateSort();
    mList->selectMultiple(selected);
    mList->setScrollPos(scroll);
}

void FSFloaterScriptProfiler::onClickAddSelection()
{
    uuid_vec_t ids;
    LLObjectSelectionHandle selection = LLSelectMgr::getInstance()->getSelection();
    for (LLObjectSelection::root_iterator it = selection->root_begin(); it != selection->root_end(); ++it)
    {
        LLViewerObject* object = (*it)->getObject();
        if (object)
        {
            ids.push_back(object->getID());
        }
    }
    if (ids.empty() && selection->getPrimaryObject())
    {
        ids.push_back(selection->getPrimaryObject()->getID());
    }
    addIds(ids);
}

void FSFloaterScriptProfiler::onClickAddAvatars()
{
    // the bridge only sees its own region
    LLViewerRegion* region = gAgent.getRegion();
    if (!region)
    {
        return;
    }
    uuid_vec_t avatars;
    std::vector<LLVector3d> positions;
    LLWorld::getInstance()->getAvatars(&avatars, &positions, gAgent.getPositionGlobal(), 4096.f);
    uuid_vec_t ids;
    for (size_t i = 0; i < avatars.size(); ++i)
    {
        if (LLWorld::getInstance()->getRegionFromPosGlobal(positions[i]) == region)
        {
            ids.push_back(avatars[i]);
        }
    }
    addIds(ids);
    for (const LLUUID& id : ids)
    {
        mEntries[id].mIsAvatar = true;
    }
}

void FSFloaterScriptProfiler::onClickRemove()
{
    for (const LLUUID& id : selected_ids(mList))
    {
        mEntries.erase(id);
    }
    mListDirty = true;
}

void FSFloaterScriptProfiler::onClickClear()
{
    mEntries.clear();
    mQueue.clear();
    mListDirty = true;
}

void FSFloaterScriptProfiler::onClickRefresh()
{
    queueAll();
}

void FSFloaterScriptProfiler::onDoubleClick()
{
    // look at it
    const LLUUID id = mList->getCurrentID();
    LLViewerObject* object = gObjectList.findObject(id);
    if (!object)
    {
        return;
    }
    gAgentCamera.setFocusOnAvatar(false, ANIMATE);
    gAgentCamera.setFocusGlobal(object->getPositionGlobal(), id);
}
