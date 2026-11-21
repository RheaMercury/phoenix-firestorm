/**
 * @file fsshadowcacheoffenders.h
 * @brief FSPerfShadowStaticCache: which objects make the far shadow cache re-render, and keeping
 *        objects that keep changing out of it
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

#ifndef FS_SHADOWCACHEOFFENDERS_H
#define FS_SHADOWCACHEOFFENDERS_H

#include "stdtypes.h"

class LLDrawable;
class LLVector4a;

// The far sun cascade static cache (FSPerfShadowStaticCache, pipeline.cpp) is dropped whenever static
// geometry inside a cached cascade changes (LLSpatialGroup hooks in llspatialpartition.cpp report it). This
// file keeps track of who reported what:
//  - FSPerfShadowBatchStats: every 10 seconds, the objects that dropped the cache most often (UUID, kind of
//    change, size of the group, distance) and what was ignored;
//  - FSPerfShadowStaticCacheFilter: changes that can't alter the cached shadows are ignored: spatial bridges
//    (the stand-ins of moving objects in the static octree, whose geometry is always drawn as dynamic)
//    entering or leaving a static group, and groups smaller than the cascade's smallest caster (also when
//    FSRenderMovingShadowCache isn't on);
//  - FSPerfShadowStaticCacheDemote: an object that keeps dropping the cache (3 times in 10 seconds, while the
//    camera is still) or is known to animate (target omega, flexible, texture animation) is made active, as
//    the viewer does itself for moving objects and objects the server updates often: it then has a spatial
//    bridge and is drawn with the dynamic casters every update, and no longer drops the cache. Once it has
//    been still for a minute (doubling each time it is demoted again, up to 16 minutes) it is made static
//    again. Region changes and teleports forget all of it (objects then stay as they are).
// Main thread only.
namespace FSShadowCacheOffenders
{
    // What a change note was
    enum EKind : U8
    {
        KIND_UNKNOWN = 0,
        KIND_ADDED,         // an object was added to a static group (rezzed, or stopped moving)
        KIND_REMOVED,       // removed (deleted, or started moving)
        KIND_REBUILT,       // its shape, detail or vertex positions were rebuilt
        KIND_RECOLOURED,    // rebuilt without moving a vertex (colour, texture, material)
        KIND_REGROUPED,     // the group was rebuilt with no object of it changed (objects joined or left it)
        KIND_COUNT
    };

    // FSPerfShadowStaticCacheFilter (default on; does nothing unless FSPerfShadowStaticCache is on)
    bool filterEnabled();

    // llspatialpartition.cpp, around each note it makes: the drawable the next fsNoteShadowStaticChange is
    // about (null: the group as a whole), and back to none after it
    void setCulprit(const LLDrawable* drawable, EKind kind);
    void clearCulprit();
    // A note skipped because it was about a spatial bridge
    void noteBridgeSkipped();

    // pipeline.cpp fsNoteShadowStaticChange: one entry per extents pair it stores, taking the culprit
    void pushEntry(size_t pair_index);
    // Wherever the pairs are cleared
    void clearEntries();

    // fsShadowCacheBeginFrame: whether invalidations should be attributed (stats or demotion on)
    bool tracking();
    // Entry pair_index dropped a cached far cascade (or would have, had another not already)
    void noteInvalidation(size_t pair_index, const LLVector4a& lo, const LLVector4a& hi, F32 caster_size);
    // Changes ignored as too small to cast into the cached map
    void noteSmallIgnored();

    // Once per far cascade update while the cache is in use: logging, demotions, promotions
    void update();
    // The cache is off: forget everything (demoted objects stay active)
    void reset();
}

#endif // FS_SHADOWCACHEOFFENDERS_H
