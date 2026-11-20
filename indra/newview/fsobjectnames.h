/**
 * @file fsobjectnames.h
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


#ifndef FS_OBJECTNAMES_H
#define FS_OBJECTNAMES_H

#include "llselectmgr.h"

// The name and description fields of the build floater are filled from the
// selection, but a line editor commits when it loses focus, which can happen
// after the selection has changed (clicking another link, the link spinner,
// Edit linked turning the whole linkset into a selection of its parts).
// Sending the text to "whatever is selected now" then renamed links the user
// never edited. These helpers remember which objects the fields were showing
// and send the text to those objects only.
namespace FSObjectNames
{
    // The objects the fields stand for: the root objects when whole linksets are
    // selected, otherwise every selected prim (as LLSelectMgr::selectionSetObjectName picks them)
    void collectTargets(LLObjectSelectionHandle selection, bool roots, uuid_vec_t& targets);

    // Sends the name (or the description) to the targets that still exist, can
    // be modified and are not locked by RLVa. A target whose known name (or
    // description) already is the text is left alone, and an empty name is
    // never sent. Returns how many objects it was sent to.
    S32 send(const uuid_vec_t& targets, const std::string& text, bool description);

    // The viewer only learns a prim's name when the prim is selected. The
    // Content tab's whole linkset view also labels the links that are not
    // selected; it asks the region for their names here (one small
    // RequestObjectPropertiesFamily each, at most once every 30 seconds per prim).
    void requestName(LLViewerObject* object);
    // A name asked for with requestName(), once the answer arrived
    bool getName(const LLUUID& id, std::string& name);
    // That answer is a minute old: the link may have been renamed since (scripts, other viewers)
    bool nameIsOld(const LLUUID& id);
    // From process_object_properties_family(): keeps the names that were asked for
    void processPropertiesFamily(LLMessageSystem* msg);
}

#endif // FS_OBJECTNAMES_H
