/**
 * @file fstoolmeasure.h
 * @brief Transient tool that picks the points of the Measure window, and its in-world drawing
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

#ifndef FS_TOOLMEASURE_H
#define FS_TOOLMEASURE_H

#include "lltool.h"

class LLPickInfo;

// <FS:Trish> Measuring tool. While the Measure window picks points, this tool is set as the
// transient tool so a left click in-world picks a surface point instead of touching, walking or
// sitting. Esc or a right click stops picking. The picked points are handed to FSFloaterMeasure,
// which keeps them; the line between them is drawn by renderInWorld() from render_hud_elements(),
// only while the window is open with at least one point (sRenderInWorld).
class FSToolMeasure : public LLTool, public LLSingleton<FSToolMeasure>
{
    LLSINGLETON(FSToolMeasure);
    virtual ~FSToolMeasure();

public:
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleDoubleClick(S32 x, S32 y, MASK mask) override;
    bool handleRightMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleRightMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleKey(KEY key, MASK mask) override;
    bool clipMouseWhenDown() override { return false; }

    void startPicking();
    void stopPicking();
    bool isPicking();

    // Set by the Measure window: true while it is open and has a point to draw
    static bool sRenderInWorld;

    // Draws the measured path in world space. Called from render_hud_elements() with gUIProgram
    // bound and the camera's modelview loaded, only when sRenderInWorld is set.
    static void renderInWorld();

private:
    static void pickCallback(const LLPickInfo& pick_info);
};
// </FS:Trish>

#endif // FS_TOOLMEASURE_H
