/**
 * @file fswindfield.cpp
 * @brief The region's wind, smoothed for the rendering effects (FSRenderWindEffects)
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

#include "fswindfield.h"

#include "llagent.h"
#include "llframetimer.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llwind.h"
#include "llworld.h"
#include "pipeline.h"

extern bool gCubeSnapshot;

namespace
{
    constexpr F32 SAMPLE_SPACING = 32.f;    // metres between the points averaged around the camera
    constexpr F32 EASE_TIME = 4.f;          // seconds: time constant of the easing
    constexpr F32 MAX_SPEED = 12.f;         // m/s; region wind can read 10 to 20
    constexpr F32 MAX_STEP = 0.25f;         // seconds: a longer frame (loading, a stall) counts as this
    constexpr F64 JUMP_DISTANCE = 256.0;    // the camera moved this far in a frame: start afresh
    constexpr F32 LOG_INTERVAL = 10.f;      // seconds between stats lines

    // Gusts: slow noise, two octaves of 13 and 5.3 seconds
    constexpr F64 GUST_PERIOD = 13.0;
    constexpr F64 GUST_PERIOD_FAST = 5.3;
    constexpr F32 GUST_CALM = 0.5f;         // m/s: below this there are no gusts
    constexpr F32 GUST_WINDY = 8.f;         // m/s: from this on, gusts at full depth

    // The effects' share of the drift
    constexpr F64 FOG_DRIFT = 1.0;          // fog patches move with the wind
    constexpr F64 DUST_DRIFT = 0.15;        // dust floats in sheltered air (sunbeams, rooms): much slower

    // Height fog density: gusts of +-12% at strength 1, and the drifting noise's depth
    constexpr F32 FOG_GUST_DEPTH = 0.12f;
    constexpr F32 FOG_NOISE_DEPTH = 0.2f;
    constexpr F64 FOG_NOISE_PERIOD = 256.0; // cells after which the shader's noise repeats

    struct Internal
    {
        FSWindField::State mState;
        LLVector3d mLastCamera;
        F64 mTime = 0.0;            // seconds while active, for the gust noise
        F32 mLogTimer = 0.f;
        bool mHaveCamera = false;
        bool mSnap = true;          // take the next measured wind as it is, without easing
    };
    Internal sInternal;

    // Integer hash to [-1, 1]
    F32 hash_signed(S64 i, U32 seed)
    {
        U32 h = (U32)(i & 0xffffffff) * 0x9e3779b1u ^ seed;
        h ^= h >> 16;
        h *= 0x7feb352du;
        h ^= h >> 15;
        h *= 0x846ca68bu;
        h ^= h >> 16;
        return (F32)(h >> 8) * (2.f / 16777216.f) - 1.f;
    }

    // Smooth 1D value noise, -1 to 1
    F32 value_noise(F64 t, U32 seed)
    {
        const F64 cell = floor(t);
        const F32 f = (F32)(t - cell);
        const F32 w = f * f * (3.f - 2.f * f);
        const S64 i = (S64)cell;
        return lerp(hash_signed(i, seed), hash_signed(i + 1, seed), w);
    }

    // Wind at the camera: the plain average of the points around it that lie over a region
    LLVector3 measure_wind(const LLVector3d& camera)
    {
        // getVelocity reads the agent's region for the region width
        if (!gAgent.getRegion())
        {
            return LLVector3::zero;
        }

        static const F32 offsets[5][2] = {
            { 0.f, 0.f }, { SAMPLE_SPACING, 0.f }, { -SAMPLE_SPACING, 0.f }, { 0.f, SAMPLE_SPACING }, { 0.f, -SAMPLE_SPACING } };
        LLWorld* world = LLWorld::getInstance();
        LLVector3 sum;
        S32 count = 0;
        for (const auto& offset : offsets)
        {
            LLVector3d point = camera;
            point.mdV[VX] += offset[0];
            point.mdV[VY] += offset[1];
            LLVector3 local;
            LLViewerRegion* region = world->resolveRegionGlobal(local, point);
            if (region)
            {
                // getVelocity maps the position onto the wind grid by the agent's region's width: a
                // neighbour of another width (OpenSim var regions) gets its position scaled to match
                const F32 width = region->getWidth();
                const F32 agent_width = gAgent.getRegion()->getWidth();
                if (width > 0.f && agent_width > 0.f && width != agent_width)
                {
                    local.mV[VX] *= agent_width / width;
                    local.mV[VY] *= agent_width / width;
                }
                sum += region->mWind.getVelocity(local);
                ++count;
            }
        }
        if (count == 0)
        {
            return LLVector3::zero;
        }
        sum /= (F32)count;
        sum.mV[VZ] = 0.f;
        return sum;
    }
}

// static
void FSWindField::update()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "FSRenderWindEffects", false);
    static LLCachedControl<F32> strength(gSavedSettings, "FSRenderWindEffectsStrength", 1.f);
    static LLCachedControl<bool> stats(gSavedSettings, "FSRenderWindEffectsStats", false);

    Internal& in = sInternal;
    State& s = in.mState;

    if (!enabled || !gAgent.getRegion())
    {
        // Everything zero, so the effects run exactly as without the option
        if (s.mActive || s.mDrift != LLVector3d::zero)
        {
            const U32 resets = s.mResets;
            s = State();
            s.mResets = resets;
        }
        in.mHaveCamera = false;
        in.mSnap = true;
        return;
    }

    const F32 dt = llclamp(LLFrameTimer::getFrameDeltaTimeF32(), 0.f, MAX_STEP);
    const LLVector3d camera = gAgent.getPosGlobalFromAgent(LLViewerCamera::getInstance()->getOrigin());

    // A teleport or a camera jump: the old wind belongs to another place
    if (gAgent.getTeleportState() != LLAgent::TELEPORT_NONE ||
        (in.mHaveCamera && dist_vec_squared(camera, in.mLastCamera) > JUMP_DISTANCE * JUMP_DISTANCE))
    {
        in.mSnap = true;
    }
    in.mLastCamera = camera;
    in.mHaveCamera = true;

    LLVector3 target = measure_wind(camera);
    const F32 speed = target.length();
    if (speed > MAX_SPEED)
    {
        target *= MAX_SPEED / speed;
    }

    if (in.mSnap)
    {
        // Wait out the teleport, then start from the wind where we landed
        if (gAgent.getTeleportState() == LLAgent::TELEPORT_NONE)
        {
            s.mVelocity = target;
            in.mSnap = false;
            ++s.mResets;
        }
    }
    else
    {
        // Crossings, a wind change, or no region under the camera (target zero): eased
        const F32 k = 1.f - expf(-dt / EASE_TIME);
        s.mVelocity += (target - s.mVelocity) * k;
    }

    s.mActive = true;
    s.mStrength = llclamp((F32)strength, 0.f, 2.f);
    s.mDrift += LLVector3d(s.mVelocity) * ((F64)s.mStrength * (F64)dt);

    // Gusts: slow noise, deeper in stronger wind
    in.mTime += dt;
    const F32 noise = llclamp(0.65f * value_noise(in.mTime / GUST_PERIOD, 0x51ed270bu) +
                              0.35f * value_noise(in.mTime / GUST_PERIOD_FAST, 0x2545f491u), -1.f, 1.f);
    const F32 ramp_up = llclamp((s.mVelocity.length() - GUST_CALM) / (GUST_WINDY - GUST_CALM), 0.f, 1.f);
    const F32 windy = ramp_up * ramp_up * (3.f - 2.f * ramp_up);
    s.mGustSwing = windy * noise;
    s.mGust = windy * (0.5f + 0.5f * noise);

    if (stats)
    {
        in.mLogTimer += dt;
        if (in.mLogTimer >= LOG_INTERVAL)
        {
            in.mLogTimer = 0.f;
            // Compass bearing the wind blows toward: 0 north, 90 east
            F32 toward = atan2f(s.mVelocity.mV[VX], s.mVelocity.mV[VY]) * RAD_TO_DEG;
            if (toward < 0.f)
            {
                toward += 360.f;
            }
            LL_INFOS("FSWindField") << "Wind: " << s.mVelocity.length() << " m/s toward " << toward
                << " deg (raw " << target.length() << " m/s), gust " << s.mGust
                << ", strength " << s.mStrength << ", fog density x" << fogDensityScale() << LL_ENDL;
        }
    }
    else
    {
        in.mLogTimer = 0.f;
    }
}

// static
const FSWindField::State& FSWindField::get()
{
    return sInternal.mState;
}

// static
F32 FSWindField::fogDensityScale()
{
    const State& s = sInternal.mState;
    // Reflection probes keep the plain density, as they keep the plain pattern (uploadHeightFogUniforms):
    // a probe face captured at a gust's peak would show denser fog than the live view until it is
    // captured again. LLEnvironment marks every program's uniforms for upload again after each probe
    // face and before the main view, so the main view never draws with a probe's value. The water
    // reflection keeps the gust: it is drawn every frame, so it follows the live view, and the
    // programs it binds first in a frame keep their values for the rest of that frame.
    if (!s.mActive || gCubeSnapshot)
    {
        return 1.f;
    }
    return llmax(1.f + FOG_GUST_DEPTH * s.mStrength * s.mGustSwing, 0.5f);
}

// static
LLVector3d FSWindField::dustDriftOffset()
{
    const State& s = sInternal.mState;
    return s.mActive ? s.mDrift * DUST_DRIFT : LLVector3d::zero;
}

// static
void FSWindField::uploadHeightFogUniforms(LLGLSLShader& shader)
{
    if (!LLGLSLShader::sGlobalDefines.count("FS_HEIGHT_FOG"))
    {
        return;
    }

    static LLStaticHashedString sWind("fs_fog_wind");
    static LLStaticHashedString sWindX("fs_fog_wind_x");
    static LLStaticHashedString sWindY("fs_fog_wind_y");

    const State& s = sInternal.mState;
    // Reflection probes keep the plain fog: they are captured now and then, and a pattern frozen
    // into them would not match the moving one
    if (!s.mActive || s.mStrength <= 0.f || gCubeSnapshot || LLPipeline::sReflectionRender)
    {
        shader.uniform4f(sWind, 0.f, 0.f, 0.f, 0.f);
        return;
    }

    // Patches a few times the fog layer's thickness (1 / falloff) across
    static LLCachedControl<F32> falloff(gSavedSettings, "FSRenderHeightFogFalloff", 0.02f);
    const F64 patch = (F64)llclamp(3.f / llmax((F32)falloff, 0.001f), 48.f, 384.f);

    // The camera in noise cells, carried back against the wind, wrapped where the noise repeats
    const LLVector3d camera = gAgent.getPosGlobalFromAgent(LLViewerCamera::getInstance()->getOrigin()) - s.mDrift * FOG_DRIFT;
    F32 origin[2];
    for (S32 i = 0; i < 2; ++i)
    {
        F64 c = fmod(camera.mdV[i] / patch, FOG_NOISE_PERIOD);
        if (c < 0.0)
        {
            c += FOG_NOISE_PERIOD;
        }
        origin[i] = (F32)c;
    }

    // The world's east and north axes in view space: the modelview's first two columns
    const glm::mat4 view = get_current_modelview();
    shader.uniform3f(sWindX, view[0][0], view[0][1], view[0][2]);
    shader.uniform3f(sWindY, view[1][0], view[1][1], view[1][2]);
    shader.uniform4f(sWind, origin[0], origin[1], (F32)(1.0 / patch), FOG_NOISE_DEPTH * llmin(s.mStrength, 1.5f));
}
