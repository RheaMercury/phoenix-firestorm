/**
 * @file fsobjectnames.cpp
 * @brief Build floater: object names and descriptions, sent only to the objects being edited, and names of links that are not selected
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

#include "fsobjectnames.h"

#include "llagent.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "message.h"
#include "rlvactions.h"
#include "rlvhandler.h"
#include "lltimer.h"

#include <map>
#include <unordered_set>

namespace
{
    // Main thread only, like the message system that feeds it
    std::unordered_map<LLUUID, F64> sNameRequests;      // when each name was asked for
    std::unordered_map<LLUUID, std::string> sNames;     // names that arrived
    const F64 NAME_RETRY_SECONDS = 30.0;
    const F64 NAME_REFRESH_SECONDS = 60.0;
    const size_t MAX_KEPT = 4096;
}

// Same as LLSelectMgr's limit for one packet
static const S32 MAX_OBJECTS_PER_MESSAGE = 254;

void FSObjectNames::collectTargets(LLObjectSelectionHandle selection, bool roots, uuid_vec_t& targets)
{
    targets.clear();
    if (selection.isNull())
    {
        return;
    }

    if (roots)
    {
        for (LLObjectSelection::root_iterator it = selection->root_begin(); it != selection->root_end(); ++it)
        {
            LLViewerObject* object = (*it)->getObject();
            if (object)
            {
                targets.push_back(object->getID());
            }
        }
    }
    else
    {
        for (LLObjectSelection::iterator it = selection->begin(); it != selection->end(); ++it)
        {
            LLViewerObject* object = (*it)->getObject();
            if (object)
            {
                targets.push_back(object->getID());
            }
        }
    }
}

S32 FSObjectNames::send(const uuid_vec_t& targets, const std::string& text, bool description)
{
    if (targets.empty() || (!description && text.empty()))
    {
        return 0;
    }

    // What the viewer knows of each target's current name and description
    std::unordered_map<LLUUID, LLSelectNode*> nodes;
    LLObjectSelectionHandle selection = LLSelectMgr::getInstance()->getSelection();
    for (LLObjectSelection::iterator it = selection->begin(); it != selection->end(); ++it)
    {
        if (LLViewerObject* object = (*it)->getObject())
        {
            nodes[object->getID()] = *it;
        }
    }

    // Grouped by region, one message per region (split when full)
    std::map<LLViewerRegion*, std::vector<U32>> per_region;
    std::unordered_set<LLUUID> seen;
    for (const LLUUID& id : targets)
    {
        if (!seen.insert(id).second)
        {
            continue;
        }
        LLViewerObject* object = gObjectList.findObject(id);
        if (!object || object->isDead() || object->isAvatar() || !object->getRegion() || !object->permModify())
        {
            continue;
        }
        if (rlv_handler_t::isEnabled() && !RlvActions::canEdit(object))
        {
            continue;
        }
        auto node_it = nodes.find(id);
        if (node_it != nodes.end() && node_it->second->mValid)
        {
            const std::string& current = description ? node_it->second->mDescription : node_it->second->mName;
            if (current == text)
            {
                continue; // unchanged, nothing to write
            }
        }
        per_region[object->getRegion()].push_back(object->getLocalID());
        if (!description)
        {
            // A name asked for earlier is now this one
            auto name_it = sNames.find(id);
            if (name_it != sNames.end())
            {
                name_it->second = text;
            }
        }
    }

    S32 sent = 0;
    LLMessageSystem* msg = gMessageSystem;
    for (const auto& [region, local_ids] : per_region)
    {
        S32 in_message = 0;
        for (U32 local_id : local_ids)
        {
            if (in_message > 0 && (in_message >= MAX_OBJECTS_PER_MESSAGE || msg->isSendFull(nullptr)))
            {
                msg->sendReliable(region->getHost());
                in_message = 0;
            }
            if (in_message == 0)
            {
                msg->newMessageFast(description ? _PREHASH_ObjectDescription : _PREHASH_ObjectName);
                msg->nextBlockFast(_PREHASH_AgentData);
                msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
                msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
            }
            msg->nextBlockFast(_PREHASH_ObjectData);
            msg->addU32Fast(_PREHASH_LocalID, local_id);
            msg->addStringFast(description ? _PREHASH_Description : _PREHASH_Name, text);
            ++in_message;
            ++sent;
        }
        if (in_message > 0)
        {
            msg->sendReliable(region->getHost());
        }
    }
    return sent;
}

void FSObjectNames::requestName(LLViewerObject* object)
{
    if (!object || object->isDead() || object->isAvatar() || !object->getRegion())
    {
        return;
    }
    const F64 now = LLTimer::getElapsedSeconds();
    auto it = sNameRequests.find(object->getID());
    if (it != sNameRequests.end() && now - it->second < NAME_RETRY_SECONDS)
    {
        return;
    }
    if (sNameRequests.size() >= MAX_KEPT)
    {
        sNameRequests.clear();
    }
    sNameRequests[object->getID()] = now;
    LLSelectMgr::getInstance()->requestObjectPropertiesFamily(object);
}

bool FSObjectNames::getName(const LLUUID& id, std::string& name)
{
    auto it = sNames.find(id);
    if (it == sNames.end())
    {
        return false;
    }
    name = it->second;
    return true;
}

bool FSObjectNames::nameIsOld(const LLUUID& id)
{
    auto it = sNameRequests.find(id);
    return it == sNameRequests.end() || LLTimer::getElapsedSeconds() - it->second >= NAME_REFRESH_SECONDS;
}

void FSObjectNames::processPropertiesFamily(LLMessageSystem* msg)
{
    if (sNameRequests.empty() || !msg)
    {
        return;
    }
    LLUUID id;
    msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_ObjectID, id);
    if (sNameRequests.find(id) == sNameRequests.end())
    {
        return; // hover tips, pay and report answers are not ours
    }
    std::string name;
    msg->getStringFast(_PREHASH_ObjectData, _PREHASH_Name, name);
    if (sNames.size() >= MAX_KEPT)
    {
        sNames.clear();
    }
    sNames[id] = name;
}
