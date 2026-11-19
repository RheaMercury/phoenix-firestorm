/**
 * @file fsglretire.h
 * @brief Releases bindless texture handles, and the textures behind them, only
 *        once the GPU has finished every frame that could sample them.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
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

#ifndef FS_GLRETIRE_H
#define FS_GLRETIRE_H

#include "stdtypes.h"

// GL keeps a bound texture, a buffer or a sampler alive until the GPU is done with it, but not a bindless
// handle: GL_ARB_bindless_texture leaves sampling a handle that went non resident, or whose texture or
// sampler is gone, undefined ("may lead to application termination"), and making it non resident does not
// wait for the frames already submitted that sample it. With the render thread and the GPU each a frame or
// two behind the main thread, releasing a handle when the main thread stops using it left the GPU reading
// freed memory: a GPU page fault, and the driver ending the viewer.
//
// So a handle, and a texture that had handles, are retired here instead: kept as they are until a fence
// recorded after everything that could use them has signalled, then made non resident, their sampler
// deleted (which deletes the handle), and the texture deleted, in that order. The fences are recorded and
// tested in stream order through FSGLStream::recordCall (directly when the render thread doesn't run),
// never waited on: a test that finds one unfinished just tries again the next frame.
//
// Nothing here runs while nothing was retired. Main thread.
namespace FSGLRetire
{
    // The function that makes a handle non resident (the bindless code loads that entry point itself). It
    // runs on the GL thread, in stream order.
    void setNonResidentFunc(void (*fn)(U64 handle));

    // A handle the GPU may still sample: made non resident (when resident) and its sampler deleted, once the
    // frames recorded so far are done. sampler may be 0.
    void retireHandle(U64 handle, bool resident, U32 sampler);

    // A texture that had bindless handles (retired before it): deleted once the frames recorded so far are done.
    void retireTexture(U32 name);

    // Once per frame (LLImageGL::updateClass): a fence after what was retired since the last one, a test of
    // the fences not known to be done yet, and the release of what they cleared.
    void frame();

    // Waits for the GPU (glFinish in stream order) and releases everything retired. For shader reloads and
    // the GL going away, which are rare; does nothing when nothing waits.
    void finish();

    struct Stats
    {
        U32 mHandlesRetired = 0;    // since the last takeStats
        U32 mTexturesRetired = 0;
        U32 mWaiting = 0;           // handles and textures retired, not yet released
        U32 mReleasedEarly = 0;     // released without a fence or finish clearing them (after a long wait); 0
    };
    Stats takeStats();
}

#endif // FS_GLRETIRE_H
