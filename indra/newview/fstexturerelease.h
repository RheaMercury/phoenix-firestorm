/**
 * @file fstexturerelease.h
 * @brief FSTextureReleaseFaceless: video memory of textures on no face is handed back after a while
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, Trish
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

#ifndef FS_TEXTURERELEASE_H
#define FS_TEXTURERELEASE_H

#include "stdtypes.h"

class LLViewerTexture;

// Textures of objects that went away (a region left behind, objects out of view the region
// dropped, avatars gone) lose all their faces but keep their full size in video memory until the
// last reference goes. Textures only the texture list holds are deleted 30 s later; PBR textures
// are held a further 30 s by the material cache, and some are held for good by references that
// never draw them. Meanwhile they keep usage up and the discard bias with it, so everything in
// view loads blurrier. Stock released them only once memory was already over its limit.
//
// LLViewerFetchedTexture::fsFacelessReleaseDue decides which may go (unboosted scene textures on
// no face for FSTextureReleaseFacelessDelay seconds that nothing else draws or asks for); they
// are shrunk to their smallest size through the existing downscale queue, at a bounded rate.
// When one comes back on a face it loads again as any other texture does.
namespace FSTextureRelease
{
    bool enabled();             // FSTextureReleaseFaceless
    bool active();              // enabled, and not while the moving hold keeps resident textures as they are
    F32 delay();                // FSTextureReleaseFacelessDelay, clamped

    // Called once a frame from LLViewerTexture::updateClass: refills the budget, refreshes the
    // particle source set and logs what was released.
    void updateClass();

    // A particle source emits with this texture; between bursts it is on no face.
    bool usedByParticles(const LLViewerTexture* tex);

    // Takes one shrink of a texture this size from the budget; false when this frame's is spent.
    bool takeBudget(S64 bytes);

    // The downscale queue shrank a faceless texture by this many bytes.
    void noteReleased(S64 bytes);
}

#endif // FS_TEXTURERELEASE_H
