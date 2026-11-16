/**
 * @file llhudnametag.h
 * @brief Name tags for avatars
 * @author James Cook
 *
 * $LicenseInfo:firstyear=2010&license=viewerlgpl$
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

#ifndef LLHUDNAMETAG_H
#define LLHUDNAMETAG_H

#include "llpointer.h"

#include "llhudobject.h"
#include "v4color.h"
//#include "v4coloru.h"
#include "v2math.h"
#include "llrect.h"
//#include "llframetimer.h"
#include "llfontgl.h"
#include "llfontvertexbuffer.h"
#include <set>
#include <vector>

class LLHUDNameTag;
class LLUIImage;

struct llhudnametag_further_away
{
    bool operator()(const LLPointer<LLHUDNameTag>& lhs, const LLPointer<LLHUDNameTag>& rhs) const;
};

class LLHUDNameTag : public LLHUDObject
{
protected:
    class LLHUDTextSegment
    {
    public:
        LLHUDTextSegment(const LLWString& text, const LLFontGL::StyleFlags style, const LLColor4& color, const LLFontGL* font)
        :   mColor(color),
            mStyle(style),
            mText(text),
            mFont(font)
        {}
        F32 getWidth(const LLFontGL* font);
        const LLWString& getText() const { return mText; }
        void clearFontWidthMap() { mFontWidthMap.clear(); }

        LLColor4                mColor;
        LLFontGL::StyleFlags    mStyle;
        const LLFontGL*         mFont;
    private:
        LLWString               mText;
        std::map<const LLFontGL*, F32> mFontWidthMap;
    };

public:
    typedef enum e_text_alignment
    {
        ALIGN_TEXT_LEFT,
        ALIGN_TEXT_CENTER
    } ETextAlignment;

    typedef enum e_vert_alignment
    {
        ALIGN_VERT_TOP,
        ALIGN_VERT_CENTER
    } EVertAlignment;

    static const F32 NAMETAG_MAX_WIDTH; // 298px, made to fit 31 M's
    static const F32 HUD_TEXT_MAX_WIDTH; // 190px

public:
    // Set entire string, eliminating existing lines
    void setString(const std::string& text_utf8);

    void clearString();

    // Add text a line at a time, allowing custom formatting
    void addLine(
        const std::string &text_utf8,
        const LLColor4& color,
        const LLFontGL::StyleFlags style = LLFontGL::NORMAL,
        const LLFontGL* font = NULL,
        const bool use_ellipses = false,
        F32 max_pixels = HUD_TEXT_MAX_WIDTH);

    // For bubble chat, set the part above the chat text
    void setLabel(const std::string& label_utf8);
    void addLabel(const std::string& label_utf8, F32 max_pixels = HUD_TEXT_MAX_WIDTH);

    // Sets the default font for lines with no font specified
    void setFont(const LLFontGL* font);
    void setColor(const LLColor4 &color);
    void setAlpha(F32 alpha);
    void setZCompare(const bool zcompare);
    void setDoFade(const bool do_fade);
    void setVisibleOffScreen(bool visible) { mVisibleOffScreen = visible; }

    // mMaxLines of -1 means unlimited lines.
    void setMaxLines(S32 max_lines) { mMaxLines = max_lines; }
    void setFadeDistance(F32 fade_distance, F32 fade_range) { mFadeDistance = fade_distance; mFadeRange = fade_range; }
    void updateVisibility();
    LLVector2 updateScreenPos(LLVector2 &offset_target);
    void updateSize();
//  void setMass(F32 mass) { mMass = llmax(0.1f, mass); }
    void setTextAlignment(ETextAlignment alignment) { mTextAlignment = alignment; }
    void setVertAlignment(EVertAlignment alignment) { mVertAlignment = alignment; }
    /*virtual*/ void markDead();
    friend class LLHUDObject;
    /*virtual*/ F32 getDistance() const { return mLastDistance; }
    S32  getLOD() const { return mLOD; }
    bool getVisible() const { return mVisible; }
    bool getHidden() const { return mHidden; }
    void setHidden( bool hide ) { mHidden = hide; }
    void shift(const LLVector3& offset);
    F32 getWorldHeight() const;

    bool lineSegmentIntersect(const LLVector4a& start, const LLVector4a& end, LLVector4a& intersection, bool debug_render = false);

    static void shiftAll(const LLVector3& offset);
    static void addPickable(std::set<LLViewerObject*> &pick_list);
    static void reshape();
    static void setDisplayText(bool flag) { sDisplayText = flag ; }

    // <FS:Trish> Pill style name tags (FSNameTagPillStyle), set by the avatar at each tag refresh:
    // whose profile picture to show (FSNameTagProfilePictures) and the colour of the ring around it,
    // which tells the chat range they are in (none beyond shout range or for yourself)
    void setFSPill(const LLUUID& avatar_id, bool has_ring, const LLColor4& ring_color);
    // </FS:Trish>

protected:
    LLHUDNameTag(const U8 type);

    /*virtual*/ void render();
    void renderText();
    static void updateAll();
    void setLOD(S32 lod);
    S32 getMaxLines();

private:
    ~LLHUDNameTag();
    bool            mDoFade;
    F32             mFadeRange;
    F32             mFadeDistance;
    F32             mLastDistance;
    bool            mZCompare;
    bool            mVisibleOffScreen;
    bool            mOffscreen;
    LLColor4        mColor;
//  LLVector3       mScale;
    F32             mWidth;
    F32             mHeight;
//  LLColor4U       mPickColor;
    const LLFontGL* mFontp;
    const LLFontGL* mBoldFontp;
    LLRectf         mSoftScreenRect;
    LLVector3       mPositionAgent;
    LLVector2       mPositionOffset;
    LLVector2       mTargetPositionOffset;
    F32             mMass;
    S32             mMaxLines;
    S32             mOffsetY;
    F32             mRadius;
    std::vector<LLHUDTextSegment> mTextSegments;
    std::vector<LLHUDTextSegment> mLabelSegments;
//  LLFrameTimer    mResizeTimer;
    ETextAlignment  mTextAlignment;
    EVertAlignment  mVertAlignment;
    S32             mLOD;
    bool            mHidden;
    LLPointer<LLUIImage> mRoundedRectImgp;
    LLPointer<LLUIImage> mRoundedRectTopImgp;

    // <FS:Trish> Pill style, see setFSPill
    bool fsPillMode() const;
    void fsRenderPill(const LLVector3& render_position, const LLVector3& x_pixel_vec, const LLVector3& y_pixel_vec,
                      const LLColor4& bg_color, F32 alpha_factor);
    bool            mFSPill = false;            // an avatar tag that may be drawn as a pill
    LLUUID          mFSAvatarID;
    bool            mFSHasRing = false;
    LLColor4        mFSRingColor;
    bool            mFSPortrait = false;        // updateSize made room for the profile picture
    F32             mFSScale = 1.f;             // updateSize: smaller with distance, mWidth and mHeight include it
    // updateSize: how far the tag has shrunk into its picture (0 the whole tag, 1 only the picture), which it does
    // farther than FSNameTagPictureOnlyDistance, and the sizes it goes between, in the tag's own pixels
    F32             mFSShrink = 0.f;
    F64             mFSShrinkTime = 0.0;        // when mFSShrink last moved
    F32             mFSFullWidth = 0.f;
    F32             mFSFullHeight = 0.f;
    F32             mFSSoloSize = 0.f;          // the picture's diameter on its own
    // </FS:Trish>

    static bool    sDisplayText ;
    static std::set<LLPointer<LLHUDNameTag> > sTextObjects;
    static std::vector<LLPointer<LLHUDNameTag> > sVisibleTextObjects;
//  static std::vector<LLPointer<LLHUDNameTag> > sVisibleHUDTextObjects;
    typedef std::set<LLPointer<LLHUDNameTag> >::iterator TextObjectIterator;
    typedef std::vector<LLPointer<LLHUDNameTag> >::iterator VisibleTextObjectIterator;
};

#endif
