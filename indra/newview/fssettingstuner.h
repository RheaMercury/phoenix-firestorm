/**
 * @file fssettingstuner.h
 * @brief "Suggest settings": measures where the frame time goes and suggests settings to change
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

#ifndef FS_SETTINGSTUNER_H
#define FS_SETTINGSTUNER_H

#include "llfloater.h"

#include <map>
#include <string>
#include <vector>

class LLButton;
class LLScrollListCtrl;
class LLTextBox;

// The measurement, run only while the tuner window asks for it. For about 20 seconds it sums the GPU
// time of each profiler GPU zone (FSGPUTimers capture, timing turned on for it if it was off), the main
// thread's own and inclusive time of each block timer (fed from fs_log_profile), the frame time and the
// time the frame limiter slept, and samples the texture memory state and the scene. Outside a
// measurement every hook is one test of a bool.
namespace FSSettingsTuner
{
    extern bool sMeasuring;

    // Main thread, once a frame, before FSGPUTimers::frame (LLAppViewer::doFrame)
    void frame();
    // fs_log_profile, once a frame while measuring: true when the block timers should be passed on now
    bool wantCPUZones();
    // ... then each timer with time this frame: own and inclusive milliseconds
    void addCPUZone(const void* key, const std::string& name, F64 own_ms, F64 total_ms);
    // The frame limiter slept this long (LLAppViewer::doFrame)
    inline void addCapSleep(S32 ms);
    void addCapSleepMs(S32 ms);
    // The shaders are being loaded (LLViewerShaderMgr::setShaders): a measurement running starts over
    inline void noteShaderReload();
    void restartForShaderReload();
}

inline void FSSettingsTuner::noteShaderReload()
{
    if (sMeasuring)
    {
        restartForShaderReload();
    }
}

inline void FSSettingsTuner::addCapSleep(S32 ms)
{
    if (sMeasuring)
    {
        addCapSleepMs(ms);
    }
}

class FSFloaterSettingsTuner : public LLFloater
{
public:
    FSFloaterSettingsTuner(const LLSD& key);
    ~FSFloaterSettingsTuner() override;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;
    void draw() override;

private:
    struct Suggestion
    {
        std::string mRuleId;
        std::string mChangeText;    // "Setting: old -> new, ..."
        std::string mLooks;
        std::string mDescription;
        std::string mEffect;        // the "saves" column when no saving is estimated
        F64 mSavingMs = 0.0;        // what the frame gains
        F64 mCPUSavingMs = 0.0;     // the main thread's part (cpu zones)
        F64 mGPUSavingMs = 0.0;     // the graphics card's part (gpu zones)
        std::vector<std::pair<std::string, LLSD>> mChanges;  // setting, new value
    };

    void startMeasure();
    void onMeasured();
    void evaluate();
    void fillList();
    std::string savesText(const Suggestion& s) const;
    void onApply();
    void onUndo();
    void rememberBaseline();
    void keepInPreferences(const std::vector<std::string>& names);
    void onCopyReport();
    void onSelect();
    void updateButtons();

    LLScrollListCtrl* mList = nullptr;
    LLTextBox* mSummary = nullptr;
    LLTextBox* mDetails = nullptr;
    LLButton* mMeasureBtn = nullptr;
    LLButton* mApplyBtn = nullptr;
    LLButton* mUndoBtn = nullptr;
    LLButton* mCopyBtn = nullptr;

    bool mHaveMeasurement = false;
    std::string mSummaryText;
    // The countdown shown while measuring, so the text is set again only when it changes
    S32 mShownSeconds = -1;
    U32 mShownRestarts = 0;

    // The measurement before the first Apply or Undo since it, so the next measurement can say
    // whether the change helped
    bool mHaveBaseline = false;
    F64 mBaseFrameMs = 0.0;
    F64 mBaseCPUMs = 0.0;
    F64 mBaseGPUMs = 0.0;
    bool mBaseGPUTimed = false;
    std::string mBaseChanges;
    std::string mCompareText;   // "After <changes>: was X ms a frame, now Y ms ...", for the last measurement
    std::vector<Suggestion> mSuggestions;
};

#endif // FS_SETTINGSTUNER_H
