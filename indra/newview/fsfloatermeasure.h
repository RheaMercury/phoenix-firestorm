/**
 * @file fsfloatermeasure.h
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

#ifndef FS_FLOATERMEASURE_H
#define FS_FLOATERMEASURE_H

#include "llfloater.h"
#include "llpointer.h"
#include "v3dmath.h"
#include "v3math.h"

#include <vector>

class LLButton;
class LLCheckBoxCtrl;
class LLHUDText;
class LLTextBox;

// <FS:Trish> Measuring tool. "Pick points" hands the mouse to FSToolMeasure; the first click on a
// surface (object, avatar or ground) is point A, the next one point B. The window shows the
// distance A-B, its horizontal part, the offsets along the region axes, the height difference and,
// when the picks gave surface normals, the angle between them and each one's tilt from vertical.
// With "Chain", every further click measures on from the previous point and a running total of the
// path is kept. While the window is open the path is drawn in-world with its length at the middle
// of the last segment.
class FSFloaterMeasure : public LLFloater
{
public:
    struct Point
    {
        LLVector3d  mPosGlobal;
        LLVector3   mPosRegion;     // region-local position at pick time
        std::string mRegionName;    // empty when the point was in no known region
        LLVector3   mNormal;        // unit surface normal, region (= global) axes
        bool        mHasNormal = false;
    };

    FSFloaterMeasure(const LLSD& key);
    ~FSFloaterMeasure();

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;
    void draw() override;
    bool handleKeyHere(KEY key, MASK mask) override;

    // From FSToolMeasure's pick callback
    void addPoint(const LLVector3d& pos_global, const LLVector3& normal, bool has_normal);
    void onPickMissed(bool hud);

    const std::vector<Point>& getPoints() const { return mPoints; }

private:
    void onClickPick();
    void onClickReset();
    void onClickCopy();
    void onCommitChain();

    void updateResults();
    void updateStatus();
    void updateRenderState();
    void clearLabel();

    std::string formatPoint(const Point& point) const;
    std::string formatMeters(F64 meters) const;
    std::string formatDegrees(F32 degrees) const;
    void setResult(const std::string& name, const std::string& text);

    std::vector<Point>      mPoints;
    F64                     mPathLength = 0.0;      // chain mode: sum of all segments

    LLButton*               mPickBtn = nullptr;
    LLCheckBoxCtrl*         mChainCheck = nullptr;
    LLTextBox*              mStatusText = nullptr;

    LLPointer<LLHUDText>    mLabel;                 // length at the middle of the last segment
    bool                    mWasPicking = false;
    std::string             mStatusOverride;        // a one-off message (missed pick), until the next pick
};
// </FS:Trish>

#endif // FS_FLOATERMEASURE_H
