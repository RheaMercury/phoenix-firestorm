/**
 * @file llfloaterscriptdebug.h
 * @brief Shows error and warning output from scripts
 *
 * $LicenseInfo:firstyear=2006&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#ifndef LL_LLFLOATERSCRIPTDEBUG_H
#define LL_LLFLOATERSCRIPTDEBUG_H

#include "llmultifloater.h"
#include "llviewerchat.h"
// <FS:Trish> Script debug filter, timestamps and object actions
#include "llframetimer.h"
#include <deque>
// </FS:Trish>

class LLTextEditor;
class LLUUID;
class LLViewerObject; // <FS:Trish/> Script debug object actions

class LLFloaterScriptDebug : public LLMultiFloater
{
public:
    LLFloaterScriptDebug(const LLSD& key);
    virtual ~LLFloaterScriptDebug();
    virtual bool postBuild();
    //virtual void setVisible(bool visible); // <FS:Ansariel> Improved script debug floater
    static void show(const LLUUID& object_id);

    /*virtual*/ //void closeFloater(bool app_quitting = false); // <FS:Ansariel> Improved script debug floater
    // <FS:Kadah> [FSllOwnerSayToScriptDebugWindow]
    // static void addScriptLine(const std::string &utf8mesg, const std::string &user_name, const LLColor4& color, const LLUUID& source_id);
    static void addScriptLine(const LLChat& chat);

protected:
    // <FS:Ansariel> Script debug icon
    //static LLFloater* addOutputWindow(const LLUUID& object_id);
    static LLFloater* addOutputWindow(const LLUUID& object_id, bool show = false);
    // </FS:Ansariel> Script debug icon

protected:
    static LLFloaterScriptDebug*    sInstance;

// <FS:Kadah> [FSllOwnerSayToScriptDebugWindow]
private:
    /*virtual*/ void onClickCloseBtn(bool app_qutting);
};

class LLFloaterScriptDebugOutput : public LLFloater
{
public:
    LLFloaterScriptDebugOutput(const LLSD& object_id);
    ~LLFloaterScriptDebugOutput();

    // <FS:Kadah> [FSllOwnerSayToScriptDebugWindow]
    // void addLine(const std::string &utf8mesg, const std::string &user_name, const LLColor4& color);
    void addLine(const LLChat& chat, const std::string &user_name);

    virtual bool postBuild();

    // <FS:Kadah> [FSllOwnerSayToScriptDebugWindow]
    void clear();

    // <FS:Trish> Script debug filter, timestamps and object actions
    /*virtual*/ void draw();
    // </FS:Trish>

protected:
    LLTextEditor* mHistoryEditor;

    LLUUID mObjectID;

    // <FS:Kadah> [FSllOwnerSayToScriptDebugWindow]
    std::string mUserName; //KC: This should be "object_name", but LL.

    // <FS:Trish> Script debug filter, timestamps and object actions
    // The output is one text editor, so the raw lines are kept here to be
    // able to rebuild the text when the filter or the timestamp setting changes.
    struct DebugLine
    {
        time_t      mTime{ 0 };
        std::string mFromName;
        std::string mText;
        std::string mSearchText; // upper case "name: text", matched against the filter
        bool        mIsError{ false };
    };

    void appendLine(const DebugLine& line, bool prepend_newline);
    void rebuildText();
    bool matchesFilter(const DebugLine& line) const;
    void onFilterEdit(const std::string& filter);
    void updateStatus();
    void updateTabTitle();
    LLViewerObject* findObject() const;
    void onClickLookAt();
    void onClickSelect();
    void onClickAddToProfiler();

    std::deque<DebugLine> mLines;
    std::string mFilterUpper;
    S32 mErrorCount{ 0 };
    LLFrameTimer mStatusTimer;
    boost::signals2::scoped_connection mTimestampConnection;
    // </FS:Trish>
};

#endif // LL_LLFLOATERSCRIPTDEBUG_H
