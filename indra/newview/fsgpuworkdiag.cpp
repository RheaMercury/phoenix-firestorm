/**
 * @file fsgpuworkdiag.cpp
 * @brief FSDiagnosticsGPUWork: repeated geometry, hidden geometry and texture memory by visibility, in the log
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

#include "fsgpuworkdiag.h"

#include "fsrepresentativefragment.h"

#include "lldrawable.h"
#include "lldrawpool.h"
#include "llface.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llspatialpartition.h"
#include "llvertexbuffer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobjectlist.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "llviewertexturelist.h"
#include "llvolume.h"
#include "llvovolume.h"
#include "pipeline.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

extern bool gCubeSnapshot;
U32 get_box_fan_indices(LLCamera* camera, const LLVector4a& center);   // llvieweroctree.cpp

namespace
{
    constexpr F32 LOG_INTERVAL = 10.f;
    constexpr U32 MAX_BOX_QUERIES = 8192;   // objects tested in one sample frame
    constexpr U32 QUERY_GIVE_UP_FRAMES = 120;
    constexpr F32 BOX_FUDGE = 0.25f;        // SG_OCCLUSION_FUDGE: the group queries pad their boxes the same

    bool on()
    {
        static LLCachedControl<bool> enabled(gSavedSettings, "FSDiagnosticsGPUWork", false);
        return enabled;
    }

    // Group occlusion query results for the main camera since the last log line
    U64 sGroupQueriesVisible = 0;
    U64 sGroupQueriesOccluded = 0;

    // The last finished sample frame
    struct OcclusionSample
    {
        bool mValid = false;
        U32 mGroups = 0;            // render-by-group groups in view (frustum)
        U32 mGroupsOccluded = 0;    // of those, hidden by their occlusion query
        U64 mBytes = 0;             // geometry bytes in those groups
        U64 mBytesOccluded = 0;
        U32 mBridgeGroups = 0;      // moving objects' groups: their boxes are in another space, not tested
        U32 mObjects = 0;           // objects drawn from the unoccluded groups
        U32 mNear = 0;              // the camera inside their box: counted as visible, no query
        U32 mTested = 0;
        U32 mHidden = 0;            // tested and behind the finished G-buffer depth
        U64 mTris = 0;              // opaque triangles of the drawn objects
        U64 mAlphaTris = 0;         // alpha (blended) triangles of the drawn objects
        U64 mHiddenTris = 0;
        U64 mHiddenAlphaTris = 0;
        U32 mOverCap = 0;           // objects not tested for the query cap
        U32 mLost = 0;              // queries given up on
    };
    OcclusionSample sLast;
    OcclusionSample sPendingSample;

    struct PendingQuery
    {
        GLuint mId;
        U32 mTris;
        U32 mAlphaTris;
    };
    std::vector<PendingQuery> sPending;
    U32 sPendingFrames = 0;
    bool sSampleWanted = false;

    void finish_sample()
    {
        sLast = sPendingSample;
        sLast.mValid = true;
        sPending.clear();
        sPendingFrames = 0;
    }

    // Reads the queries of the sample frame that are in, without waiting
    void poll_queries()
    {
        if (sPending.empty())
        {
            return;
        }
        ++sPendingFrames;
        std::vector<PendingQuery> still;
        std::vector<GLuint> done;
        still.reserve(sPending.size());
        done.reserve(sPending.size());
        for (const PendingQuery& q : sPending)
        {
            GLuint available = 0;
            glGetQueryObjectuiv(q.mId, GL_QUERY_RESULT_AVAILABLE, &available);
            if (!available)
            {
                if (sPendingFrames > QUERY_GIVE_UP_FRAMES)
                {
                    ++sPendingSample.mLost;
                    done.push_back(q.mId);
                }
                else
                {
                    still.push_back(q);
                }
                continue;
            }
            GLuint passed = 1;
            glGetQueryObjectuiv(q.mId, GL_QUERY_RESULT, &passed);
            ++sPendingSample.mTested;
            if (!passed)
            {
                ++sPendingSample.mHidden;
                sPendingSample.mHiddenTris += q.mTris;
                sPendingSample.mHiddenAlphaTris += q.mAlphaTris;
            }
            done.push_back(q.mId);
        }
        if (!done.empty())
        {
            glDeleteQueries((GLsizei)done.size(), done.data());
        }
        sPending.swap(still);
        if (sPending.empty())
        {
            finish_sample();
        }
    }

    bool camera_inside(const LLCamera& camera, const LLVector4a& center, const LLVector4a& size)
    {
        LLVector4a pad;
        pad.splat(BOX_FUDGE * 2.f);
        LLVector4a r;
        r.setAdd(size, pad);
        LLVector4a min;
        min.setSub(center, r);
        LLVector4a max;
        max.setAdd(center, r);
        LLVector4a e;
        e.load3(camera.getOrigin().mV);
        return !(e.lessThan(min).getGatheredBits() & 0x7) && !(e.greaterThan(max).getGatheredBits() & 0x7);
    }

    struct Box
    {
        LLVector4a mCenter;
        LLVector4a mSize;
        U32 mTris;
        U32 mAlphaTris;
    };

    void issue_sample(LLCamera& camera, LLCullResult* cull)
    {
        sPendingSample = OcclusionSample();
        std::vector<Box> boxes;
        boxes.reserve(1024);

        for (LLCullResult::sg_iterator it = cull->beginVisibleGroups(); it != cull->endVisibleGroups(); ++it)
        {
            LLSpatialGroup* group = *it;
            if (!group || group->isDead() || group->isEmpty())
            {
                continue;
            }
            ++sPendingSample.mGroups;
            sPendingSample.mBytes += group->mGeometryBytes;
            if (LLPipeline::sUseOcclusion > 1 && group->isOcclusionState(LLSpatialGroup::OCCLUDED))
            {
                ++sPendingSample.mGroupsOccluded;
                sPendingSample.mBytesOccluded += group->mGeometryBytes;
                continue;
            }
            if (group->getSpatialPartition()->asBridge())
            {
                ++sPendingSample.mBridgeGroups;
                continue;
            }
            for (LLSpatialGroup::element_iter i = group->getDataBegin(); i != group->getDataEnd(); ++i)
            {
                LLDrawable* drawable = (LLDrawable*)(*i)->getDrawable();
                if (!drawable || drawable->isDead() || drawable->isAvatar() || drawable->isActive() || !drawable->getVOVolume())
                {
                    continue;
                }
                U32 tris = 0;
                U32 alpha_tris = 0;
                for (S32 f = 0; f < drawable->getNumFaces(); ++f)
                {
                    LLFace* face = drawable->getFace(f);
                    if (!face || !face->hasGeometry())
                    {
                        continue;
                    }
                    if (face->getPoolType() == LLDrawPool::POOL_ALPHA)
                    {
                        alpha_tris += face->getIndicesCount() / 3;
                    }
                    else
                    {
                        tris += face->getIndicesCount() / 3;
                    }
                }
                if (!tris && !alpha_tris)
                {
                    continue;
                }
                ++sPendingSample.mObjects;
                sPendingSample.mTris += tris;
                sPendingSample.mAlphaTris += alpha_tris;

                const LLVector4a* ext = drawable->getSpatialExtents();
                Box box;
                box.mCenter.setAdd(ext[0], ext[1]);
                box.mCenter.mul(0.5f);
                box.mSize.setSub(ext[1], ext[0]);
                box.mSize.mul(0.5f);
                box.mTris = tris;
                box.mAlphaTris = alpha_tris;
                if (camera_inside(camera, box.mCenter, box.mSize))
                {
                    ++sPendingSample.mNear;
                }
                else if (boxes.size() >= MAX_BOX_QUERIES)
                {
                    ++sPendingSample.mOverCap;
                }
                else
                {
                    boxes.push_back(box);
                }
            }
        }

        if (boxes.empty())
        {
            finish_sample();
            return;
        }

        std::vector<GLuint> ids(boxes.size());
        glGenQueries((GLsizei)ids.size(), ids.data());

        // The state LLPipeline::doOcclusion draws its group boxes with, against the finished G-buffer depth
        LLVertexBuffer::unbind();
        gGL.setColorMask(false, false);
        LLGLDisable blend(GL_BLEND);
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        LLGLDepthTest depth(GL_TRUE, GL_FALSE);
        LLGLDisable cull_face(GL_CULL_FACE);
        FSRepresentativeFragmentScope representative_fragment; // query-only draws: any sample is enough
        gGLLastMatrix = NULL;
        gGL.matrixMode(LLRender::MM_MODELVIEW);
        gGL.loadMatrix(gGLModelView);

        gOcclusionCubeProgram.bind();
        gPipeline.mCubeVB->setBuffer();
        sPending.reserve(boxes.size());
        for (size_t b = 0; b < boxes.size(); ++b)
        {
            const Box& box = boxes[b];
            glBeginQuery(GL_ANY_SAMPLES_PASSED, ids[b]);
            gOcclusionCubeProgram.uniform3fv(LLShaderMgr::BOX_CENTER, 1, box.mCenter.getF32ptr());
            gOcclusionCubeProgram.uniform3f(LLShaderMgr::BOX_SIZE, box.mSize[0] + BOX_FUDGE, box.mSize[1] + BOX_FUDGE,
                                            box.mSize[2] + BOX_FUDGE);
            gPipeline.mCubeVB->drawRange(LLRender::TRIANGLE_FAN, 0, 7, 8, get_box_fan_indices(&camera, box.mCenter));
            glEndQuery(GL_ANY_SAMPLES_PASSED);
            sPending.push_back({ ids[b], box.mTris, box.mAlphaTris });
        }
        gOcclusionCubeProgram.unbind();
        LLVertexBuffer::unbind();
        gGL.setColorMask(true, true);
        sPendingFrames = 0;
    }

    F64 to_mb(U64 bytes)
    {
        return (F64)bytes / (1024.0 * 1024.0);
    }

    F64 pct(F64 part, F64 whole)
    {
        return whole > 0.0 ? 100.0 * part / whole : 0.0;
    }

    // Objects sharing one LLVolume: LLVolumeMgr hands every object with the same volume parameters (the
    // same mesh asset, or the same prim shape) at the same LOD the same LLVolume, but each object's faces
    // are written into the vertex buffers of its spatial group, transformed, as its own copy.
    std::string repeats_line()
    {
        struct Stat
        {
            U32 mInstances = 0;
            U32 mVisible = 0;
            U32 mVisibleFaces = 0;
            U64 mBytesAll = 0;
            U64 mBytesEach = 0;     // the largest one copy: what one shared copy would take
            U64 mVertsAll = 0;
            U32 mVertsEach = 0;
            bool mMesh = false;
            LLUUID mAsset;
        };
        std::unordered_map<const LLVolume*, Stat> stats;
        stats.reserve(8192);
        U32 objects = 0;
        U32 skipped_attached = 0;
        U32 skipped_rigged = 0;
        U64 active_bytes = 0;

        const S32 count = gObjectList.getNumObjects();
        for (S32 i = 0; i < count; ++i)
        {
            LLViewerObject* obj = gObjectList.getObject(i);
            if (!obj || obj->isDead() || obj->getPCode() != LL_PCODE_VOLUME)
            {
                continue;
            }
            if (obj->isAttachment() || obj->isHUDAttachment())
            {
                ++skipped_attached;
                continue;
            }
            LLVOVolume* vobj = static_cast<LLVOVolume*>(obj);
            LLDrawable* drawable = obj->mDrawable;
            const LLVolume* volume = vobj->getVolume();
            if (!drawable || !volume)
            {
                continue;
            }
            if (vobj->isRiggedMesh())
            {
                ++skipped_rigged;   // animesh: skinned per object
                continue;
            }
            U64 bytes = 0;
            U32 verts = 0;
            U32 faces = 0;
            for (S32 f = 0; f < drawable->getNumFaces(); ++f)
            {
                LLFace* face = drawable->getFace(f);
                LLVertexBuffer* vb = face ? face->getVertexBuffer() : nullptr;
                if (!face || !face->hasGeometry() || !vb)
                {
                    continue;
                }
                verts += face->getGeomCount();
                bytes += (U64)face->getGeomCount() * LLVertexBuffer::calcVertexSize(vb->getTypeMask()) +
                         (U64)face->getIndicesCount() * sizeof(U16);
                ++faces;
            }
            if (!verts)
            {
                continue;
            }
            ++objects;
            if (drawable->isActive())
            {
                active_bytes += bytes;
            }
            Stat& s = stats[volume];
            ++s.mInstances;
            s.mBytesAll += bytes;
            s.mVertsAll += verts;
            s.mBytesEach = llmax(s.mBytesEach, bytes);
            s.mVertsEach = llmax(s.mVertsEach, verts);
            if (drawable->isVisible())
            {
                ++s.mVisible;
                s.mVisibleFaces += faces;
            }
            if (s.mInstances == 1)
            {
                s.mMesh = vobj->isMesh();
                s.mAsset = s.mMesh ? volume->getParams().getSculptID() : LLUUID::null;
            }
        }

        U64 bytes_all = 0, bytes_kept = 0, verts_all = 0, verts_kept = 0;
        U64 mesh_all = 0, mesh_kept = 0, prim_all = 0, prim_kept = 0;
        U32 repeated_volumes = 0, repeated_objects = 0, visible_objects = 0, visible_repeated = 0;
        U32 visible_faces = 0, visible_repeated_faces = 0;
        std::vector<const Stat*> top;
        for (const auto& entry : stats)
        {
            const Stat& s = entry.second;
            bytes_all += s.mBytesAll;
            bytes_kept += s.mBytesEach;
            verts_all += s.mVertsAll;
            verts_kept += s.mVertsEach;
            (s.mMesh ? mesh_all : prim_all) += s.mBytesAll;
            (s.mMesh ? mesh_kept : prim_kept) += s.mBytesEach;
            visible_objects += s.mVisible;
            visible_faces += s.mVisibleFaces;
            if (s.mInstances > 1)
            {
                ++repeated_volumes;
                repeated_objects += s.mInstances;
                visible_repeated += s.mVisible;
                visible_repeated_faces += s.mVisibleFaces;
                top.push_back(&s);
            }
        }
        const size_t top_n = llmin(top.size(), (size_t)5);
        std::partial_sort(top.begin(), top.begin() + top_n, top.end(), [](const Stat* a, const Stat* b)
                          { return a->mBytesAll - a->mBytesEach > b->mBytesAll - b->mBytesEach; });

        std::string out = llformat(
            "Repeats: %u world objects with geometry (%u attachments and %u animesh left out) on %u distinct volumes; "
            "%u volumes used more than once, by %u objects; vertices %llu, %llu of them (%.0f%%) repeats; "
            "face geometry %.1f MB, %.1f MB of it (%.0f%%) repeats (mesh %.1f of %.1f MB, prims %.1f of %.1f MB; "
            "moving objects %.1f MB); drawn: %u objects with %u faces, %u objects with %u faces of them on repeated volumes; "
            "vertex buffers %.0f MB in all; most repeated:",
            objects, skipped_attached, skipped_rigged, (U32)stats.size(), repeated_volumes, repeated_objects,
            verts_all, verts_all - verts_kept, pct((F64)(verts_all - verts_kept), (F64)verts_all),
            to_mb(bytes_all), to_mb(bytes_all - bytes_kept), pct((F64)(bytes_all - bytes_kept), (F64)bytes_all),
            to_mb(mesh_all - mesh_kept), to_mb(mesh_all), to_mb(prim_all - prim_kept), to_mb(prim_all), to_mb(active_bytes),
            visible_objects, visible_faces, visible_repeated, visible_repeated_faces,
            to_mb(LLVertexBuffer::getBytesAllocated()));
        for (size_t i = 0; i < top_n; ++i)
        {
            const Stat* s = top[i];
            out += llformat(" %s x%u (%u drawn) %u verts each %.1f MB;", s->mMesh ? s->mAsset.asString().c_str() : "prim shape",
                            s->mInstances, s->mVisible, s->mVertsEach, to_mb(s->mBytesAll));
        }
        return out;
    }

    std::string occlusion_line()
    {
        std::string out = llformat("Occlusion: group query results (render and object cache trees) in the last 10 s: %llu visible, %llu occluded (%.0f%%)",
                                   sGroupQueriesVisible, sGroupQueriesOccluded,
                                   pct((F64)sGroupQueriesOccluded, (F64)(sGroupQueriesVisible + sGroupQueriesOccluded)));
        sGroupQueriesVisible = 0;
        sGroupQueriesOccluded = 0;
        if (!sLast.mValid)
        {
            return out + "; no sample frame finished yet";
        }
        const OcclusionSample& s = sLast;
        out += llformat("; sample frame: groups in view %u, %u occluded (%.0f%%) holding %.1f of %.1f MB geometry (%.0f%%), "
                        "%u moving-object groups not tested; drawn static objects %u (%llu opaque, %llu alpha triangles): "
                        "camera inside the box %u, tested %u, behind the finished depth %u (%.0f%% of tested), "
                        "%llu opaque triangles (%.0f%%) and %llu alpha triangles (%.0f%%) of them; %u over the query cap, %u queries lost",
                        s.mGroups, s.mGroupsOccluded, pct(s.mGroupsOccluded, s.mGroups), to_mb(s.mBytesOccluded), to_mb(s.mBytes),
                        pct((F64)s.mBytesOccluded, (F64)s.mBytes), s.mBridgeGroups, s.mObjects, s.mTris, s.mAlphaTris, s.mNear, s.mTested,
                        s.mHidden, pct(s.mHidden, s.mTested), s.mHiddenTris, pct((F64)s.mHiddenTris, (F64)s.mTris), s.mHiddenAlphaTris,
                        pct((F64)s.mHiddenAlphaTris, (F64)s.mAlphaTris), s.mOverCap, s.mLost);
        return out;
    }

    // Unboosted (LOD) textures only: those the face sizes and the discard bias decide.
    std::string textures_line()
    {
        struct Bucket
        {
            F64 mMB = 0.0;
            U32 mCount = 0;
            void add(F64 mb) { mMB += mb; ++mCount; }
        };
        Bucket all, visible, visible_alpha_only, hidden, off_screen, no_faces, maps_only, full_size, wants_sharper;
        const U32 MAX_FACES = 256;  // per texture; a texture on more is called visible

        const LLViewerCamera::eCameraID saved_camera = LLViewerCamera::sCurCameraID;
        LLViewerCamera::sCurCameraID = LLViewerCamera::CAMERA_WORLD;   // the main camera's occlusion state
        for (const LLPointer<LLViewerFetchedTexture>& ptr : gTextureList)
        {
            LLViewerFetchedTexture* imagep = ptr.get();
            if (!imagep || !imagep->hasGLTexture() || imagep->getType() != LLViewerTexture::LOD_TEXTURE ||
                imagep->getBoostLevel() != LLGLTexture::BOOST_NONE)
            {
                continue;
            }
            const F64 mb = (F64)imagep->getTextureMemory().value() / (1024.0 * 1024.0);
            if (mb <= 0.0)
            {
                continue;
            }
            all.add(mb);
            if (imagep->getDiscardLevel() == 0)
            {
                full_size.add(mb);
            }
            if (imagep->getDesiredDiscardLevel() < imagep->getDiscardLevel())
            {
                wants_sharper.add(mb);
            }

            bool any_face = false, colour = false, in_view = false, seen = false, seen_opaque = false;
            U32 checked = 0;
            for (U32 ch = 0; ch < LLRender::NUM_TEXTURE_CHANNELS; ++ch)
            {
                const S32 n = imagep->getNumFaces(ch);
                if (n > 0 && (ch == LLRender::DIFFUSE_MAP || ch == LLRender::BASECOLOR_MAP))
                {
                    colour = true;
                }
                for (S32 fi = 0; fi < n; ++fi)
                {
                    any_face = true;
                    if (++checked > MAX_FACES)
                    {
                        seen = seen_opaque = in_view = true;
                        break;
                    }
                    LLFace* face = (*imagep->getFaceList(ch))[fi];
                    LLDrawable* drawable = face ? face->getDrawable() : nullptr;
                    if (!drawable || !face->mInFrustum)
                    {
                        continue;
                    }
                    in_view = true;
                    LLSpatialGroup* group = drawable->getSpatialGroup();
                    if (group && LLPipeline::sUseOcclusion > 1 && group->isOcclusionState(LLSpatialGroup::OCCLUDED))
                    {
                        continue;
                    }
                    seen = true;
                    if (face->getPoolType() != LLDrawPool::POOL_ALPHA)
                    {
                        seen_opaque = true;
                    }
                }
                if (checked > MAX_FACES)
                {
                    break;
                }
            }
            if (!any_face)
            {
                no_faces.add(mb);   // sculpt maps, textures of objects gone, or only on avatars and particles
                continue;
            }
            if (!colour)
            {
                maps_only.add(mb);
            }
            if (seen)
            {
                visible.add(mb);
                if (!seen_opaque)
                {
                    visible_alpha_only.add(mb);
                }
            }
            else if (in_view)
            {
                hidden.add(mb);
            }
            else
            {
                off_screen.add(mb);
            }
        }
        LLViewerCamera::sCurCameraID = saved_camera;

        return llformat("Textures (unboosted): %.0f MB in %u; on a visible face %.0f MB in %u (of which only on alpha faces %.0f MB in %u), "
                        "in view but only in occluded groups %.0f MB in %u, off screen %.0f MB in %u, on no face %.0f MB in %u; "
                        "normal/specular/PBR maps without a colour use %.0f MB in %u; at full size %.0f MB in %u; "
                        "wanting a sharper level %.0f MB in %u; discard bias %.2f",
                        all.mMB, all.mCount, visible.mMB, visible.mCount, visible_alpha_only.mMB, visible_alpha_only.mCount,
                        hidden.mMB, hidden.mCount, off_screen.mMB, off_screen.mCount, no_faces.mMB, no_faces.mCount,
                        maps_only.mMB, maps_only.mCount, full_size.mMB, full_size.mCount, wants_sharper.mMB, wants_sharper.mCount,
                        LLViewerTexture::sDesiredDiscardBias);
    }
}

namespace FSGPUWorkDiag
{
    void frame()
    {
        static LLFrameTimer timer;
        if (!on())
        {
            timer.reset();
            sSampleWanted = false;
            return;
        }
        if (timer.getElapsedTimeF32() < LOG_INTERVAL)
        {
            return;
        }
        timer.reset();

        LLTimer cost;
        const std::string repeats = repeats_line();
        const std::string textures = textures_line();
        LL_INFOS("FSGPUWork") << repeats << LL_ENDL;
        LL_INFOS("FSGPUWork") << occlusion_line() << LL_ENDL;
        LL_INFOS("FSGPUWork") << textures << llformat(" (these walks took %.1f ms)", cost.getElapsedTimeF32() * 1000.f) << LL_ENDL;
        sSampleWanted = true;
    }

    void afterGBuffer(LLCamera& camera, LLCullResult* cull)
    {
        if (!on())
        {
            if (!sPending.empty())
            {   // turned off with a sample in flight: let the query names go
                std::vector<GLuint> ids;
                ids.reserve(sPending.size());
                for (const PendingQuery& q : sPending)
                {
                    ids.push_back(q.mId);
                }
                glDeleteQueries((GLsizei)ids.size(), ids.data());
                sPending.clear();
            }
            return;
        }
        if (!cull || gCubeSnapshot || LLPipeline::sShadowRender || LLPipeline::sReflectionRender ||
            LLPipeline::sRenderingHUDs || LLViewerCamera::sCurCameraID != LLViewerCamera::CAMERA_WORLD)
        {
            return;
        }
        poll_queries();
        if (sSampleWanted && sPending.empty() && gPipeline.mCubeVB.notNull())
        {
            sSampleWanted = false;
            issue_sample(camera, cull);
        }
    }

    void noteGroupQuery(bool occluded)
    {
        if (LLViewerCamera::sCurCameraID != LLViewerCamera::CAMERA_WORLD || !on())
        {
            return;
        }
        if (occluded)
        {
            ++sGroupQueriesOccluded;
        }
        else
        {
            ++sGroupQueriesVisible;
        }
    }
}
