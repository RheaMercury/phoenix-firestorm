/**
 * @file fsfloaterbuildcheckpoints.cpp
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

#include "llviewerprecompiledheaders.h"

#include "fsfloaterbuildcheckpoints.h"

#include "fsbuildhistory.h"
#include "llagent.h"
#include "lldir.h"
#include "llfile.h"
#include "llfloaterreg.h"
#include "llnotificationsutil.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "llsdserialize.h"
#include "llsdutil.h"
#include "llselectmgr.h"
#include "llviewermenu.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "message.h"
#include "pipeline.h"
#include "rlvactions.h"

#include <algorithm>
#include <map>
#include <set>

namespace
{
    constexpr size_t MAX_NAME_LENGTH = 64;
    constexpr S32 MAX_BLOCKS_PER_PACKET = 254; // as LLSelectMgr::sendListToRegions
    const std::string CHECKPOINTS_FILE = "build_checkpoints.xml";

    std::string checkpoints_filename()
    {
        return gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, CHECKPOINTS_FILE);
    }

    // Names are trimmed and kept short so the list stays readable; an empty name means "cancel"
    std::string clean_name(std::string name)
    {
        LLStringUtil::trim(name);
        if (name.size() > MAX_NAME_LENGTH)
        {
            name = utf8str_truncate(name, (S32)MAX_NAME_LENGTH);
            LLStringUtil::trim(name);
        }
        return name;
    }

    bool is_vector3(const LLSD& sd)
    {
        return sd.isArray() && sd.size() == 3 && LLVector3(sd).isFinite();
    }

    bool is_quaternion(const LLSD& sd)
    {
        return sd.isArray() && sd.size() == 4 && LLQuaternion(sd).isFinite();
    }

    // An object entry the restore can use; anything else (a file edited by hand) is left out on load
    bool is_valid_entry(const LLSD& entry)
    {
        return entry.isMap() && entry["id"].asUUID().notNull() &&
               is_vector3(entry["position"]) && is_quaternion(entry["rotation"]) && is_vector3(entry["scale"]) &&
               is_vector3(entry["region_position"]) && is_quaternion(entry["region_rotation"]);
    }

    bool can_edit(const LLViewerObject* object)
    {
        return !RlvActions::isRlvEnabled() || RlvActions::canEdit(object);
    }

    // Every prim of the selected linksets, roots first. Each entry keeps the local transform (the
    // region one for roots, relative to the root for children, as the simulator stores them) and the
    // region transform, used when the object was linked to another root since.
    LLSD capture_selection(std::string& root_name, std::string& region_name, U64& region_handle)
    {
        LLObjectSelectionHandle selection = LLSelectMgr::getInstance()->getSelection();

        std::vector<LLViewerObject*> roots;
        for (LLObjectSelection::iterator iter = selection->begin(); iter != selection->end(); ++iter)
        {
            LLViewerObject* object = (*iter)->getObject();
            if (!object || object->isDead() || object->isAvatar() || object->isAttachment() || !object->getRegion())
            {
                continue;
            }
            LLViewerObject* root = object->getRootEdit();
            if (root && !root->isAvatar() && !root->isAttachment() && std::find(roots.begin(), roots.end(), root) == roots.end())
            {
                roots.push_back(root);
            }
        }

        LLSD objects = LLSD::emptyArray();
        if (roots.empty())
        {
            return objects;
        }

        LLSelectNode* root_node = selection->getFirstRootNode(nullptr, true);
        root_name = (root_node && root_node->mValid) ? root_node->mName : std::string();
        region_name = roots.front()->getRegion()->getName();
        region_handle = roots.front()->getRegion()->getHandle();

        std::set<LLUUID> seen;
        for (LLViewerObject* root : roots)
        {
            std::vector<LLViewerObject*> family;
            root->addThisAndNonJointChildren(family);
            for (LLViewerObject* object : family)
            {
                if (!object || object->isDead() || object->isAvatar() || !object->getRegion() || !seen.insert(object->getID()).second)
                {
                    continue;
                }
                LLSD entry;
                entry["id"] = object->getID();
                entry["root"] = (object == root);
                entry["parent"] = (object == root) ? LLUUID::null : root->getID();
                entry["position"] = object->getPosition().getValue();
                entry["rotation"] = object->getRotation().getValue();
                entry["scale"] = object->getScale().getValue();
                entry["region_position"] = object->getPositionRegion().getValue();
                entry["region_rotation"] = object->getRotationRegion().getValue();
                entry["region"] = object->getRegion()->getName();
                entry["region_handle"] = ll_sd_from_U64(object->getRegion()->getHandle());
                objects.append(entry);
            }
        }
        return objects;
    }

    // One ObjectData block of a MultipleObjectUpdate
    struct UpdateBlock
    {
        U32             mLocalID;
        U8              mType;
        LLVector3       mPosition;
        LLQuaternion    mRotation;
        LLVector3       mScale;
    };

    // Packs the blocks like LLSelectMgr::packMultipleUpdate, as many per message as fit
    void send_updates(const std::map<LLViewerRegion*, std::vector<UpdateBlock>>& blocks_by_region)
    {
        LLMessageSystem* msg = gMessageSystem;
        for (const auto& [region, blocks] : blocks_by_region)
        {
            S32 in_packet = 0;
            for (const UpdateBlock& block : blocks)
            {
                if (in_packet > 0 && (msg->isSendFull(nullptr) || in_packet >= MAX_BLOCKS_PER_PACKET))
                {
                    msg->sendReliable(region->getHost());
                    in_packet = 0;
                }
                if (in_packet == 0)
                {
                    msg->newMessageFast(_PREHASH_MultipleObjectUpdate);
                    msg->nextBlockFast(_PREHASH_AgentData);
                    msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
                    msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
                }

                // The simulator reads them in this order
                U8 data[256];
                S32 offset = 0;
                if (block.mType & UPD_POSITION)
                {
                    htolememcpy(&data[offset], &(block.mPosition.mV), MVT_LLVector3, 12);
                    offset += 12;
                }
                if (block.mType & UPD_ROTATION)
                {
                    LLVector3 packed = block.mRotation.packToVector3();
                    htolememcpy(&data[offset], &(packed.mV), MVT_LLQuaternion, 12);
                    offset += 12;
                }
                if (block.mType & UPD_SCALE)
                {
                    htolememcpy(&data[offset], &(block.mScale.mV), MVT_LLVector3, 12);
                    offset += 12;
                }
                msg->nextBlockFast(_PREHASH_ObjectData);
                msg->addU32Fast(_PREHASH_ObjectLocalID, block.mLocalID);
                msg->addU8Fast(_PREHASH_Type, block.mType);
                msg->addBinaryDataFast(_PREHASH_Data, data, offset);
                ++in_packet;
            }
            if (in_packet > 0)
            {
                msg->sendReliable(region->getHost());
            }
        }
    }

    bool position_differs(const LLVector3& a, const LLVector3& b)
    {
        return dist_vec_squared(a, b) > 1.0e-8f;
    }

    bool rotation_differs(const LLQuaternion& a, const LLQuaternion& b)
    {
        return !a.isEqualEps(b, 1.0e-5f);
    }
}

FSFloaterBuildCheckpoints::FSFloaterBuildCheckpoints(const LLSD& key)
    : LLFloater(key),
      mCheckpoints(LLSD::emptyArray())
{
}

bool FSFloaterBuildCheckpoints::postBuild()
{
    mList = getChild<LLScrollListCtrl>("checkpoints");
    mList->setCommitOnSelectionChange(true);
    mList->setCommitCallback([this](LLUICtrl*, const LLSD&) { updateButtons(); });
    mList->setDoubleClickCallback([this]() { onClickRestore(); });

    getChild<LLUICtrl>("save")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickSave(); });
    getChild<LLUICtrl>("restore")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickRestore(); });
    getChild<LLUICtrl>("select")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickSelect(); });
    getChild<LLUICtrl>("rename")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickRename(); });
    getChild<LLUICtrl>("delete")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickDelete(); });

    load();
    refreshList();
    return true;
}

//static
void FSFloaterBuildCheckpoints::saveSelection()
{
    if (FSFloaterBuildCheckpoints* floater = LLFloaterReg::showTypedInstance<FSFloaterBuildCheckpoints>("fs_build_checkpoints"))
    {
        floater->onClickSave();
    }
}

void FSFloaterBuildCheckpoints::load()
{
    mCheckpoints = LLSD::emptyArray();

    const std::string filename = checkpoints_filename();
    if (!LLFile::isfile(filename))
    {
        return;
    }
    llifstream file(filename.c_str());
    if (!file.is_open())
    {
        LL_WARNS() << "Couldn't open build checkpoints file " << filename << LL_ENDL;
        return;
    }
    LLSD data;
    LLSDSerialize::fromXMLDocument(data, file);
    file.close();
    if (!data.isArray())
    {
        return;
    }

    // Entries edited by hand into something unusable are left out instead of breaking the list
    for (size_t i = 0; i < data.size(); ++i)
    {
        const LLSD& entry = data[i];
        if (!entry.isMap() || !entry.has("name") || !entry["objects"].isArray())
        {
            continue;
        }
        LLSD checkpoint = entry;
        if (checkpoint["id"].asUUID().isNull())
        {
            checkpoint["id"] = LLUUID::generateNewID();
        }
        LLSD objects = LLSD::emptyArray();
        for (size_t j = 0; j < entry["objects"].size(); ++j)
        {
            if (is_valid_entry(entry["objects"][j]))
            {
                objects.append(entry["objects"][j]);
            }
        }
        checkpoint["objects"] = objects;
        mCheckpoints.append(checkpoint);
    }
}

void FSFloaterBuildCheckpoints::save() const
{
    const std::string filename = checkpoints_filename();
    llofstream file(filename.c_str());
    if (!file.is_open())
    {
        LL_WARNS() << "Couldn't save build checkpoints file " << filename << LL_ENDL;
        return;
    }
    LLSDSerialize::toPrettyXML(mCheckpoints, file);
    file.close();
}

void FSFloaterBuildCheckpoints::refreshList(const LLUUID& select_id)
{
    const S32 scroll = mList->getScrollPos();
    mList->deleteAllItems();

    for (size_t i = 0; i < mCheckpoints.size(); ++i)
    {
        const LLSD& checkpoint = mCheckpoints[i];

        std::string date = getString("date_format");
        LLSD substitution;
        substitution["datetime"] = checkpoint["date"].asDate();
        LLStringUtil::format(date, substitution);

        LLSD row;
        row["value"] = checkpoint["id"];
        LLSD& columns = row["columns"];
        columns[0]["column"] = "name";
        columns[0]["value"] = checkpoint["name"].asString();
        columns[1]["column"] = "count";
        columns[1]["value"] = llformat("%d", (S32)checkpoint["objects"].size());
        columns[2]["column"] = "region";
        columns[2]["value"] = checkpoint["region"].asString();
        columns[3]["column"] = "date";
        columns[3]["value"] = date;
        mList->addElement(row);
    }

    mList->setScrollPos(scroll);
    if (select_id.notNull())
    {
        mList->selectByID(select_id);
        mList->scrollToShowSelected();
    }
    updateButtons();
}

void FSFloaterBuildCheckpoints::updateButtons()
{
    const bool selected = selectedId().notNull();
    childSetEnabled("restore", selected);
    childSetEnabled("select", selected);
    childSetEnabled("rename", selected);
    childSetEnabled("delete", selected);
}

void FSFloaterBuildCheckpoints::setStatus(const std::string& text)
{
    getChild<LLUICtrl>("status")->setValue(text);
}

LLUUID FSFloaterBuildCheckpoints::selectedId() const
{
    LLScrollListItem* item = mList ? mList->getFirstSelected() : nullptr;
    if (!item)
    {
        return LLUUID::null;
    }
    const LLUUID id = item->getValue().asUUID();
    return findIndex(id) >= 0 ? id : LLUUID::null;
}

S32 FSFloaterBuildCheckpoints::findIndex(const LLUUID& id) const
{
    if (id.isNull())
    {
        return -1;
    }
    for (S32 i = 0; i < (S32)mCheckpoints.size(); ++i)
    {
        if (mCheckpoints[i]["id"].asUUID() == id)
        {
            return i;
        }
    }
    return -1;
}

void FSFloaterBuildCheckpoints::onClickSave()
{
    std::string root_name;
    std::string region_name;
    U64 region_handle = 0;
    const LLSD objects = capture_selection(root_name, region_name, region_handle);
    if (objects.size() == 0)
    {
        LLNotificationsUtil::add("FSBuildCheckpointNothingSelected");
        return;
    }

    // Taken now, so the checkpoint holds the objects as they were when Save was clicked, not after
    // the name was typed
    LLSD checkpoint;
    checkpoint["id"] = LLUUID::generateNewID();
    checkpoint["date"] = LLDate::now();
    checkpoint["region"] = region_name;
    checkpoint["region_handle"] = ll_sd_from_U64(region_handle);
    checkpoint["objects"] = objects;

    std::string time = getString("date_format");
    LLSD substitution;
    substitution["datetime"] = LLDate::now();
    LLStringUtil::format(time, substitution);

    LLStringUtil::format_map_t name_args;
    name_args["[NAME]"] = root_name.empty() ? getString("default_object_name") : root_name;
    name_args["[TIME]"] = time;

    LLSD args;
    args["NAME"] = clean_name(getString("default_name", name_args));
    LLHandle<FSFloaterBuildCheckpoints> handle = getDerivedHandle<FSFloaterBuildCheckpoints>();
    LLNotificationsUtil::add("FSBuildCheckpointName", args, LLSD(),
        [handle, checkpoint](const LLSD& notification, const LLSD& response)
        {
            FSFloaterBuildCheckpoints* self = handle.get();
            if (self && LLNotificationsUtil::getSelectedOption(notification, response) == 0)
            {
                self->addCheckpoint(response["message"].asString(), checkpoint);
            }
        });
}

void FSFloaterBuildCheckpoints::addCheckpoint(std::string name, LLSD checkpoint)
{
    name = clean_name(name);
    if (name.empty())
    {
        return;
    }
    checkpoint["name"] = name;
    mCheckpoints.append(checkpoint);
    save();
    refreshList(checkpoint["id"].asUUID());

    LLStringUtil::format_map_t args;
    args["[NAME]"] = name;
    args["[COUNT]"] = llformat("%d", (S32)checkpoint["objects"].size());
    setStatus(getString("saved", args));
}

void FSFloaterBuildCheckpoints::onClickRestore()
{
    const LLUUID id = selectedId();
    const S32 index = findIndex(id);
    if (index < 0)
    {
        return;
    }
    LLSD args;
    args["NAME"] = mCheckpoints[index]["name"].asString();
    args["COUNT"] = (S32)mCheckpoints[index]["objects"].size();
    LLHandle<FSFloaterBuildCheckpoints> handle = getDerivedHandle<FSFloaterBuildCheckpoints>();
    LLNotificationsUtil::add("FSBuildCheckpointRestore", args, LLSD(),
        [handle, id](const LLSD& notification, const LLSD& response)
        {
            FSFloaterBuildCheckpoints* self = handle.get();
            if (self && LLNotificationsUtil::getSelectedOption(notification, response) == 0)
            {
                self->restoreCheckpoint(id);
            }
        });
}

// Roots go back with UPD_LINKED_SETS, so their children follow and keep their offsets, and a root's
// scale on its own, as scaling with UPD_LINKED_SETS would scale the whole linkset. Children are set
// relative to their root, so the order the simulator applies the blocks in doesn't matter.
void FSFloaterBuildCheckpoints::restoreCheckpoint(const LLUUID& id)
{
    const S32 index = findIndex(id);
    if (index < 0)
    {
        return;
    }
    const LLSD objects = mCheckpoints[index]["objects"];

    struct Target
    {
        LLViewerObject* mObject;
        LLVector3       mPosition;  // local: the region for roots, the root for children
        LLQuaternion    mRotation;
        LLVector3       mScale;
    };
    std::vector<Target> roots;
    std::vector<std::pair<LLViewerObject*, LLSD>> children;
    std::map<LLUUID, std::pair<LLVector3, LLQuaternion>> root_targets; // region transforms the roots get

    S32 missing = 0;
    S32 no_mod = 0;
    S32 other_region = 0;

    LLWorld* world = LLWorld::getInstance();
    const F32 min_scale = world->getRegionMinPrimScale();
    const F32 max_scale = world->getRegionMaxPrimScale();

    for (size_t i = 0; i < objects.size(); ++i)
    {
        const LLSD& entry = objects[i];
        LLViewerObject* object = gObjectList.findObject(entry["id"].asUUID());
        if (!object || object->isDead())
        {
            ++missing;
            continue;
        }
        // An object taken and worn, or one that crossed into another region, can't be put back
        // from region coordinates
        if (object->isAvatar() || object->isAttachment() || object->getRootEdit()->isAttachment() || !object->getRegion())
        {
            ++no_mod;
            continue;
        }
        if (entry.has("region_handle") && object->getRegion()->getHandle() != ll_U64_from_sd(entry["region_handle"]))
        {
            ++other_region;
            continue;
        }
        if (!object->permModify() || !object->permMove() || object->isPermanentEnforced() || !can_edit(object))
        {
            ++no_mod;
            continue;
        }

        if (object->isRoot())
        {
            Target target;
            target.mObject = object;
            target.mPosition = LLVector3(entry["region_position"]);
            target.mRotation = LLQuaternion(entry["region_rotation"]);
            target.mScale = LLVector3(entry["scale"]);
            roots.push_back(target);
            root_targets[object->getID()] = std::make_pair(target.mPosition, target.mRotation);
        }
        else
        {
            children.emplace_back(object, entry);
        }
    }

    std::vector<Target> child_targets;
    for (const auto& [object, entry] : children)
    {
        LLViewerObject* parent = (LLViewerObject*)object->getParent();
        if (!parent)
        {
            ++missing;
            continue;
        }
        Target target;
        target.mObject = object;
        target.mScale = LLVector3(entry["scale"]);
        if (!entry["root"].asBoolean() && entry["parent"].asUUID() == parent->getID())
        {
            // still in the same linkset: the offsets from the root are what was saved
            target.mPosition = LLVector3(entry["position"]);
            target.mRotation = LLQuaternion(entry["rotation"]);
        }
        else
        {
            // linked to another root since: placed where it was in the region, relative to where
            // its root is now, or is about to be put back
            LLVector3 parent_position = parent->getPositionRegion();
            LLQuaternion parent_rotation = parent->getRotationRegion();
            auto found = root_targets.find(parent->getID());
            if (found != root_targets.end())
            {
                parent_position = found->second.first;
                parent_rotation = found->second.second;
            }
            const LLQuaternion inverse = ~parent_rotation;
            target.mPosition = (LLVector3(entry["region_position"]) - parent_position) * inverse;
            target.mRotation = LLQuaternion(entry["region_rotation"]) * inverse;
        }
        child_targets.push_back(target);
    }

    for (Target& target : roots)
    {
        target.mScale.clamp(min_scale, max_scale);
    }
    for (Target& target : child_targets)
    {
        target.mScale.clamp(min_scale, max_scale);
    }

    // The build undo history records three steps at most, newest undone first: the roots' moves
    // (as linksets), the roots' own scale, then the children's offsets
    FSBuildHistory& history = FSBuildHistory::instance();
    history.closeStep();
    for (const Target& target : child_targets)
    {
        history.noteBaseline(target.mObject);
    }
    for (const Target& target : roots)
    {
        history.noteBaseline(target.mObject);
    }

    std::map<LLViewerRegion*, std::vector<UpdateBlock>> blocks;
    std::set<LLViewerObject*> moved;

    // Children first: their offsets don't depend on the root, so nothing is in the way
    std::vector<LLViewerObject*> changed_children;
    for (const Target& target : child_targets)
    {
        LLViewerObject* object = target.mObject;
        if (!position_differs(object->getPosition(), target.mPosition) &&
            !rotation_differs(object->getRotation(), target.mRotation) &&
            !position_differs(object->getScale(), target.mScale))
        {
            continue;
        }
        object->setPosition(target.mPosition);
        object->setRotation(target.mRotation);
        object->setScale(target.mScale);
        blocks[object->getRegion()].push_back({ object->getLocalID(), (U8)(UPD_POSITION | UPD_ROTATION | UPD_SCALE),
                                                target.mPosition, target.mRotation, target.mScale });
        changed_children.push_back(object);
        moved.insert(object);
    }
    history.onUpdateSent(changed_children, UPD_POSITION | UPD_ROTATION | UPD_SCALE);
    history.closeStep();

    std::vector<LLViewerObject*> scaled_roots;
    for (const Target& target : roots)
    {
        LLViewerObject* object = target.mObject;
        if (!position_differs(object->getScale(), target.mScale))
        {
            continue;
        }
        object->setScale(target.mScale);
        blocks[object->getRegion()].push_back({ object->getLocalID(), UPD_SCALE, target.mPosition, target.mRotation, target.mScale });
        scaled_roots.push_back(object);
        moved.insert(object);
    }
    history.onUpdateSent(scaled_roots, UPD_SCALE);
    history.closeStep();

    std::vector<LLViewerObject*> moved_roots;
    for (const Target& target : roots)
    {
        LLViewerObject* object = target.mObject;
        if (!position_differs(object->getPosition(), target.mPosition) &&
            !rotation_differs(object->getRotation(), target.mRotation))
        {
            continue;
        }
        object->setPosition(target.mPosition);
        object->setRotation(target.mRotation);
        blocks[object->getRegion()].push_back({ object->getLocalID(), (U8)(UPD_POSITION | UPD_ROTATION | UPD_LINKED_SETS),
                                                target.mPosition, target.mRotation, target.mScale });
        moved_roots.push_back(object);
        moved.insert(object);
    }
    history.onUpdateSent(moved_roots, UPD_POSITION | UPD_ROTATION | UPD_LINKED_SETS);
    history.closeStep();

    for (LLViewerObject* object : moved)
    {
        if (object->mDrawable.notNull())
        {
            gPipeline.markMoved(object->mDrawable, false);
        }
    }
    send_updates(blocks);

    // The selected objects' saved transforms (manips, the next undo baseline) start from here
    LLSelectMgr::getInstance()->updateSelectionCenter();
    LLSelectMgr::getInstance()->saveSelectedObjectTransform(SELECT_ACTION_TYPE_PICK);

    LLStringUtil::format_map_t args;
    args["[RESTORED]"] = llformat("%d", (S32)(roots.size() + child_targets.size()));
    args["[MISSING]"] = llformat("%d", missing);
    args["[NO_MOD]"] = llformat("%d", no_mod);
    args["[OTHER_REGION]"] = llformat("%d", other_region);
    setStatus(getString("restored", args));
}

void FSFloaterBuildCheckpoints::onClickSelect()
{
    const S32 index = findIndex(selectedId());
    if (index < 0)
    {
        return;
    }
    const LLSD& checkpoint = mCheckpoints[index];
    const LLSD& objects = checkpoint["objects"];

    std::vector<LLViewerObject*> roots;
    S32 present = 0;
    for (size_t i = 0; i < objects.size(); ++i)
    {
        LLViewerObject* object = gObjectList.findObject(objects[i]["id"].asUUID());
        if (!object || object->isDead() || object->isAvatar() || object->isAttachment())
        {
            continue;
        }
        LLViewerObject* root = object->getRootEdit();
        if (!root || root->isAttachment() || !can_edit(root))
        {
            continue;
        }
        ++present;
        if (std::find(roots.begin(), roots.end(), root) == roots.end())
        {
            roots.push_back(root);
        }
    }

    if (roots.empty())
    {
        LLStringUtil::format_map_t args;
        args["[NAME]"] = checkpoint["name"].asString();
        setStatus(getString("nothing_present", args));
        return;
    }

    // Whole linksets are selected, as the checkpoint holds them; the build selection filters (only
    // own, movable or copyable objects) are ignored, as these objects were asked for by name
    LLSelectMgr::getInstance()->deselectAll();
    for (LLViewerObject* root : roots)
    {
        LLSelectMgr::getInstance()->selectObjectAndFamily(root, true, true);
    }
    if (LLSelectMgr::getInstance()->getSelection()->getObjectCount() > 0)
    {
        handle_object_edit();
    }

    LLStringUtil::format_map_t args;
    args["[SELECTED]"] = llformat("%d", present);
    args["[COUNT]"] = llformat("%d", (S32)objects.size());
    args["[MISSING]"] = llformat("%d", (S32)objects.size() - present);
    setStatus(getString("selected", args));
}

void FSFloaterBuildCheckpoints::onClickRename()
{
    const LLUUID id = selectedId();
    const S32 index = findIndex(id);
    if (index < 0)
    {
        return;
    }
    LLSD args;
    args["NAME"] = mCheckpoints[index]["name"].asString();
    LLHandle<FSFloaterBuildCheckpoints> handle = getDerivedHandle<FSFloaterBuildCheckpoints>();
    LLNotificationsUtil::add("FSBuildCheckpointName", args, LLSD(),
        [handle, id](const LLSD& notification, const LLSD& response)
        {
            FSFloaterBuildCheckpoints* self = handle.get();
            if (self && LLNotificationsUtil::getSelectedOption(notification, response) == 0)
            {
                self->renameCheckpoint(id, response["message"].asString());
            }
        });
}

void FSFloaterBuildCheckpoints::renameCheckpoint(const LLUUID& id, std::string name)
{
    name = clean_name(name);
    const S32 index = findIndex(id);
    if (name.empty() || index < 0)
    {
        return;
    }
    mCheckpoints[index]["name"] = name;
    save();
    refreshList(id);
}

void FSFloaterBuildCheckpoints::onClickDelete()
{
    const LLUUID id = selectedId();
    const S32 index = findIndex(id);
    if (index < 0)
    {
        return;
    }
    LLSD args;
    args["NAME"] = mCheckpoints[index]["name"].asString();
    LLHandle<FSFloaterBuildCheckpoints> handle = getDerivedHandle<FSFloaterBuildCheckpoints>();
    LLNotificationsUtil::add("FSBuildCheckpointDelete", args, LLSD(),
        [handle, id](const LLSD& notification, const LLSD& response)
        {
            FSFloaterBuildCheckpoints* self = handle.get();
            if (self && LLNotificationsUtil::getSelectedOption(notification, response) == 0)
            {
                self->deleteCheckpoint(id);
            }
        });
}

void FSFloaterBuildCheckpoints::deleteCheckpoint(const LLUUID& id)
{
    const S32 index = findIndex(id);
    if (index < 0)
    {
        return;
    }
    mCheckpoints.erase(index);
    save();

    // keeps a row selected, the one that took the deleted one's place
    LLUUID next_id;
    if (mCheckpoints.size() > 0)
    {
        next_id = mCheckpoints[llmin(index, (S32)mCheckpoints.size() - 1)]["id"].asUUID();
    }
    refreshList(next_id);
}
