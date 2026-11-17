/**
 * @file fstoolmeasure.cpp
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

#include "llviewerprecompiledheaders.h"

#include "fstoolmeasure.h"

#include "fsfloatermeasure.h"
#include "llagent.h"
#include "llfloaterreg.h"
#include "llgl.h"
#include "llglstates.h"
#include "llrender.h"
#include "lltoolmgr.h"
#include "llviewercamera.h"
#include "llviewerobject.h"
#include "llviewerwindow.h"
#include "llworld.h"
#include "pipeline.h"
#include "v4color.h"

bool FSToolMeasure::sRenderInWorld = false;

namespace
{
    // LLPickInfo lifts a land pick this far above the ground (LLPickInfo::fetchResults())
    constexpr F64 LAND_PICK_FUDGE = 0.1;

    // On-screen sizes of what is drawn, in pixels
    constexpr F32 LINE_HALF_WIDTH_PX    = 1.5f;
    constexpr F32 NORMAL_HALF_WIDTH_PX  = 1.f;
    constexpr F32 NORMAL_LENGTH_PX      = 32.f;
    constexpr F32 MARKER_HALF_SIZE_PX   = 6.f;

    // Parts hidden behind something are drawn again, fainter, so the whole path stays readable
    constexpr F32 HIDDEN_ALPHA = 0.3f;

    const LLColor4 PATH_COLOR(1.f, 0.8f, 0.1f, 1.f);
    const LLColor4 NORMAL_COLOR(0.3f, 0.8f, 1.f, 1.f);
    const LLColor4 POINT_A_COLOR(0.2f, 1.f, 0.3f, 1.f);
    const LLColor4 POINT_B_COLOR(1.f, 0.3f, 0.3f, 1.f);
    const LLColor4 POINT_MID_COLOR(1.f, 0.8f, 0.1f, 1.f);

    struct DrawContext
    {
        LLVector3   mCameraOrigin;
        LLVector3   mCameraLeft;
        LLVector3   mCameraUp;
        F32         mPixelScale = 0.f;      // meters per pixel at one meter from the camera
        F32         mAlpha = 1.f;
    };

    // Size of a pixel, in meters, at this point
    F32 meters_per_pixel(const DrawContext& ctx, const LLVector3& pos_agent)
    {
        return llmax(dist_vec(pos_agent, ctx.mCameraOrigin), 0.01f) * ctx.mPixelScale;
    }

    void set_color(const DrawContext& ctx, const LLColor4& color)
    {
        gGL.color4f(color.mV[VRED], color.mV[VGREEN], color.mV[VBLUE], color.mV[VALPHA] * ctx.mAlpha);
    }

    // A camera facing ribbon from p0 to p1, a constant number of pixels wide
    void add_segment(const DrawContext& ctx, const LLVector3& p0, const LLVector3& p1, F32 half_width_px)
    {
        const LLVector3 dir = p1 - p0;
        if (dir.lengthSquared() < 1.e-10f)
        {
            return;
        }

        LLVector3 side0 = dir % (p0 - ctx.mCameraOrigin);
        LLVector3 side1 = dir % (p1 - ctx.mCameraOrigin);
        if (side0.normalize() == 0.f || side1.normalize() == 0.f)
        {
            return; // looking straight down the line
        }
        side0 *= half_width_px * meters_per_pixel(ctx, p0);
        side1 *= half_width_px * meters_per_pixel(ctx, p1);

        gGL.vertex3fv((p0 - side0).mV);
        gGL.vertex3fv((p0 + side0).mV);
        gGL.vertex3fv((p1 + side1).mV);

        gGL.vertex3fv((p0 - side0).mV);
        gGL.vertex3fv((p1 + side1).mV);
        gGL.vertex3fv((p1 - side1).mV);
    }

    // A camera facing diamond centered on the point
    void add_marker(const DrawContext& ctx, const LLVector3& pos, F32 half_size_px)
    {
        const F32 size = half_size_px * meters_per_pixel(ctx, pos);
        const LLVector3 left = ctx.mCameraLeft * size;
        const LLVector3 up = ctx.mCameraUp * size;

        gGL.vertex3fv((pos + up).mV);
        gGL.vertex3fv((pos + left).mV);
        gGL.vertex3fv((pos - up).mV);

        gGL.vertex3fv((pos - up).mV);
        gGL.vertex3fv((pos - left).mV);
        gGL.vertex3fv((pos + up).mV);
    }

    void draw_path(const DrawContext& ctx, const std::vector<FSFloaterMeasure::Point>& points, const std::vector<LLVector3>& pos_agent)
    {
        const size_t count = points.size();

        gGL.begin(LLRender::TRIANGLES);

        set_color(ctx, PATH_COLOR);
        for (size_t i = 1; i < count; ++i)
        {
            add_segment(ctx, pos_agent[i - 1], pos_agent[i], LINE_HALF_WIDTH_PX);
        }

        set_color(ctx, NORMAL_COLOR);
        for (size_t i = 0; i < count; ++i)
        {
            if (points[i].mHasNormal)
            {
                const LLVector3 tip = pos_agent[i] + points[i].mNormal * (NORMAL_LENGTH_PX * meters_per_pixel(ctx, pos_agent[i]));
                add_segment(ctx, pos_agent[i], tip, NORMAL_HALF_WIDTH_PX);
            }
        }

        for (size_t i = 0; i < count; ++i)
        {
            if (i == 0)
            {
                set_color(ctx, POINT_A_COLOR);
            }
            else if (i == count - 1)
            {
                set_color(ctx, POINT_B_COLOR);
            }
            else
            {
                set_color(ctx, POINT_MID_COLOR);
            }
            add_marker(ctx, pos_agent[i], MARKER_HALF_SIZE_PX);
        }

        gGL.end();
    }
}

FSToolMeasure::FSToolMeasure()
:   LLTool(std::string("Measure"))
{
}

FSToolMeasure::~FSToolMeasure()
{
}

bool FSToolMeasure::handleMouseDown(S32 x, S32 y, MASK mask)
{
    // Clicks on the UI never get here; this one is in-world. Transparent faces are skipped like a
    // normal click does, rigged meshes and objects that can't be selected can still be measured.
    gViewerWindow->pickAsync(x, y, mask, pickCallback, false, true, true);
    return true;
}

bool FSToolMeasure::handleMouseUp(S32 x, S32 y, MASK mask)
{
    return true;
}

bool FSToolMeasure::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    // Two quick clicks are still two points, and must not walk or teleport there
    return handleMouseDown(x, y, mask);
}

bool FSToolMeasure::handleRightMouseDown(S32 x, S32 y, MASK mask)
{
    stopPicking();
    return true;
}

bool FSToolMeasure::handleRightMouseUp(S32 x, S32 y, MASK mask)
{
    return true;
}

bool FSToolMeasure::handleHover(S32 x, S32 y, MASK mask)
{
    gViewerWindow->setCursor(UI_CURSOR_CROSS);
    return true;
}

bool FSToolMeasure::handleKey(KEY key, MASK mask)
{
    if (key == KEY_ESCAPE)
    {
        stopPicking();
        return true;
    }
    return false;
}

void FSToolMeasure::startPicking()
{
    LLToolMgr::getInstance()->setTransientTool(this);
}

void FSToolMeasure::stopPicking()
{
    // Only take back the mouse if no other transient tool has taken it since
    if (isPicking())
    {
        LLToolMgr::getInstance()->clearTransientTool();
    }
}

bool FSToolMeasure::isPicking()
{
    return LLToolMgr::instanceExists() && LLToolMgr::getInstance()->getCurrentTool() == this;
}

// static
void FSToolMeasure::pickCallback(const LLPickInfo& pick_info)
{
    FSFloaterMeasure* floater = LLFloaterReg::findTypedInstance<FSFloaterMeasure>("fs_measure");
    if (!floater)
    {
        return;
    }

    LLVector3d pos_global;
    LLVector3 normal;
    bool has_normal = false;

    switch (pick_info.mPickType)
    {
        case LLPickInfo::PICK_LAND:
        {
            // The pick lifted the point above the ground; put it back on the terrain surface
            pos_global = pick_info.mPosGlobal;
            pos_global.mdV[VZ] -= LAND_PICK_FUDGE;
            LLWorld* world = LLWorld::getInstance();
            if (world->getRegionFromPosGlobal(pos_global))
            {
                pos_global.mdV[VZ] = world->resolveLandHeightGlobal(pos_global);
                normal = world->resolveLandNormalGlobal(pos_global);
                has_normal = normal.normalize() > 0.f;
            }
            break;
        }

        case LLPickInfo::PICK_OBJECT:
        case LLPickInfo::PICK_FLORA:
        {
            if (pick_info.mPickHUD)
            {
                floater->onPickMissed(true);
                return;
            }
            // mPosGlobal is the ray hit itself; the surface pass (getSurfaceInfo()) adds the normal
            // in agent axes, left at zero when it couldn't hit the object again
            pos_global = pick_info.mPosGlobal;
            normal = pick_info.mNormal;
            has_normal = normal.isFinite() && normal.normalize() > 0.f;
            break;
        }

        default:
            floater->onPickMissed(false);
            return;
    }

    floater->addPoint(pos_global, has_normal ? normal : LLVector3::zero, has_normal);
}

// static
void FSToolMeasure::renderInWorld()
{
    if (LLPipeline::sReflectionRender)
    {
        return;
    }

    FSFloaterMeasure* floater = LLFloaterReg::findTypedInstance<FSFloaterMeasure>("fs_measure");
    if (!floater)
    {
        return;
    }

    const std::vector<FSFloaterMeasure::Point>& points = floater->getPoints();
    if (points.empty())
    {
        return;
    }

    LLViewerCamera* camera = LLViewerCamera::getInstance();
    const S32 height_px = camera->getViewHeightInPixels();
    if (height_px <= 0)
    {
        return;
    }

    DrawContext ctx;
    ctx.mCameraOrigin = camera->getOrigin();
    ctx.mCameraLeft = camera->getLeftAxis();
    ctx.mCameraUp = camera->getUpAxis();
    ctx.mPixelScale = 2.f * tanf(camera->getView() * 0.5f) / (F32)height_px;

    // Agent coordinates move on region crossings, so they are worked out from the global positions
    std::vector<LLVector3> pos_agent;
    pos_agent.reserve(points.size());
    for (const FSFloaterMeasure::Point& point : points)
    {
        pos_agent.push_back(gAgent.getPosAgentFromGlobal(point.mPosGlobal));
    }

    // Same state as LLTracker's beacon, drawn just before this in render_hud_elements()
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    LLGLDisable cull_face(GL_CULL_FACE);
    LLGLEnable blend(GL_BLEND);

    {
        // The parts behind other things, faint
        LLGLDepthTest depth(GL_TRUE, GL_FALSE, GL_GREATER);
        ctx.mAlpha = HIDDEN_ALPHA;
        draw_path(ctx, points, pos_agent);
    }
    {
        LLGLDepthTest depth(GL_TRUE, GL_FALSE, GL_LEQUAL);
        ctx.mAlpha = 1.f;
        draw_path(ctx, points, pos_agent);
    }

    gGL.color4f(1.f, 1.f, 1.f, 1.f);
}
