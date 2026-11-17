/**
 * @file fsvolumeprebuild.h
 * @brief Builds the shapes of prims for a new level of detail on worker threads ahead of the switch
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

#ifndef FS_VOLUMEPREBUILD_H
#define FS_VOLUMEPREBUILD_H

#include "stdtypes.h"

class LLVOVolume;

// FSPerfThreadedGeometry
//
// When a prim switches to a level of detail whose shape nobody has built yet, the stock viewer builds it
// on the main thread inside LLVOVolume::updateGeometry: LLVolumeLODGroup::refLOD constructs the LLVolume,
// which generates the path and profile, the mesh points and every volume face. After a teleport or while
// the camera moves through a prim build, thousands of objects change LOD within a few frames, and the
// unique shapes among them (twisted, cut, hollowed prims) are each built there.
//
// With the setting on, LLVOVolume::calcLOD asks lodReady before switching. When the shape for the new LOD
// is not built yet, a job for it (its volume parameters and LOD, nothing else) is posted to the General
// pool and the object keeps drawing its current LOD for this frame. The worker constructs the LLVolume
// exactly as refLOD would, detached from the volume manager: no LOD group, no map, no object. On the
// object's next calcLOD the switch goes ahead whatever state the job is in, so the switch is at most one
// frame later than stock. refLOD, on the main thread, then takes the built shape from here through
// LLVolumeLODGroup::sFSPrebuiltLOD instead of building one; if the job hasn't started the main thread runs
// it itself, if a worker is on it the main thread waits for that one shape. The shape is the one refLOD
// would have built, so what is drawn is identical.
//
// Only plain prims in a LOD group go through here. Sculpts (built from their texture), meshes (mesh
// threads), flexible and other unique shapes, selected objects and their linksets, and HUD attachments
// keep the stock path. Shape changes from the region or from the build tools never wait: they call
// setVolume directly, and refLOD takes a matching built shape if there is one, else builds it.
//
// Jobs hold no pointer to any object, region or LOD group, only a copy of the parameters, so killed
// objects, teleports and region removal need nothing. Jobs nobody took are dropped after 120 frames.
class FSVolumePrebuild
{
public:
    // LLVOVolume::calcLOD, main thread, before volobj switches to lod: false keeps its current LOD for this
    // frame while the new shape is built on a worker. Always true with the setting off.
    static bool lodReady(LLVOVolume* volobj, S32 lod);

    // Once a frame on the main thread: drops the jobs nobody took.
    static void onFrame();

    // Before the volume manager is cleaned up.
    static void cleanup();
};

#endif // FS_VOLUMEPREBUILD_H
