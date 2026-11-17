/**
 * @file fsfloatermeasure.cpp
 * @brief Measures the distance, offsets and surface angles between points picked in-world
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

#include "fsfloatermeasure.h"

#include "fstoolmeasure.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llclipboard.h"
#include "llfontgl.h"
#include "llhudobject.h"
#include "llhudtext.h"
#include "lltextbox.h"
#include "llviewerregion.h"
#include "llworld.h"

namespace
{
    // A chain is a path someone clicks out by hand; this only bounds what is kept and drawn
    constexpr size_t MAX_CHAIN_POINTS = 500;

    // Names of the value fields, in the order they are copied
    const char* const RESULT_FIELDS[] =
    {
        "point_a",
        "point_b",
        "distance",
        "horizontal",
        "delta_x",
        "delta_y",
        "delta_z",
        "height",
        "normal_angle",
        "tilt_a",
        "tilt_b",
        "path_total",
    };

    F32 tilt_from_vertical(const LLVector3& normal)
    {
        return acosf(llclamp(normal.mV[VZ], -1.f, 1.f)) * RAD_TO_DEG;
    }
}

FSFloaterMeasure::FSFloaterMeasure(const LLSD& key)
:   LLFloater(key)
{
}

FSFloaterMeasure::~FSFloaterMeasure()
{
    clearLabel();
    FSToolMeasure::sRenderInWorld = false;
}

bool FSFloaterMeasure::postBuild()
{
    mPickBtn = getChild<LLButton>("pick_btn");
    mPickBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickPick(); });

    mChainCheck = getChild<LLCheckBoxCtrl>("chain_check");
    mChainCheck->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCommitChain(); });

    getChild<LLButton>("reset_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickReset(); });
    getChild<LLButton>("copy_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickCopy(); });

    mStatusText = getChild<LLTextBox>("status");

    updateResults();
    updateStatus();
    return true;
}

void FSFloaterMeasure::onOpen(const LLSD& key)
{
    updateResults();
    updateRenderState();
}

void FSFloaterMeasure::onClose(bool app_quitting)
{
    if (!app_quitting && FSToolMeasure::instanceExists())
    {
        FSToolMeasure::getInstance()->stopPicking();
    }
    clearLabel();
    FSToolMeasure::sRenderInWorld = false;
}

void FSFloaterMeasure::draw()
{
    // Esc, a right click or another tool can end picking without this window knowing
    const bool picking = FSToolMeasure::getInstance()->isPicking();
    if (picking != mWasPicking)
    {
        mWasPicking = picking;
        updateStatus();
    }
    mPickBtn->setToggleState(picking);

    LLFloater::draw();
}

bool FSFloaterMeasure::handleKeyHere(KEY key, MASK mask)
{
    // With this window focused the key doesn't reach the tool
    if (key == KEY_ESCAPE && FSToolMeasure::getInstance()->isPicking())
    {
        FSToolMeasure::getInstance()->stopPicking();
        return true;
    }
    return LLFloater::handleKeyHere(key, mask);
}

void FSFloaterMeasure::addPoint(const LLVector3d& pos_global, const LLVector3& normal, bool has_normal)
{
    Point point;
    point.mPosGlobal = pos_global;
    point.mNormal = normal;
    point.mHasNormal = has_normal;
    if (LLViewerRegion* region = LLWorld::getInstance()->getRegionFromPosGlobal(pos_global))
    {
        point.mPosRegion = region->getPosRegionFromGlobal(pos_global);
        point.mRegionName = region->getName();
    }

    const bool chain = mChainCheck->get();
    if (!chain && mPoints.size() >= 2)
    {
        // A new measurement
        mPoints.clear();
    }
    if (chain && mPoints.size() >= MAX_CHAIN_POINTS)
    {
        // Drop the oldest leg from the path
        mPoints.erase(mPoints.begin());
    }

    mPoints.push_back(point);
    mStatusOverride.clear();

    if (!chain && mPoints.size() >= 2)
    {
        // Point B: done
        FSToolMeasure::getInstance()->stopPicking();
    }

    updateResults();
    updateStatus();
    updateRenderState();
}

void FSFloaterMeasure::onPickMissed(bool hud)
{
    mStatusOverride = getString(hud ? "status_hud" : "status_missed");
    updateStatus();
}

void FSFloaterMeasure::onClickPick()
{
    FSToolMeasure* tool = FSToolMeasure::getInstance();
    if (tool->isPicking())
    {
        tool->stopPicking();
        return;
    }

    if (!mChainCheck->get() && mPoints.size() >= 2)
    {
        // Picking again starts a new measurement
        mPoints.clear();
        updateResults();
        updateRenderState();
    }
    mStatusOverride.clear();
    tool->startPicking();
    mWasPicking = tool->isPicking();
    updateStatus();
}

void FSFloaterMeasure::onClickReset()
{
    mPoints.clear();
    mStatusOverride.clear();
    updateResults();
    updateStatus();
    updateRenderState();
}

void FSFloaterMeasure::onClickCopy()
{
    std::string summary;
    for (const char* field : RESULT_FIELDS)
    {
        LLTextBox* value = getChild<LLTextBox>(field);
        if (!value->getVisible())
        {
            continue;
        }
        const std::string label_name = std::string(field) + "_label";
        summary += getChild<LLTextBox>(label_name)->getText() + " " + value->getText() + "\n";
    }

    const LLWString text = utf8str_to_wstring(summary);
    LLClipboard::instance().copyToClipboard(text, 0, static_cast<S32>(text.size()));
}

void FSFloaterMeasure::onCommitChain()
{
    if (!mChainCheck->get() && mPoints.size() > 2)
    {
        // Back to a single measurement: keep the last leg
        mPoints.erase(mPoints.begin(), mPoints.end() - 2);
    }
    updateResults();
    updateStatus();
    updateRenderState();
}

void FSFloaterMeasure::updateResults()
{
    const std::string none = getString("none");
    const size_t count = mPoints.size();
    const bool chain = mChainCheck->get();

    // The path is recounted from the points kept, so dropping or trimming them keeps it right
    mPathLength = 0.0;
    for (size_t i = 1; i < count; ++i)
    {
        mPathLength += (mPoints[i].mPosGlobal - mPoints[i - 1].mPosGlobal).magVec();
    }

    getChild<LLTextBox>("path_total_label")->setVisible(chain);
    getChild<LLTextBox>("path_total")->setVisible(chain);

    if (count < 2)
    {
        for (const char* field : RESULT_FIELDS)
        {
            setResult(field, none);
        }
        if (count == 1)
        {
            setResult("point_a", formatPoint(mPoints[0]));
            setResult("tilt_a", mPoints[0].mHasNormal ? formatDegrees(tilt_from_vertical(mPoints[0].mNormal)) : getString("no_normal"));
        }
        clearLabel();
        return;
    }

    // In a chain, A is the previous point and B the last one
    const Point& a = mPoints[count - 2];
    setResult("point_a", formatPoint(a));

    const Point& b = mPoints[count - 1];
    const LLVector3d delta = b.mPosGlobal - a.mPosGlobal;
    const F64 distance = delta.magVec();
    const F64 horizontal = sqrt(delta.mdV[VX] * delta.mdV[VX] + delta.mdV[VY] * delta.mdV[VY]);

    setResult("point_b", formatPoint(b));
    setResult("distance", formatMeters(distance));
    setResult("horizontal", formatMeters(horizontal));
    setResult("delta_x", formatMeters(delta.mdV[VX]));
    setResult("delta_y", formatMeters(delta.mdV[VY]));
    setResult("delta_z", formatMeters(delta.mdV[VZ]));

    LLStringUtil::format_map_t height_args;
    height_args["[VALUE]"] = formatMeters(fabs(delta.mdV[VZ]));
    const char* height_string = delta.mdV[VZ] > 0.0005 ? "height_b_higher" : (delta.mdV[VZ] < -0.0005 ? "height_b_lower" : "height_same");
    setResult("height", getString(height_string, height_args));

    if (a.mHasNormal && b.mHasNormal)
    {
        setResult("normal_angle", formatDegrees(angle_between(a.mNormal, b.mNormal) * RAD_TO_DEG));
    }
    else
    {
        setResult("normal_angle", getString("no_normal"));
    }
    setResult("tilt_a", a.mHasNormal ? formatDegrees(tilt_from_vertical(a.mNormal)) : getString("no_normal"));
    setResult("tilt_b", b.mHasNormal ? formatDegrees(tilt_from_vertical(b.mNormal)) : getString("no_normal"));
    setResult("path_total", formatMeters(mPathLength));

    // The length at the middle of the last leg, in-world
    if (mLabel.isNull())
    {
        mLabel = (LLHUDText*)LLHUDObject::addHUDObject(LLHUDObject::LL_HUD_TEXT);
        mLabel->setFont(LLFontGL::getFontSansSerif());
        mLabel->setZCompare(false);
        mLabel->setDoFade(false);
        mLabel->setColor(LLColor4(1.f, 0.9f, 0.4f, 1.f));
        mLabel->setVertAlignment(LLHUDText::ALIGN_VERT_CENTER);
    }
    std::string label = formatMeters(distance);
    if (chain && count > 2)
    {
        LLStringUtil::format_map_t total_args;
        total_args["[VALUE]"] = formatMeters(mPathLength);
        label += "\n" + getString("label_total", total_args);
    }
    mLabel->setString(label);
    mLabel->setPositionGlobal((a.mPosGlobal + b.mPosGlobal) * 0.5);
}

void FSFloaterMeasure::updateStatus()
{
    if (!mStatusText)
    {
        return;
    }

    std::string status;
    if (!mStatusOverride.empty())
    {
        status = mStatusOverride;
    }
    else if (FSToolMeasure::getInstance()->isPicking())
    {
        if (mPoints.empty())
        {
            status = getString("status_pick_a");
        }
        else if (mChainCheck->get())
        {
            status = getString("status_pick_next");
        }
        else
        {
            status = getString("status_pick_b");
        }
    }
    else
    {
        status = getString("status_idle");
    }
    mStatusText->setText(status);
}

void FSFloaterMeasure::updateRenderState()
{
    // Only called while the window is open; onClose() clears it
    FSToolMeasure::sRenderInWorld = !mPoints.empty();
}

void FSFloaterMeasure::clearLabel()
{
    if (mLabel.notNull())
    {
        mLabel->markDead();
        mLabel = nullptr;
    }
}

std::string FSFloaterMeasure::formatPoint(const Point& point) const
{
    LLStringUtil::format_map_t args;
    args["[REGION]"] = point.mRegionName.empty() ? getString("unknown_region") : point.mRegionName;
    args["[X]"] = llformat("%.3f", point.mPosRegion.mV[VX]);
    args["[Y]"] = llformat("%.3f", point.mPosRegion.mV[VY]);
    args["[Z]"] = llformat("%.3f", point.mPosRegion.mV[VZ]);
    return getString("point_format", args);
}

std::string FSFloaterMeasure::formatMeters(F64 meters) const
{
    LLStringUtil::format_map_t args;
    args["[VALUE]"] = llformat("%.3f", meters);
    return getString("meters_format", args);
}

std::string FSFloaterMeasure::formatDegrees(F32 degrees) const
{
    LLStringUtil::format_map_t args;
    args["[VALUE]"] = llformat("%.1f", degrees);
    return getString("degrees_format", args);
}

void FSFloaterMeasure::setResult(const std::string& name, const std::string& text)
{
    getChild<LLTextBox>(name)->setText(text);
}
