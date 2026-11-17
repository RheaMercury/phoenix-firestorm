/**
 * @file llfloaterscriptdebug.cpp
 * @brief Chat window for showing script errors and warnings
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

#include "llviewerprecompiledheaders.h"

#include "llfloaterscriptdebug.h"

#include "llfloaterreg.h"
#include "lluictrlfactory.h"
#include "llfontgl.h"
#include "llrect.h"
#include "llerror.h"
#include "llstring.h"
#include "llvoavatarself.h"
#include "message.h"
#include "lltrans.h"

// project include
#include "llviewertexteditor.h"
#include "llviewercontrol.h"
#include "llviewerobjectlist.h"
#include "llviewertexturelist.h"
// <FS:Trish> Script debug filter, timestamps and object actions
#include "fsfloaterscriptprofiler.h"
#include "llagent.h"
#include "llagentcamera.h"
#include "llfiltereditor.h"
#include "llselectmgr.h"
#include "llviewerobject.h"
#include "llviewerregion.h"

namespace
{
    // Each tab keeps at most this many lines for filtering. When it overflows,
    // a batch of the oldest lines is dropped at once so the text is rebuilt
    // rarely instead of on every new line.
    constexpr size_t SCRIPT_DEBUG_MAX_LINES = 2000;
    constexpr size_t SCRIPT_DEBUG_DROP_LINES = 200;
    // Distance and position change as the object or the avatar moves, so the
    // status line is refreshed on this interval while the tab is drawn.
    constexpr F32 SCRIPT_DEBUG_STATUS_INTERVAL = 0.5f;
}
// </FS:Trish>

//
// Statics
//

//
// Member Functions
//
LLFloaterScriptDebug::LLFloaterScriptDebug(const LLSD& key)
  : LLMultiFloater(key)
{
    // avoid resizing of the window to match
    // the initial size of the tabbed-childs, whenever a tab is opened or closed
    mAutoResize = false;
    // enabled autocous blocks controling focus via  LLFloaterReg::showInstance
    setAutoFocus(false);
}

LLFloaterScriptDebug::~LLFloaterScriptDebug()
{
}

void LLFloaterScriptDebug::show(const LLUUID& object_id)
{
    // <FS:Ansariel> Script debug icon
    //addOutputWindow(object_id);
    addOutputWindow(object_id, true);
    // </FS:Ansariel> Script debug icon
}

bool LLFloaterScriptDebug::postBuild()
{
    LLMultiFloater::postBuild();

    // <FS:Ansariel> Disconnect close signal. It would call closeAllFloaters()
    //               and the floaters in the tabs will be gone since LL
    //               messed with the floater instance handling!
    mCloseSignal.disconnect_all_slots();

    if (mTabContainer)
    {
        return true;
    }

    return false;
}

// <FS:Ansariel> Improved script debug floater
//void LLFloaterScriptDebug::setVisible(bool visible)
//{
//  if(visible)
//  {
//      LLFloaterScriptDebugOutput* floater_output = LLFloaterReg::findTypedInstance<LLFloaterScriptDebugOutput>("script_debug_output", LLUUID::null);
//      if (floater_output == NULL)
//      {
//          floater_output = dynamic_cast<LLFloaterScriptDebugOutput*>(LLFloaterReg::showInstance("script_debug_output", LLUUID::null, false));
//          if (floater_output)
//          {
//              addFloater(floater_output, false);
//          }
//      }

//  }
//  LLMultiFloater::setVisible(visible);
//}

//void LLFloaterScriptDebug::closeFloater(bool app_quitting/* = false*/)
//{
//  if(app_quitting)
//  {
//      LLMultiFloater::closeFloater(app_quitting);
//  }
//  else
//  {
//      setVisible(false);
//  }
//}
// </FS:Ansariel>

// <FS:Ansariel> Script debug icon
//LLFloater* LLFloaterScriptDebug::addOutputWindow(const LLUUID &object_id)
LLFloater* LLFloaterScriptDebug::addOutputWindow(const LLUUID& object_id, bool show /* = false */)
// </FS:Ansariel> Script debug icon
{
    LLMultiFloater* host = LLFloaterReg::showTypedInstance<LLMultiFloater>("script_debug", LLSD());
    if (!host)
        return NULL;

    LLFloater::setFloaterHost(host);
    // prevent stealing focus, see EXT-8040
    LLFloater* floaterp = LLFloaterReg::showInstance("script_debug_output", object_id, false);
    LLFloater::setFloaterHost(NULL);

    // <FS:Ansariel> Script debug icon
    if (gSavedSettings.getS32("ShowScriptErrorsLocation") == 0 && !show)
    {
        host->closeFloater();
    }
    // </FS:Ansariel> Script debug icon

    return floaterp;
}

// <FS:Kadah> [FSllOwnerSayToScriptDebugWindow] Show llOwnerSays in the script debug window instead of local chat
// void LLFloaterScriptDebug::addScriptLine(const std::string &utf8mesg, const std::string &user_name, const LLColor4& color, const LLUUID& source_id)
void LLFloaterScriptDebug::addScriptLine(const LLChat& chat)
{
    // <FS:Kadah> [FSllOwnerSayToScriptDebugWindow]
    // LLViewerObject* objectp = gObjectList.findObject(source_id);
    LLViewerObject* objectp = gObjectList.findObject(chat.mFromID);
    std::string floater_label;

    // Handle /me messages.
    // <FS:Kadah> [FSllOwnerSayToScriptDebugWindow]
    // std::string prefix = utf8mesg.substr(0, 4);
    // std::string message = (prefix == "/me " || prefix == "/me'") ? user_name + utf8mesg.substr(3) : utf8mesg;

    if (objectp)
    {
        // <FS:Kadah> [FSllOwnerSayToScriptDebugWindow]
        if (chat.mChatType == CHAT_TYPE_DEBUG_MSG)
        {
            if(objectp->isHUDAttachment())
            {
                if (isAgentAvatarValid())
                {
                    ((LLViewerObject*)gAgentAvatarp)->setIcon(LLViewerTextureManager::getFetchedTextureFromFile("script_error.j2c", FTT_LOCAL_FILE, true, LLGLTexture::BOOST_UI));
                    // <FS:Ansariel> Mark script error icons
                    ((LLViewerObject*)gAgentAvatarp)->getIcon()->setScriptError();
                }
            }
            else
            {
                objectp->setIcon(LLViewerTextureManager::getFetchedTextureFromFile("script_error.j2c", FTT_LOCAL_FILE, true, LLGLTexture::BOOST_UI));
                // <FS:Ansariel> Mark script error icons
                objectp->getIcon()->setScriptError();
            }
        }
        // </FS:Kadah> [FSllOwnerSayToScriptDebugWindow]
        floater_label = llformat("%s (%.0f, %.0f, %.0f)",
                        // <FS:Kadah> [FSllOwnerSayToScriptDebugWindow]
                        // user_name.c_str(),
                        chat.mFromName.c_str(),
                        objectp->getPositionRegion().mV[VX],
                        objectp->getPositionRegion().mV[VY],
                        objectp->getPositionRegion().mV[VZ]);
    }
    else
    {
        // <FS:Kadah> [FSllOwnerSayToScriptDebugWindow]
        // floater_label = user_name;
        floater_label = chat.mFromName;
    }

    addOutputWindow(LLUUID::null); // <FS:Ansariel> Improved script debug floater
    // <FS:Kadah> [FSllOwnerSayToScriptDebugWindow]
    // addOutputWindow(source_id);
    static LLCachedControl<U32> FSllOwnerSayRouting(gSavedSettings, "FSllOwnerSayToScriptDebugWindowRouting");

    // add to "All" floater
    if ((chat.mChatType == CHAT_TYPE_DEBUG_MSG) || (FSllOwnerSayRouting != 1))
    {
        LLFloaterScriptDebugOutput* floaterp = LLFloaterReg::findTypedInstance<LLFloaterScriptDebugOutput>("script_debug_output", LLUUID::null);
        if (floaterp)
        {
            floaterp->addLine(chat, chat.mFromName);
        }
    }

    // add to specific script instance floater
    if ((chat.mChatType == CHAT_TYPE_DEBUG_MSG) || (FSllOwnerSayRouting != 2))
    {
        addOutputWindow(chat.mFromID);
        LLFloaterScriptDebugOutput* floaterp = LLFloaterReg::findTypedInstance<LLFloaterScriptDebugOutput>("script_debug_output", chat.mFromID);
        if (floaterp)
        {
            floaterp->addLine(chat, floater_label);
        }
    }

#if 0
    // add to "All" floater
    LLFloaterScriptDebugOutput* floaterp =  LLFloaterReg::getTypedInstance<LLFloaterScriptDebugOutput>("script_debug_output", LLUUID::null);
    if (floaterp)
    {
        floaterp->addLine(message, user_name, color);
    }

    // add to specific script instance floater
    floaterp = LLFloaterReg::getTypedInstance<LLFloaterScriptDebugOutput>("script_debug_output", source_id);
    if (floaterp)
    {
        floaterp->addLine(message, floater_label, color);
    }
#endif
    // </FS:Kadah> [FSllOwnerSayToScriptDebugWindow]
}

// <FS:Kadah> [FSllOwnerSayToScriptDebugWindow]
// virtual
void LLFloaterScriptDebug::onClickCloseBtn(bool app_qutting)
{
    if (gSavedSettings.getBOOL("FSScriptDebugWindowClearOnClose"))
    {
        LLFloaterScriptDebugOutput* floaterp = LLFloaterReg::findTypedInstance<LLFloaterScriptDebugOutput>("script_debug_output", LLUUID::null);
        if (floaterp)
        {
            floaterp->clear();
        }
    }
    LLMultiFloater::onClickCloseBtn(app_qutting);
}
// </FS:Kadah> [FSllOwnerSayToScriptDebugWindow]

//
// LLFloaterScriptDebugOutput
//

LLFloaterScriptDebugOutput::LLFloaterScriptDebugOutput(const LLSD& object_id)
  : LLFloater(LLSD(object_id)),
    mObjectID(object_id.asUUID())
{
    // enabled autocous blocks controling focus via  LLFloaterReg::showInstance
    setAutoFocus(false);
}

bool LLFloaterScriptDebugOutput::postBuild()
{
    LLFloater::postBuild();
    mHistoryEditor = getChild<LLViewerTextEditor>("Chat History Editor");

    // <FS:Trish> Script debug filter, timestamps and object actions
    getChild<LLFilterEditor>("filter_input")->setCommitCallback([this](LLUICtrl*, const LLSD& value) { onFilterEdit(value.asString()); });

    // The "All" tab has no single object to act on, so it only gets the filter.
    const bool has_object = mObjectID.notNull();
    getChildView("look_at_btn")->setVisible(has_object);
    getChildView("select_btn")->setVisible(has_object);
    getChildView("profile_btn")->setVisible(has_object);
    getChild<LLUICtrl>("look_at_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickLookAt(); });
    getChild<LLUICtrl>("select_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickSelect(); });
    getChild<LLUICtrl>("profile_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickAddToProfiler(); });

    // Turning timestamps on or off should apply to the lines already shown too.
    if (LLControlVariable* control = gSavedSettings.getControl("FSScriptDebugTimestamps"))
    {
        mTimestampConnection = control->getSignal()->connect([this](LLControlVariable*, const LLSD&, const LLSD&) { rebuildText(); });
    }

    updateStatus();
    // </FS:Trish>
    return true;
}

LLFloaterScriptDebugOutput::~LLFloaterScriptDebugOutput()
{
}

// <FS:Kadah> [FSllOwnerSayToScriptDebugWindow]

#if 0
void LLFloaterScriptDebugOutput::addLine(const std::string &utf8mesg, const std::string &user_name, const LLColor4& color)
{
    if (mObjectID.isNull())
    {
        setCanTearOff(false);
        setCanClose(false);
    }
    else
    {
        setTitle(user_name);
        setShortTitle(user_name);
    }

    mHistoryEditor->appendText(utf8mesg, true, LLStyle::Params().color(color));
    mHistoryEditor->blockUndo();
}
#endif

void LLFloaterScriptDebugOutput::addLine(const LLChat& chat, const std::string &user_name)
{
    LLFontGL* fontp = LLViewerChat::getChatFont();
    std::string font_name = LLFontGL::nameFromFont(fontp);
    std::string font_size = LLFontGL::sizeFromFont(fontp);
    LLStyle::Params message_params;
    message_params.font.name(font_name);
    message_params.font.size(font_size);

    if (mObjectID.isNull())
    {
        setCanTearOff(false);
        setCanClose(false);
    }
    else
    {
        // Print object name slurl to output on first output or name change
        // <FS:Trish> The editor can be empty because of the filter, so ask the stored lines instead
        //if (mHistoryEditor->getText().empty() || mUserName.compare(user_name) != 0)
        if (mLines.empty() || mUserName.compare(user_name) != 0)
        // </FS:Trish>
        {
            mUserName = user_name;
            // <FS:Trish> The tab title also carries the error count
            //setTitle(user_name);
            //setShortTitle(user_name);
            updateTabTitle();
            // </FS:Trish>

            if (gSavedPerAccountSettings.getBOOL("FSllOwnerSayToScriptDebugWindow") && getKey().asUUID().isNull())
            {
                std::string url = chat.mURL;
                if ((url.empty()) || (std::string::npos == url.find("objectim")))
                {
                    url = LLViewerChat::getSenderSLURL(chat, LLSD());
                }
                LLStyle::Params link_params(message_params);
                link_params.readonly_color(LLUIColorTable::instance().getColor("ChatNameObjectColor"));
                link_params.is_link = true;
                link_params.link_href = url;
                mHistoryEditor->appendText(chat.mFromName, (!mHistoryEditor->getText().empty()), link_params);
            }
        }
    }

    // <FS:Trish> Script debug filter, timestamps and object actions
    // Lines are stored so the filter can rebuild the text; appendLine() draws them.
    //time_t utc_time;
    //utc_time = time_corrected();
    //std::string timeStr ="["+ LLTrans::getString("TimeHour")+"]:["+LLTrans::getString("TimeMin")+"]";
    //LLSD substitution;
    //substitution["datetime"] = (S32) utc_time;
    //LLStringUtil::format (timeStr, substitution);
    //
    //mHistoryEditor->appendText("[" + timeStr + "] ", (!mHistoryEditor->getText().empty()), message_params.readonly_color(LLUIColorTable::instance().getColor("ChatTimestampColor")));
    //
    //if (mObjectID.isNull())
    //{
    //    LLStyle::Params link_params(message_params);
    //    link_params.readonly_color(LLUIColorTable::instance().getColor("ChatNameObjectColor"));
    //    mHistoryEditor->appendText(chat.mFromName + ": ", false, link_params);
    //}
    //
    //if (chat.mChatType == CHAT_TYPE_DEBUG_MSG)
    //{
    //    mHistoryEditor->appendText(chat.mText, false, LLStyle::Params().readonly_color(LLUIColorTable::instance().getColor("ScriptErrorColor")));
    //}
    //else
    //{
    //    mHistoryEditor->appendText(chat.mText, false, message_params.readonly_color(LLUIColorTable::instance().getColor("llOwnerSayChatColor")));
    //}
    //mHistoryEditor->blockUndo();

    DebugLine line;
    line.mTime = time_corrected();
    line.mFromName = chat.mFromName;
    line.mText = chat.mText;
    // Debug channel messages are the ones that raise the script error icon;
    // owner says routed here are ordinary output and are not counted.
    line.mIsError = (chat.mChatType == CHAT_TYPE_DEBUG_MSG);
    line.mSearchText = line.mFromName + ": " + line.mText;
    LLStringUtil::toUpper(line.mSearchText);

    if (line.mIsError)
    {
        ++mErrorCount;
        updateTabTitle();
    }

    if (matchesFilter(line))
    {
        appendLine(line, mHistoryEditor->getLength() > 0);
    }
    mLines.push_back(std::move(line));

    if (mLines.size() > SCRIPT_DEBUG_MAX_LINES)
    {
        mLines.erase(mLines.begin(), mLines.begin() + SCRIPT_DEBUG_DROP_LINES);
        rebuildText();
    }

    updateStatus();
    // </FS:Trish>
}

void LLFloaterScriptDebugOutput::clear()
{
    mHistoryEditor->clear();
    // <FS:Trish> Script debug filter, timestamps and object actions
    mLines.clear();
    mErrorCount = 0;
    updateTabTitle();
    updateStatus();
    // </FS:Trish>
}

// <FS:Trish> Script debug filter, timestamps and object actions
void LLFloaterScriptDebugOutput::appendLine(const DebugLine& line, bool prepend_newline)
{
    LLFontGL* fontp = LLViewerChat::getChatFont();
    LLStyle::Params message_params;
    message_params.font.name(LLFontGL::nameFromFont(fontp));
    message_params.font.size(LLFontGL::sizeFromFont(fontp));

    static LLCachedControl<bool> show_timestamps(gSavedSettings, "FSScriptDebugTimestamps");
    if (show_timestamps)
    {
        // Local time with seconds, so a line can be matched to what the user
        // was doing or to the log of the external editor.
        std::string time_str = "[hour,datetime,local]:[min,datetime,local]:[second,datetime,local]";
        LLSD substitution;
        substitution["datetime"] = (S32)line.mTime;
        LLStringUtil::format(time_str, substitution);

        LLStyle::Params time_params(message_params);
        time_params.readonly_color(LLUIColorTable::instance().getColor("ChatTimestampColor"));
        mHistoryEditor->appendText("[" + time_str + "] ", prepend_newline, time_params);
        prepend_newline = false;
    }

    if (mObjectID.isNull())
    {
        LLStyle::Params link_params(message_params);
        link_params.readonly_color(LLUIColorTable::instance().getColor("ChatNameObjectColor"));
        mHistoryEditor->appendText(line.mFromName + ": ", prepend_newline, link_params);
        prepend_newline = false;
    }

    if (line.mIsError)
    {
        mHistoryEditor->appendText(line.mText, prepend_newline, LLStyle::Params().readonly_color(LLUIColorTable::instance().getColor("ScriptErrorColor")));
    }
    else
    {
        mHistoryEditor->appendText(line.mText, prepend_newline, message_params.readonly_color(LLUIColorTable::instance().getColor("llOwnerSayChatColor")));
    }
    mHistoryEditor->blockUndo();
}

void LLFloaterScriptDebugOutput::rebuildText()
{
    mHistoryEditor->clear();
    bool prepend_newline = false;
    for (const DebugLine& line : mLines)
    {
        if (matchesFilter(line))
        {
            appendLine(line, prepend_newline);
            prepend_newline = true;
        }
    }
}

bool LLFloaterScriptDebugOutput::matchesFilter(const DebugLine& line) const
{
    return mFilterUpper.empty() || line.mSearchText.find(mFilterUpper) != std::string::npos;
}

void LLFloaterScriptDebugOutput::onFilterEdit(const std::string& filter)
{
    std::string filter_upper = filter;
    LLStringUtil::trim(filter_upper);
    LLStringUtil::toUpper(filter_upper);
    if (filter_upper == mFilterUpper)
    {
        return;
    }
    mFilterUpper = filter_upper;
    rebuildText();
}

LLViewerObject* LLFloaterScriptDebugOutput::findObject() const
{
    if (mObjectID.isNull())
    {
        return nullptr;
    }
    LLViewerObject* objectp = gObjectList.findObject(mObjectID);
    return (objectp && !objectp->isDead()) ? objectp : nullptr;
}

void LLFloaterScriptDebugOutput::updateTabTitle()
{
    // The "All" tab keeps its fixed title from the XML.
    if (mObjectID.isNull())
    {
        return;
    }
    setTitle(mUserName);
    if (mErrorCount > 0)
    {
        LLStringUtil::format_map_t args;
        args["[NAME]"] = mUserName;
        args["[ERRORS]"] = llformat("%d", mErrorCount);
        setShortTitle(getString("tab_title_errors", args));
    }
    else
    {
        setShortTitle(mUserName);
    }
}

void LLFloaterScriptDebugOutput::updateStatus()
{
    mStatusTimer.reset();

    LLStringUtil::format_map_t args;
    args["[ERRORS]"] = llformat("%d", mErrorCount);
    args["[LINES]"] = llformat("%d", (S32)mLines.size());

    LLViewerObject* objectp = findObject();
    std::string status;
    if (mObjectID.isNull())
    {
        status = getString("status_all", args);
    }
    else if (!objectp)
    {
        status = getString("status_not_in_view", args);
    }
    else if (objectp->isHUDAttachment())
    {
        status = getString("status_hud", args);
    }
    else
    {
        const LLVector3d pos_global = objectp->getPositionGlobal();
        args["[DISTANCE]"] = llformat("%.1f", (F32)dist_vec(gAgent.getPositionGlobal(), pos_global));
        if (LLViewerRegion* regionp = objectp->getRegion())
        {
            const LLVector3 pos_region = regionp->getPosRegionFromGlobal(pos_global);
            args["[REGION]"] = regionp->getName();
            args["[POSITION]"] = llformat("%.0f, %.0f, %.0f", pos_region.mV[VX], pos_region.mV[VY], pos_region.mV[VZ]);
            status = getString("status_object", args);
        }
        else
        {
            status = getString("status_object_no_region", args);
        }
    }
    getChild<LLUICtrl>("status_text")->setValue(status);

    // The actions need the object in the viewer's object list. Looking at a
    // HUD object would point the camera at a meaningless spot in the world.
    const bool known = (objectp != nullptr);
    getChildView("look_at_btn")->setEnabled(known && !objectp->isHUDAttachment());
    getChildView("select_btn")->setEnabled(known);
    getChildView("profile_btn")->setEnabled(known);
}

// virtual
void LLFloaterScriptDebugOutput::draw()
{
    if (mStatusTimer.getElapsedTimeF32() > SCRIPT_DEBUG_STATUS_INTERVAL)
    {
        updateStatus();
    }
    LLFloater::draw();
}

void LLFloaterScriptDebugOutput::onClickLookAt()
{
    LLViewerObject* objectp = findObject();
    if (!objectp || objectp->isHUDAttachment())
    {
        return;
    }
    gAgentCamera.setFocusOnAvatar(false, ANIMATE);
    gAgentCamera.setFocusGlobal(objectp->getPositionGlobal(), mObjectID);
}

void LLFloaterScriptDebugOutput::onClickSelect()
{
    if (LLViewerObject* objectp = findObject())
    {
        LLSelectMgr::getInstance()->selectObjectAndFamily(objectp);
    }
}

void LLFloaterScriptDebugOutput::onClickAddToProfiler()
{
    // Messages can come from any prim of a linkset while the profiler reports
    // whole objects, so it is handed the root.
    if (LLViewerObject* objectp = findObject())
    {
        FSFloaterScriptProfiler::addTargets({ objectp->getRootEdit()->getID() });
    }
}
// </FS:Trish>
// </FS:Kadah> [FSllOwnerSayToScriptDebugWindow]
