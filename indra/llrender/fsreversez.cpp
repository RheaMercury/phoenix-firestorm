/**
 * @file fsreversez.cpp
 * @brief Reverse-Z depth (FSRenderReverseZ), see fsreversez.h.
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

#include "fsreversez.h"

#include "llgl.h"
#include "llrender.h"

namespace
{
    bool sEnabled = false;
    bool sActive = false;
    GLenum sDepthFunc = GL_LESS;    // the standard function last asked for (GL's initial one)

    // R * proj: the depth row becomes (w - z) / 2, which takes the standard NDC depth in [-1, 1]
    // to [1, 0]. Only row 2 changes, so x, y and w (and every on-screen position) are untouched.
    glm::mat4 reverse(const glm::mat4& proj)
    {
        glm::mat4 r = proj;
        for (int c = 0; c < 4; ++c)
        {
            r[c][2] = 0.5f * (proj[c][3] - proj[c][2]);
        }
        return r;
    }

    // The clip convention, depth clear value and depth function for sActive. When the convention
    // changed (reload_projection), the projection the shaders hold changes with it: loading the same
    // matrix again bumps its hash, so every program takes it anew through LLRender::syncMatrices.
    // The once a frame re-apply of an unchanged convention leaves it be.
    void apply(bool reload_projection)
    {
#if LL_GL_FUNC_POINTER
        if (!glClipControl)
        {
            return;
        }
        gGL.flush();
        glClipControl(GL_LOWER_LEFT, sActive ? GL_ZERO_TO_ONE : GL_NEGATIVE_ONE_TO_ONE);
        glClearDepth(sActive ? 0.0 : 1.0);
        glDepthFunc(FSReverseZ::glDepthFuncFor(sDepthFunc));
        if (!reload_projection)
        {
            return;
        }

        const LLRender::eMatrixMode mode = gGL.getMatrixMode();
        gGL.matrixMode(LLRender::MM_PROJECTION);
        const glm::mat4 proj = gGL.getProjectionMatrix();
        gGL.loadMatrix((const GLfloat*)&proj[0][0]);
        gGL.matrixMode(mode);
#endif
    }

    void setActive(bool on)
    {
        if (on != sActive)
        {
            sActive = on;
            apply(true);
        }
    }
}

namespace FSReverseZ
{
    bool supported()
    {
#if LL_GL_FUNC_POINTER
        return glClipControl != nullptr && gGLManager.mGLVersion >= 2.99f;
#else
        return false;
#endif
    }

    bool enabled()
    {
        return sEnabled;
    }

    void setEnabled(bool on)
    {
        on = on && supported();
        if (on != sEnabled)
        {
            LL_INFOS("RenderInit") << "Reverse-Z depth " << (on ? "on" : "off") << LL_ENDL;
        }
        sEnabled = on;
        setActive(on);
    }

    bool active()
    {
        return sActive;
    }

    void applyState()
    {
        if (sEnabled)
        {
            apply(false);
        }
    }

    StandardScope::StandardScope()
    : mWasActive(sActive)
    {
        setActive(false);
    }

    StandardScope::~StandardScope()
    {
        setActive(mWasActive);
    }

    glm::mat4 rasterProjection(const glm::mat4& proj)
    {
        return sActive ? reverse(proj) : proj;
    }

    glm::mat4 depthProjection(const glm::mat4& proj)
    {
        return sEnabled ? reverse(proj) : proj;
    }

    GLenum glDepthFuncFor(GLenum func)
    {
        if (!sActive)
        {
            return func;
        }
        switch (func)
        {
        case GL_LESS:       return GL_GREATER;
        case GL_LEQUAL:     return GL_GEQUAL;
        case GL_GREATER:    return GL_LESS;
        case GL_GEQUAL:     return GL_LEQUAL;
        default:            return func;    // GL_ALWAYS, GL_NEVER, GL_EQUAL, GL_NOTEQUAL
        }
    }

    void depthFunc(GLenum func)
    {
        sDepthFunc = func;
        glDepthFunc(glDepthFuncFor(func));
    }

    void polygonOffset(F32 factor, F32 units)
    {
        if (sActive)
        {
            glPolygonOffset(-factor, -units);
        }
        else
        {
            glPolygonOffset(factor, units);
        }
    }

    GLenum depthFormat()
    {
        return sActive ? GL_DEPTH_COMPONENT32F : GL_DEPTH_COMPONENT24;
    }

    F32 toStandardDepth(F32 depth)
    {
        return sEnabled ? 1.f - depth : depth;
    }
}
