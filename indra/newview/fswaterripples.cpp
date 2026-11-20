/**
 * @file fswaterripples.cpp
 * @brief Interactive water: ripples, wakes, entry splashes and churned-water foam (FSRenderWaterRipples)
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

#include "fswaterripples.h"

#include "llagent.h"
#include "llcharacter.h"
#include "lldrawable.h"
#include "llframetimer.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llshadermgr.h"
#include "llspatialpartition.h"
#include "llvertexbuffer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvoavatar.h"
#include "pipeline.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

extern bool gCubeSnapshot;
extern bool gSnapshot;

namespace
{
    // Must match FS_RIPPLE_MAX_SOURCES in fsWaterRippleSimF.glsl
    constexpr S32 MAX_SOURCES = 64;

    // Fixed simulation step. A slow frame runs at most MAX_STEPS of them and drops the rest of its
    // time, so the waves slow down for a moment instead of the frame getting slower still.
    constexpr F64 STEP = 1.0 / 60.0;
    constexpr S32 MAX_STEPS = 4;

    // Longer without a step (water out of view, a stall, a snapshot) and the sources start over:
    // no wake drawn across the gap, no splash for whoever crossed the surface meanwhile
    constexpr F64 GAP_SECONDS = 1.0;

    // A source not seen for this long is forgotten
    constexpr F64 TRACK_SECONDS = 2.0;

    // Further than this between two steps is a jump (teleport, sitting down), not a path
    constexpr F64 JUMP_METRES = 6.0;

    // Moving objects: slower than this (m/s) and they are left out. Avatars always count.
    constexpr F32 MOVING_SPEED = 0.2f;

    // Height pushed down per second, per m/s of speed, at the centre of a 35 cm footprint. It
    // grows with the speed again (up to 4x at 6 m/s), so a runner churns the water more than a
    // walker instead of leaving the same wake spread thinner.
    constexpr F32 MOVE_RATE = 0.03f;
    // Moving objects (boats, vehicles, floats) push this much harder than an avatar of their size
    // would: a hull displaces water along its whole length, and at the avatar's rate their wakes
    // read as too faint. Avatars keep 1.
    constexpr F32 OBJECT_WAKE_GAIN = 1.22f;
    // Idle bob of an avatar standing or swimming in the water: rate amplitude and frequency
    constexpr F32 IDLE_RATE = 0.015f;
    constexpr F32 IDLE_HZ = 0.6f;
    // Entry splash: depth of the impulse at strength 1, and how much wider than the footprint
    constexpr F32 SPLASH_DEPTH = 0.05f;
    constexpr F32 SPLASH_WIDTH = 1.8f;

    // Visual gain on the simulated slope at strength 1
    constexpr F32 SLOPE_GAIN = 2.5f;
    // Churned-water foam: slope sensitivity at strength 1, and how long foam takes to fade to 5 %
    constexpr F32 FOAM_GAIN = 6.f;
    constexpr F64 FOAM_SECONDS = 1.5;

    // A grid that could not be allocated is not tried again every frame
    constexpr F64 FAILED_RETRY_SECONDS = 30.0;

    LLGLSLShader sSimProgram;

    // Two maps in turn: r height (m), g height a step earlier, b foam. Never destroyed at exit:
    // released with the GL buffers.
    LLRenderTarget* sMaps[2] = { nullptr, nullptr };
    S32 sCurrent = 0;      // the map holding the latest state
    bool sValid = false;   // the maps hold a state for the grid below (cleared, or stepped since)
    S32 sRes = 0;          // cells across
    F64 sCell = 0.0;       // cell size, metres
    S64 sOriginX = 0;      // grid corner, in whole cells of global coordinates
    S64 sOriginY = 0;
    F64 sFailedAt = -1.0;

    F64 sLastTime = -1.0;      // when update last ran for the main view
    F64 sLastStepTime = -1.0;  // when the simulation last stepped
    F64 sAccum = 0.0;          // simulated time owed
    U32 sLastFrame = U32_MAX;

    // What is remembered of a source between steps: where it was, so its wake joins up, and
    // whether it was in the water, so crossing the surface splashes once. Keyed by id, never by
    // pointer, so a source that dies or leaves is simply not found again.
    struct Track
    {
        LLVector3d mLast;
        F64 mSeen = 0.0;
        bool mInWater = false;
    };
    std::unordered_map<LLUUID, Track> sTracks;

    struct Source
    {
        LLVector3d mFrom;
        LLVector3d mTo;
        F32 mRadius;
        F32 mAmount;   // metres added at the centre this step (negative pushes down)
        F64 mDist2;    // horizontal distance to the camera, squared
    };

    struct Settings
    {
        F32 mSpeed;
        F32 mDamping;
        F32 mSplash;
        S32 mMaxSources;
        bool mFoam;
        F32 mFoamStrength;
    };

    LLRenderTarget& map(S32 i)
    {
        if (!sMaps[i])
        {
            sMaps[i] = new LLRenderTarget();
        }
        return *sMaps[i];
    }

    bool mapsComplete()
    {
        return sMaps[0] && sMaps[1] && sMaps[0]->isComplete() && sMaps[1]->isComplete();
    }

    void releaseMaps()
    {
        for (LLRenderTarget* target : sMaps)
        {
            if (target)
            {
                target->release();
            }
        }
        sValid = false;
    }

    // AMD doesn't zero new targets: flat water in both maps before either is read
    void clearMaps()
    {
        LLGLDisable scissor(GL_SCISSOR_TEST);
        gGL.setColorMask(true, true);
        const GLfloat zero[4] = { 0.f, 0.f, 0.f, 0.f };
        for (LLRenderTarget* target : sMaps)
        {
            target->bindTarget();
            glClearBufferfv(GL_COLOR, 0, zero);
            target->flush();
        }
        sCurrent = 0;
    }

    bool ensureMaps(S32 res, F64 now)
    {
        if (mapsComplete() && (S32)sMaps[0]->getWidth() == res && (S32)sMaps[1]->getWidth() == res)
        {
            return true;
        }
        if (sFailedAt >= 0.0 && now - sFailedAt < FAILED_RETRY_SECONDS)
        {
            return false;
        }
        releaseMaps();
        if (!map(0).allocate(res, res, GL_RGBA16F, false) || !map(1).allocate(res, res, GL_RGBA16F, false))
        {
            LL_WARNS("Render") << "Water ripples: a " << res << "x" << res << " grid could not be allocated" << LL_ENDL;
            releaseMaps();
            sFailedAt = now;
            return false;
        }
        sFailedAt = -1.0;
        sValid = false; // cleared before use
        return true;
    }

    // Brings a source's track up to date and, when it is in the water, adds what it does to the
    // surface this step
    void addSource(std::vector<Source>& out, const LLUUID& id, const LLVector3d& pos, const LLVector3d& camera,
                   bool in_water, F32 radius, F32 speed, bool idle_bob, F32 sim_dt, F64 now, F32 splash, F32 gain)
    {
        Track& track = sTracks[id];
        const bool fresh = track.mSeen <= 0.0;
        LLVector3d from = fresh ? pos : track.mLast;
        if ((pos - from).magVecSquared() > JUMP_METRES * JUMP_METRES)
        {
            from = pos;
        }
        // Already in the water when first seen (arriving, the grid starting over): no splash
        const bool entered = !fresh && in_water && !track.mInWater;
        track.mLast = pos;
        track.mSeen = now;
        track.mInWater = in_water;
        if (!in_water)
        {
            return;
        }

        F32 amount = 0.f;
        if (speed > 0.1f)
        {
            const F32 size = llclamp(sqrtf(radius / 0.35f), 0.5f, 3.f);
            amount -= MOVE_RATE * speed * llclamp(speed / 1.5f, 0.3f, 4.f) * size * sim_dt;
        }
        if (idle_bob)
        {
            // Each avatar bobs at its own phase, so a crowd doesn't pulse together
            const F32 phase = (F32)(id.getDigest64() % 1024) * (F_TWO_PI / 1024.f);
            const F32 t = (F32)fmod(now, 1000.0);
            amount += IDLE_RATE * sinf(t * IDLE_HZ * F_TWO_PI + phase) * sim_dt;
        }
        if (entered && splash > 0.f)
        {
            amount -= SPLASH_DEPTH * splash * llclamp(0.5f + speed * 0.25f, 0.5f, 2.f);
            radius *= SPLASH_WIDTH;
        }
        amount *= gain; // the simulation clamps each source's push (fs_ripple_src.y) to +-0.5 m
        if (amount == 0.f || !std::isfinite(amount))
        {
            return;
        }

        const F64 dx = pos.mdV[VX] - camera.mdV[VX];
        const F64 dy = pos.mdV[VY] - camera.mdV[VY];
        out.push_back({ from, pos, radius, amount, dx * dx + dy * dy });
    }

    // The avatars and moving objects near the camera, nearest first, at most max_sources. Cheap:
    // the avatar list and the active (moving) object list, no geometry.
    void gatherSources(std::vector<Source>& out, const LLVector3d& camera, F64 half_extent, F32 sim_dt, F64 now,
                       const Settings& settings)
    {
        out.clear();
        const F64 reach = half_extent + 8.0;

        for (LLCharacter* character : LLCharacter::sInstances)
        {
            LLVOAvatar* avatar = static_cast<LLVOAvatar*>(character);
            if (!avatar || avatar->isDead() || avatar->isControlAvatar() || avatar->mDrawable.isNull())
            {
                continue;
            }
            LLViewerRegion* region = avatar->getRegion();
            if (!region)
            {
                continue;
            }
            const LLVector3d pos = avatar->getPositionGlobal();
            if (fabs(pos.mdV[VX] - camera.mdV[VX]) > reach || fabs(pos.mdV[VY] - camera.mdV[VY]) > reach)
            {
                continue;
            }
            // Seated: the seat makes the wake when it moves (a boat, a vehicle: the object loop below).
            // On a still seat the avatar makes none either: the sit pose's pelvis height puts the
            // feet of someone on a deck just above the water below its surface, which rippled and
            // splashed as they sat down.
            if (avatar->isSitting())
            {
                continue;
            }
            F32 height = avatar->mBodySize.mV[VZ];
            if (!(height > 0.5f) || height > 4.f)
            {
                height = 1.8f;
            }
            const F32 foot = (F32)pos.mdV[VZ] - avatar->getPelvisToFoot();
            const F32 water = region->getWaterHeight();
            const bool in_water = foot < water + 0.05f && foot + height > water - 0.1f;
            const F32 radius = llclamp(height * 0.18f, 0.25f, 0.6f);
            addSource(out, avatar->getID(), pos, camera, in_water, radius, avatar->getVelocity().length(), true, sim_dt, now,
                      settings.mSplash, 1.f);
        }

        for (const LLPointer<LLViewerObject>& ptr : gObjectList.getActiveObjects())
        {
            LLViewerObject* object = ptr.get();
            if (!object || object->isDead() || object->isAvatar() || object->getParent() || object->isAttachment() ||
                object->mDrawable.isNull())
            {
                continue;
            }
            const F32 speed = object->getVelocity().length();
            if (!(speed > MOVING_SPEED))
            {
                continue;
            }
            LLViewerRegion* region = object->getRegion();
            if (!region)
            {
                continue;
            }
            const LLVector3d pos = object->getPositionGlobal();
            if (fabs(pos.mdV[VX] - camera.mdV[VX]) > reach || fabs(pos.mdV[VY] - camera.mdV[VY]) > reach)
            {
                continue;
            }
            // The whole linkset's box when it moves as one (its bridge), agent space
            LLDrawable* drawable = object->mDrawable;
            LLSpatialBridge* bridge = drawable->getSpatialBridge();
            const LLVector4a* ext = bridge ? bridge->getSpatialExtents() : drawable->getSpatialExtents();
            const F32 size_x = ext[1][0] - ext[0][0];
            const F32 size_y = ext[1][1] - ext[0][1];
            if (!(size_x >= 0.f && size_y >= 0.f && size_x < 256.f && size_y < 256.f))
            {
                continue; // not computed yet, or not finite
            }
            const F32 water = region->getWaterHeight();
            const bool in_water = ext[0][2] < water && ext[1][2] > water;
            const F32 radius = llclamp(0.5f * llmin(size_x, size_y), 0.3f, 4.f);
            addSource(out, object->getID(), pos, camera, in_water, radius, speed, false, sim_dt, now, settings.mSplash,
                      OBJECT_WAKE_GAIN);
        }

        const size_t max_sources = (size_t)llclamp(settings.mMaxSources, 1, MAX_SOURCES);
        if (out.size() > max_sources)
        {
            std::nth_element(out.begin(), out.begin() + max_sources, out.end(),
                             [](const Source& a, const Source& b) { return a.mDist2 < b.mDist2; });
            out.resize(max_sources);
        }

        // Forget what has not been seen for a while (gone, out of range)
        for (auto it = sTracks.begin(); it != sTracks.end();)
        {
            if (now - it->second.mSeen > TRACK_SECONDS)
            {
                it = sTracks.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    Settings readSettings()
    {
        static LLCachedControl<F32> speed(gSavedSettings, "FSRenderWaterRipplesSpeed", 1.4f);
        static LLCachedControl<F32> damping(gSavedSettings, "FSRenderWaterRipplesDamping", 0.6f);
        static LLCachedControl<F32> splash(gSavedSettings, "FSRenderWaterRipplesSplash", 1.f);
        static LLCachedControl<S32> max_sources(gSavedSettings, "FSRenderWaterRipplesMaxSources", 32);
        static LLCachedControl<bool> foam(gSavedSettings, "FSRenderWaterRippleFoam", false);
        static LLCachedControl<F32> foam_strength(gSavedSettings, "FSRenderWaterRippleFoamStrength", 1.f);
        Settings settings;
        settings.mSpeed = llclamp((F32)speed, 0.1f, 10.f);
        settings.mDamping = llclamp((F32)damping, 0.f, 0.99f);
        settings.mSplash = llclamp((F32)splash, 0.f, 4.f);
        settings.mMaxSources = llclamp((S32)max_sources, 1, MAX_SOURCES);
        settings.mFoam = foam;
        settings.mFoamStrength = llclamp((F32)foam_strength, 0.f, 4.f);
        return settings;
    }

    void gridSettings(S32& res, F64& cell)
    {
        static LLCachedControl<F32> grid_size(gSavedSettings, "FSRenderWaterRipplesGridSize", 64.f);
        static LLCachedControl<S32> resolution(gSavedSettings, "FSRenderWaterRipplesResolution", 512);
        res = llclamp((S32)resolution, 128, 1024);
        cell = (F64)llclamp((F32)grid_size, 16.f, 256.f) / (F64)res;
    }

    // Runs 'steps' simulation steps: the grid shift and the sources go into the first
    void simulate(LLVertexBuffer* screen_triangle, S32 steps, S32 shift_x, S32 shift_y, const std::vector<Source>& sources,
                  const Settings& settings)
    {
        static LLStaticHashedString s_prev("fs_ripple_prev");
        static LLStaticHashedString s_shift("fs_ripple_shift");
        static LLStaticHashedString s_sim("fs_ripple_sim");
        static LLStaticHashedString s_cell("fs_ripple_cell");
        static LLStaticHashedString s_count("fs_ripple_count");
        static LLStaticHashedString s_seg("fs_ripple_seg");
        static LLStaticHashedString s_src("fs_ripple_src");

        // Waves cross (speed * step / cell) cells a step. The explicit scheme is only stable below
        // 1/sqrt(2) of a cell; very fine cells hold the speed down rather than blow up.
        const F64 courant = llmin((F64)settings.mSpeed * STEP / sCell, 0.67);
        const F32 k = (F32)(courant * courant);
        // Amplitude keeps sqrt(keep) per step: (1 - damping) per second
        const F32 keep = (F32)pow(1.0 - (F64)settings.mDamping, 2.0 * STEP);
        const F32 foam_keep = (F32)pow(0.05, STEP / FOAM_SECONDS);
        const F32 foam_gain = settings.mFoam ? FOAM_GAIN * settings.mFoamStrength : 0.f;

        // Sources in cells of the grid as it is after this frame's shift
        GLfloat seg[MAX_SOURCES * 4];
        GLfloat src[MAX_SOURCES * 4];
        const S32 count = llmin((S32)sources.size(), MAX_SOURCES);
        const F64 corner_x = (F64)sOriginX;
        const F64 corner_y = (F64)sOriginY;
        for (S32 i = 0; i < count; ++i)
        {
            const Source& source = sources[i];
            seg[i * 4 + 0] = (GLfloat)(source.mFrom.mdV[VX] / sCell - corner_x);
            seg[i * 4 + 1] = (GLfloat)(source.mFrom.mdV[VY] / sCell - corner_y);
            seg[i * 4 + 2] = (GLfloat)(source.mTo.mdV[VX] / sCell - corner_x);
            seg[i * 4 + 3] = (GLfloat)(source.mTo.mdV[VY] / sCell - corner_y);
            src[i * 4 + 0] = (GLfloat)llmax((F64)source.mRadius / sCell, 1.5);
            src[i * 4 + 1] = (GLfloat)llclamp(source.mAmount, -0.5f, 0.5f);
            src[i * 4 + 2] = 0.f;
            src[i * 4 + 3] = 0.f;
        }

        gGL.flush();
        LLGLDepthTest depth(GL_FALSE, GL_FALSE);
        LLGLDisable blend(GL_BLEND);
        LLGLDisable cull(GL_CULL_FACE);
        LLGLDisable scissor(GL_SCISSOR_TEST);
        gGL.setColorMask(true, true);

        sSimProgram.bind();
        sSimProgram.uniform1i(s_prev, 0);
        sSimProgram.uniform4f(s_sim, k, keep, foam_keep, foam_gain);
        sSimProgram.uniform2f(s_cell, (F32)sCell, (F32)sRes);
        if (count > 0)
        {
            sSimProgram.uniform4fv(s_seg, count, seg);
            sSimProgram.uniform4fv(s_src, count, src);
        }
        for (S32 step = 0; step < steps; ++step)
        {
            LLRenderTarget& read = *sMaps[sCurrent];
            LLRenderTarget& write = *sMaps[1 - sCurrent];
            write.bindTarget();
            read.bindTexture(0, 0, LLTexUnit::TFO_BILINEAR); // texelFetch: the filter is the one the water samples with
            sSimProgram.uniform2f(s_shift, step == 0 ? (F32)shift_x : 0.f, step == 0 ? (F32)shift_y : 0.f);
            sSimProgram.uniform1i(s_count, step == 0 ? count : 0);
            screen_triangle->setBuffer();
            screen_triangle->drawArrays(LLRender::TRIANGLES, 0, 3);
            write.flush();
            sCurrent = 1 - sCurrent;
        }
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        sSimProgram.unbind();
    }
}

bool FSWaterRipples::enabled()
{
    static LLCachedControl<bool> ripples(gSavedSettings, "FSRenderWaterRipples", false);
    return ripples;
}

void FSWaterRipples::loadShaders(S32 shader_level)
{
    sSimProgram.unload();
    if (!enabled() || shader_level <= 0)
    {
        release(); // switched off: give the memory back
        return;
    }

    sSimProgram.mName = "FS Water Ripples Simulation";
    sSimProgram.mShaderFiles.clear();
    sSimProgram.clearPermutations();
    sSimProgram.mShaderFiles.push_back(std::make_pair("deferred/postDeferredNoTCV.glsl", GL_VERTEX_SHADER));
    sSimProgram.mShaderFiles.push_back(std::make_pair("environment/fsWaterRippleSimF.glsl", GL_FRAGMENT_SHADER));
    sSimProgram.mShaderLevel = shader_level;
    if (!sSimProgram.createShader())
    {
        LL_WARNS("ShaderLoading") << "FS Water Ripples Simulation failed to build, the water stays still" << LL_ENDL;
        sSimProgram.unload();
    }
}

void FSWaterRipples::update(LLVertexBuffer* screen_triangle)
{
    if (!enabled())
    {
        if (sValid || (sMaps[0] && sMaps[0]->isComplete()) || !sTracks.empty())
        {
            release();
        }
        return;
    }
    // Main view only, once a frame
    if (gCubeSnapshot || LLPipeline::sRenderingHUDs || !screen_triangle || !sSimProgram.isComplete())
    {
        return;
    }
    const U32 frame = LLFrameTimer::getFrameCount();
    if (frame == sLastFrame)
    {
        return;
    }
    const F64 now = LLFrameTimer::getElapsedSeconds();
    // Frames went by without an update for over GAP_SECONDS: the water was out of view (the water
    // pool updates only while it draws water). It comes back flat, not with the waves it had when
    // it went. One long frame (a stall) skips no frame and keeps its waves, as before.
    const bool was_unseen = sLastFrame != U32_MAX && frame - sLastFrame > 1 && sLastTime >= 0.0 && now - sLastTime > GAP_SECONDS;
    sLastFrame = frame;

    if (gSnapshot)
    {
        // Frozen: every tile of a hi-res snapshot must see the same water. The time spent is
        // not owed afterwards.
        sLastTime = now;
        return;
    }
    if (was_unseen)
    {
        sValid = false; // cleared below, with the sources
    }
    if (!gAgent.getRegion())
    {
        return;
    }

    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;

    S32 res = 0;
    F64 cell = 0.0;
    gridSettings(res, cell);
    if (res != sRes || cell != sCell)
    {
        releaseMaps();
        sTracks.clear();
        sRes = res;
        sCell = cell;
        sFailedAt = -1.0;
    }
    if (!ensureMaps(res, now))
    {
        return;
    }

    // Time owed, in whole steps
    const F64 elapsed = sLastTime < 0.0 ? 0.0 : now - sLastTime;
    sLastTime = now;
    if (sLastStepTime < 0.0 || now - sLastStepTime > GAP_SECONDS)
    {
        sTracks.clear(); // no wakes across the gap
        sAccum = STEP;
    }
    else
    {
        sAccum += llclamp(elapsed, 0.0, 0.25);
    }
    S32 steps = (S32)(sAccum / STEP);
    if (steps > MAX_STEPS)
    {
        steps = MAX_STEPS;
        sAccum = 0.0;
    }
    else
    {
        sAccum -= steps * STEP;
    }
    if (steps <= 0 && sValid)
    {
        return; // faster than the step rate: the sources wait for the next step, their wakes join up
    }

    // The grid follows the camera in whole cells, so what is in it stays put in the world. Global
    // coordinates, so crossing into another region moves nothing.
    const LLVector3d camera = gAgent.getPosGlobalFromAgent(LLViewerCamera::getInstance()->getOrigin());
    const S64 origin_x = (S64)floor(camera.mdV[VX] / sCell) - res / 2;
    const S64 origin_y = (S64)floor(camera.mdV[VY] / sCell) - res / 2;
    S64 shift_x = origin_x - sOriginX;
    S64 shift_y = origin_y - sOriginY;
    if (!sValid || llabs(shift_x) >= res / 2 || llabs(shift_y) >= res / 2)
    {
        // Starting, or a jump (teleport, a camera far away): flat water and fresh sources
        clearMaps();
        sTracks.clear();
        shift_x = 0;
        shift_y = 0;
        sValid = true;
    }
    sOriginX = origin_x;
    sOriginY = origin_y;
    if (steps <= 0)
    {
        return;
    }
    sLastStepTime = now;

    const Settings settings = readSettings();
    static std::vector<Source> sources;
    gatherSources(sources, camera, 0.5 * sCell * res, (F32)(steps * STEP), now, settings);

    LL_PROFILE_GPU_ZONE("water ripples sim");
    simulate(screen_triangle, steps, (S32)shift_x, (S32)shift_y, sources, settings);
}

S32 FSWaterRipples::bind(LLGLSLShader& shader, S32 unit)
{
    static LLStaticHashedString s_map("fs_ripple_map");
    static LLStaticHashedString s_grid("fs_ripple_grid");
    static LLStaticHashedString s_params("fs_ripple_params");
    static LLCachedControl<F32> strength(gSavedSettings, "FSRenderWaterRipplesStrength", 1.f);
    static LLCachedControl<bool> foam(gSavedSettings, "FSRenderWaterRippleFoam", false);
    static LLCachedControl<F32> foam_strength(gSavedSettings, "FSRenderWaterRippleFoamStrength", 1.f);

    if (shader.getUniformLocation(s_params) < 0)
    {
        return -1; // built without FS_WATER_RIPPLES
    }

    const bool unit_ok = unit >= 0 && unit < llmin(gGLManager.mNumTextureImageUnits, (S32)LL_NUM_TEXTURE_LAYERS);
    // The sampler always names a free unit, so it never shares one with a sampler of another type
    shader.uniform1i(s_map, unit_ok ? unit : llmax(shader.getTextureChannel(LLShaderMgr::BUMP_MAP), 0));
    if (!unit_ok || !enabled() || !sValid || !mapsComplete() || sRes <= 0)
    {
        shader.uniform4f(s_params, 0.f, 0.f, 0.f, 0.f);
        return -1;
    }

    sMaps[sCurrent]->bindTexture(0, unit, LLTexUnit::TFO_BILINEAR);

    const LLVector3 corner = gAgent.getPosAgentFromGlobal(LLVector3d((F64)sOriginX * sCell, (F64)sOriginY * sCell, 0.0));
    const F32 size = (F32)(sCell * sRes);
    shader.uniform4f(s_grid, corner.mV[VX], corner.mV[VY], 1.f / size, 1.f / (F32)sRes);

    // Wrapped so the phase keeps its precision; the foam pattern jumps once an hour
    const F32 phase = (F32)fmod(LLFrameTimer::getElapsedSeconds(), 3600.0);
    shader.uniform4f(s_params, llclamp((F32)strength, 0.f, 4.f) * SLOPE_GAIN, (F32)(0.5 / sCell),
                     foam ? llclamp((F32)foam_strength, 0.f, 4.f) : 0.f, phase); // 0 to 4 as the setting: the shader clamps the foam after it
    return unit;
}

void FSWaterRipples::release()
{
    releaseMaps();
    sTracks.clear();
    sRes = 0;
    sCell = 0.0;
    sAccum = 0.0;
    sLastTime = -1.0;
    sLastStepTime = -1.0;
    sFailedAt = -1.0;
}
