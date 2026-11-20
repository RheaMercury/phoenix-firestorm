/**
 * @file fstaajitter.cpp
 * @brief Temporal anti-aliasing (FSRenderTAA): the projection jitter, see fstaajitter.h.
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

#include "linden_common.h"

#include "fstaajitter.h"

#include "llrender.h"

namespace
{
    bool sActive = false;
    F32 sX = 0.f;
    F32 sY = 0.f;
    F32 sLodBias = 0.f;
    bool sLodBiasCompiled = false;

    // Every program takes the projection anew at its next LLRender::syncMatrices: loading the same
    // matrix again bumps its hash (as FSReverseZ does when its convention changes). What is batched
    // in immediate mode goes out first, with the matrices it was made under.
    void refresh()
    {
        gGL.flush();
        const LLRender::eMatrixMode mode = gGL.getMatrixMode();
        gGL.matrixMode(LLRender::MM_PROJECTION);
        const glm::mat4 proj = gGL.getProjectionMatrix();
        gGL.loadMatrix((const GLfloat*)&proj[0][0]);
        gGL.matrixMode(mode);
    }
}

namespace FSTAAJitter
{
    void set(F32 ndc_x, F32 ndc_y)
    {
        if (sActive && sX == ndc_x && sY == ndc_y)
        {
            return;
        }
        sActive = true;
        sX = ndc_x;
        sY = ndc_y;
        refresh();
    }

    void clear()
    {
        if (!sActive)
        {
            return;
        }
        sActive = false;
        sX = 0.f;
        sY = 0.f;
        refresh();
    }

    bool active()
    {
        return sActive;
    }

    glm::mat4 apply(const glm::mat4& proj)
    {
        if (!sActive)
        {
            return proj;
        }
        // J * proj with J a translation of clip x and y by the offset times w: row 0 gains
        // sX * row 3, row 1 gains sY * row 3. glm is column major, m[column][row].
        glm::mat4 j = proj;
        for (int c = 0; c < 4; ++c)
        {
            j[c][0] += sX * proj[c][3];
            j[c][1] += sY * proj[c][3];
        }
        return j;
    }

    void setLodBias(F32 bias)
    {
        sLodBias = bias;
    }

    F32 lodBias()
    {
        return sActive ? sLodBias : 0.f;
    }

    void setLodBiasCompiled(bool compiled)
    {
        sLodBiasCompiled = compiled;
    }

    bool lodBiasCompiled()
    {
        return sLodBiasCompiled;
    }
}
