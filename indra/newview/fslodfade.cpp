/**
 * @file fslodfade.cpp
 * @brief Cross-dissolve of object detail changes (FSRenderFadeLOD)
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

#include "fslodfade.h"

#include "llappviewer.h"
#include "lldrawable.h"
#include "lldrawpool.h"
#include "llface.h"
#include "llspatialpartition.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llvovolume.h"
#include "pipeline.h"

#include <unordered_map>
#include <utility>
#include <vector>

extern bool gCubeSnapshot;
extern bool gTeleportDisplay;

namespace
{
    // what is kept of one object's old shape
    struct Kept
    {
        LLPointer<LLSpatialGroup> mGroup;   // the group whose draw map held the batches
        LLPointer<LLDrawable> mDrawable;    // their model matrix lives in the drawable (or its parent)
        LLPointer<LLDrawable> mParent;
        std::vector<std::pair<U32, LLPointer<LLDrawInfo> > > mInfos;
        F32 mTime = 0.f;                    // committed (pending), or fading out since (fading)
    };

    // Allocated once and never freed: no destructors of draw infos or vertex buffers at exit, after
    // GL is gone. clear() empties them while GL is still up (LLPipeline::destroyGL).
    std::vector<Kept>& pending()
    {
        static std::vector<Kept>* s = new std::vector<Kept>();
        return *s;
    }

    std::vector<Kept>& fading()
    {
        static std::vector<Kept>* s = new std::vector<Kept>();
        return *s;
    }

    Kept& scratch()
    {
        static Kept* s = new Kept();
        return *s;
    }

    std::unordered_map<const LLDrawable*, F32>& fade_in()
    {
        static std::unordered_map<const LLDrawable*, F32>* s = new std::unordered_map<const LLDrawable*, F32>();
        return *s;
    }

    constexpr F32 PENDING_TIMEOUT = 2.f;    // seconds a capture waits for its group's rebuild
    constexpr size_t MAX_KEPT = 512;        // objects at most in either list

    F32 duration()
    {
        static LLCachedControl<F32> fade_time(gSavedSettings, "FSRenderFadeInTime", 0.6f);
        return (F32)fade_time;
    }

    bool alive(Kept& k)
    {
        return k.mGroup.notNull() && !k.mGroup->isDead() && k.mDrawable.notNull() && !k.mDrawable->isDead() &&
               (k.mParent.isNull() || !k.mParent->isDead());
    }

    void clear_kept(Kept& k)
    {
        k.mInfos.clear();
        k.mGroup = nullptr;
        k.mDrawable = nullptr;
        k.mParent = nullptr;
    }
}

bool FSLODFade::active()
{
    static LLCachedControl<bool> fade_in_setting(gSavedSettings, "FSRenderFadeIn", false);
    static LLCachedControl<bool> fade_lod(gSavedSettings, "FSRenderFadeLOD", false);
    return fade_in_setting && fade_lod && duration() > 0.f;
}

bool FSLODFade::fadesPass(U32 type)
{
    // the passes drawn from the render map by programs that have the dissolve (add_fade_in)
    switch (type)
    {
        case LLRenderPass::PASS_SIMPLE:
        case LLRenderPass::PASS_FULLBRIGHT:
        case LLRenderPass::PASS_FULLBRIGHT_SHINY:
        case LLRenderPass::PASS_BUMP:
        case LLRenderPass::PASS_ALPHA_MASK:
        case LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK:
        case LLRenderPass::PASS_MATERIAL:
        case LLRenderPass::PASS_MATERIAL_ALPHA_MASK:
        case LLRenderPass::PASS_MATERIAL_ALPHA_EMISSIVE:
        case LLRenderPass::PASS_SPECMAP:
        case LLRenderPass::PASS_SPECMAP_MASK:
        case LLRenderPass::PASS_SPECMAP_EMISSIVE:
        case LLRenderPass::PASS_NORMMAP:
        case LLRenderPass::PASS_NORMMAP_MASK:
        case LLRenderPass::PASS_NORMMAP_EMISSIVE:
        case LLRenderPass::PASS_NORMSPEC:
        case LLRenderPass::PASS_NORMSPEC_MASK:
        case LLRenderPass::PASS_NORMSPEC_EMISSIVE:
        case LLRenderPass::PASS_GLTF_PBR:
        case LLRenderPass::PASS_GLTF_PBR_ALPHA_MASK:
            return true;
        default:
            return false;
    }
}

void FSLODFade::capture(LLDrawable* drawable)
{
    Kept& k = scratch();
    clear_kept(k);
    if (!active() || !drawable || drawable->isDead() || gCubeSnapshot)
    {
        return;
    }
    const LLViewerObject* obj = drawable->getVObj();
    if (!obj || obj->isAttachment() || obj->isHUDAttachment() || obj->isAnimatedObject() ||
        drawable->isState(LLDrawable::RIGGED))
    {
        return;
    }
    LLSpatialGroup* group = drawable->getSpatialGroup();
    if (!group || group->isDead() || group->mDrawMap.empty())
    {
        return;
    }

    // the batches holding the object's faces: same vertex buffer, overlapping index range
    for (S32 i = 0; i < drawable->getNumFaces(); ++i)
    {
        const LLFace* face = drawable->getFace(i);
        const LLVertexBuffer* vb = face ? face->getVertexBuffer() : nullptr;
        if (!vb || !face->getIndicesCount())
        {
            continue;
        }
        const U32 start = (U32)face->getIndicesStart();
        const U32 end = start + face->getIndicesCount();
        for (auto& entry : group->mDrawMap)
        {
            if (!fadesPass(entry.first))
            {
                continue;
            }
            for (const LLPointer<LLDrawInfo>& info : entry.second)
            {
                if (info.isNull() || info->mVertexBuffer.get() != vb || info->mFSFadeOut ||
                    info->mOffset >= end || start >= info->mOffset + info->mCount)
                {
                    continue;
                }
                bool known = false;
                for (const auto& kept : k.mInfos)
                {
                    known |= kept.second == info;
                }
                if (!known)
                {
                    k.mInfos.emplace_back(entry.first, info);
                }
            }
        }
    }
    if (!k.mInfos.empty())
    {
        k.mGroup = group;
        k.mDrawable = drawable;
        k.mParent = drawable->getParent();
    }
}

void FSLODFade::commit(LLDrawable* drawable, bool changed)
{
    Kept& k = scratch();
    if (changed && !k.mInfos.empty() && k.mDrawable.get() == drawable)
    {
        std::vector<Kept>& list = pending();
        // a capture still waiting for the same object is older: this one replaces it
        for (auto it = list.begin(); it != list.end(); ++it)
        {
            if (it->mDrawable.get() == drawable)
            {
                list.erase(it);
                break;
            }
        }
        if (list.size() >= MAX_KEPT)
        {
            list.erase(list.begin());
        }
        k.mTime = (F32)gFrameTimeSeconds;
        list.push_back(std::move(k));
    }
    clear_kept(scratch());
}

void FSLODFade::onClearDrawMap(LLSpatialGroup* group)
{
    std::vector<Kept>& list = pending();
    if (list.empty())
    {
        return;
    }
    const F32 now = (F32)gFrameTimeSeconds;
    std::vector<Kept>& out = fading();
    for (size_t i = 0; i < list.size();)
    {
        Kept& k = list[i];
        if (now - k.mTime > PENDING_TIMEOUT || !alive(k))
        {
            list.erase(list.begin() + i);
            continue;
        }
        if (k.mGroup.get() != group)
        {
            ++i;
            continue;
        }
        // an object already fading out of an earlier change: that older shape goes now
        for (auto it = out.begin(); it != out.end(); ++it)
        {
            if (it->mDrawable == k.mDrawable)
            {
                out.erase(it);
                break;
            }
        }
        for (auto& entry : k.mInfos)
        {
            entry.second->mFSFadeStart = now;
            entry.second->mFSFadeOut = true;
        }
        fade_in()[k.mDrawable.get()] = now;
        k.mTime = now;
        if (out.size() >= MAX_KEPT)
        {
            out.erase(out.begin());
        }
        out.push_back(std::move(k));
        list.erase(list.begin() + i);
    }
}

F32 FSLODFade::fadeInStart(const LLDrawable* drawable, U32 type)
{
    std::unordered_map<const LLDrawable*, F32>& map = fade_in();
    if (map.empty() || !fadesPass(type))
    {
        return -1.f;
    }
    auto it = map.find(drawable);
    if (it == map.end())
    {
        return -1.f;
    }
    if ((F32)gFrameTimeSeconds - it->second >= duration())
    {
        map.erase(it);
        return -1.f;
    }
    return it->second;
}

void FSLODFade::pushFadingOut(LLCullResult* cull)
{
    std::vector<Kept>& list = fading();
    if (list.empty() && fade_in().empty())
    {
        return;
    }
    if (!active() || gTeleportDisplay)
    {
        clear();
        return;
    }
    // the main view only: shadows, probes, mirrors, water reflections and impostors draw the new shape
    if (!cull || gCubeSnapshot || LLPipeline::sShadowRender || LLPipeline::sReflectionRender ||
        LLPipeline::sImpostorRender || LLViewerCamera::sCurCameraID != LLViewerCamera::CAMERA_WORLD)
    {
        return;
    }
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;

    const F32 now = (F32)gFrameTimeSeconds;
    const F32 length = duration();
    for (size_t i = 0; i < list.size();)
    {
        Kept& k = list[i];
        if (now - k.mTime >= length || !alive(k))
        {
            list.erase(list.begin() + i);
            continue;
        }
        ++i;
        LLSpatialGroup* group = k.mGroup.get();
        if (!group->isVisible() ||
            (LLPipeline::sUseOcclusion && group->isOcclusionState(LLSpatialGroup::OCCLUDED)))
        {
            continue;
        }
        for (auto& entry : k.mInfos)
        {
            if (gPipeline.hasRenderType(entry.first))
            {
                cull->pushDrawInfo(entry.first, entry.second.get());
            }
        }
    }

    // fade-in times that are over
    std::unordered_map<const LLDrawable*, F32>& map = fade_in();
    for (auto it = map.begin(); it != map.end();)
    {
        if (now - it->second >= length)
        {
            it = map.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void FSLODFade::clear()
{
    pending().clear();
    fading().clear();
    fade_in().clear();
    clear_kept(scratch());
}
