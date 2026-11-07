/**
 * @file fsdynamicresolution.cpp
 * @brief Dynamic resolution scaling for the 3D scene (FSRenderDynamicResolution)
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
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

// Design notes
//
// Why discrete reallocation and not a sub-rectangle viewport. Rendering into a corner of
// max-size targets would let the scale change every frame for free, but every full-screen pass
// that samples a screen target derives its texture coordinate from the full-screen triangle
// (vary_fragcoord = pos * 0.5 + 0.5) or from gl_FragCoord / screen_res, and many reconstruct
// view-space position from that coordinate. Each of those (deferred lighting, SSAO / GTAO,
// SSR and its history, water, WBOIT, sun rays, bloom, glow, DoF, FXAA, SMAA, CAS, luminance
// and exposure, lens effects, the RLVa sphere) would need a uv scale, a clamp so bilinear and
// blur taps don't read the unrendered border, and the luminance mip chain would average that
// border into the exposure. That is several dozen shaders, most of them under active change by
// other work. A missed one shows as a smeared or offset image only at some scales.
//
// The viewer already renders the main view below window size correctly when the screen
// targets are allocated smaller (RenderResolutionMultiplier < 1): every pass sizes itself from
// its target. So a step here is the same allocation, made cheaper:
//  - only the screen buffers are released and reallocated (releaseScreenBuffers +
//    allocateScreenBuffer), not the sun / spot shadow maps, which keep their full-resolution
//    size, so shadow quality and the static shadow cache don't change with the scale;
//  - targets that didn't change size early out of LLRenderTarget::allocate.
// The measured cost is logged per change ("DynamicResolution"). It is a few dozen texture and
// FBO creations, typically a few milliseconds: one short hitch, so changes are limited to one
// per interval (3 s by default) with hysteresis and backoff.
//
// Detecting a GPU-bound frame. Two GL_TIMESTAMP queries (glQueryCounter, so they never collide
// with the GL_TIME_ELAPSED queries the shader and avatar profilers use) bracket the scene: from
// before the shadow passes to after renderFinalize. They are read back a few frames later
// without stalling. The CPU time over the same span and the frame interval are measured too.
// When the GPU is the bottleneck it lags the CPU, so the start stamp executes late and the GPU
// span is the GPU's real work: longer than the CPU took to submit it, and most of the frame.
// When the CPU is the bottleneck the GPU idles waiting for commands, its span tracks the CPU
// span, and it is a smaller part of the frame. So:
//   gpu_bound = gpu > 0.75 * frame && (gpu > 1.1 * cpu || gpu > 0.9 * frame)
// Resolution is only lowered when the frame rate is below target, the frame is GPU bound and
// the GPU alone is over budget. It is raised when the GPU time predicted at the next step
// (scaled by the pixel count, which overestimates it) still fits in 80% of the budget, or, as
// a probe, after a long CPU-bound stretch, with a holdoff that doubles if the raise had to be
// undone soon after.

#include "llviewerprecompiledheaders.h"

#include "fsdynamicresolution.h"

#include "llglslshader.h"
#include "llrendertarget.h"
#include "llstartup.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include "llviewerdisplay.h"
#include "llviewershadermgr.h"
#include "llviewerwindow.h"
#include "pipeline.h"

extern bool gCubeSnapshot;

namespace
{
    constexpr F32 SCALE_STEP = 0.1f;
    constexpr F32 SCALE_FLOOR = 0.5f;           // lowest the minimum scale setting may go
    constexpr F32 LOWER_MARGIN = 1.05f;         // frame time over target before lowering
    constexpr F32 RAISE_BUDGET = 0.8f;          // predicted GPU time must fit this much of the budget
    constexpr F64 SUSTAIN_SECONDS = 1.0;        // a condition must hold this long to act
    constexpr F64 SETTLE_SECONDS = 0.5;         // samples ignored after a change
    constexpr F64 MAX_RAISE_HOLDOFF = 60.0;
    constexpr S32 QUERY_SLOTS = 4;              // frames the timestamps may be in flight

    struct TimingSlot
    {
        GLuint  mQuery[2] = { 0, 0 };
        F64     mCPUStart = 0.0;
        F64     mCPUSpan = 0.0;
        U32     mGeneration = 0;
        bool    mPending = false;
    };

    // Controller state. All of it is touched on the render thread only.
    F32 sScale = 1.f;               // scale the main targets should have
    F32 sAppliedScale = 1.f;        // scale they were last allocated with
    U32 sFullResX = 0;              // their size before scaling
    U32 sFullResY = 0;
    S32 sSuspended = 0;             // live SnapshotGuards

    TimingSlot sSlots[QUERY_SLOTS];
    S32 sNextSlot = 0;
    S32 sOpenSlot = -1;             // slot begun this frame, closed by endScene()
    bool sQueriesCreated = false;
    U32 sGeneration = 0;            // bumped on every change, older samples are dropped

    F64 sLastFrameStart = 0.0;
    F64 sLastChange = 0.0;
    F64 sLastRaise = -1000.0;
    F64 sRaiseHoldoff = 0.0;
    F64 sLowerSince = -1.0;
    F64 sRaiseSince = -1.0;
    F64 sCPUBoundSince = -1.0;

    // Smoothed, in milliseconds
    F32 sGPUms = 0.f;
    F32 sCPUms = 0.f;
    F32 sFrameMs = 0.f;
    U32 sSamples = 0;

    F64 now_seconds()
    {
        return LLTimer::getTotalSeconds().value();
    }

    void reset_smoothing()
    {
        sSamples = 0;
        sGPUms = sCPUms = sFrameMs = 0.f;
        sLowerSince = sRaiseSince = sCPUBoundSince = -1.0;
    }

    void release_queries()
    {
        if (sQueriesCreated)
        {
            for (TimingSlot& slot : sSlots)
            {
                glDeleteQueries(2, slot.mQuery);
                slot = TimingSlot();
            }
            sQueriesCreated = false;
        }
        sOpenSlot = -1;
        sNextSlot = 0;
        reset_smoothing();
    }

    bool timing_supported()
    {
        return glQueryCounter != nullptr && glGetQueryObjectui64v != nullptr;
    }

    // Collects finished timestamp pairs without stalling and feeds the averages.
    void poll_queries()
    {
        for (TimingSlot& slot : sSlots)
        {
            if (!slot.mPending)
            {
                continue;
            }
            GLuint available = GL_FALSE;
            glGetQueryObjectuiv(slot.mQuery[1], GL_QUERY_RESULT_AVAILABLE, &available);
            if (!available)
            {
                continue;
            }
            GLuint64 t0 = 0;
            GLuint64 t1 = 0;
            glGetQueryObjectui64v(slot.mQuery[0], GL_QUERY_RESULT, &t0);
            glGetQueryObjectui64v(slot.mQuery[1], GL_QUERY_RESULT, &t1);
            slot.mPending = false;
            if (slot.mGeneration != sGeneration || t1 <= t0)
            {
                continue;
            }
            const F32 gpu_ms = (F32)((F64)(t1 - t0) / 1000000.0);
            const F32 cpu_ms = (F32)(slot.mCPUSpan * 1000.0);
            const F32 k = sSamples < 8 ? 1.f / (F32)(sSamples + 1) : 0.08f;
            sGPUms += (gpu_ms - sGPUms) * k;
            sCPUms += (cpu_ms - sCPUms) * k;
            ++sSamples;
        }
    }

    F32 target_fps()
    {
        static LLCachedControl<F32> target(gSavedSettings, "FSRenderDynamicResolutionTargetFPS", 0.f);
        F32 fps = target;
        if (fps <= 0.f)
        {
            static LLCachedControl<bool> limit_framerate(gSavedSettings, "FSLimitFramerate", false);
            static LLCachedControl<U32> max_fps(gSavedSettings, "FramePerSecondLimit", 0);
            fps = (limit_framerate && max_fps > 0) ? (F32)max_fps : 60.f;
        }
        return llclamp(fps, 15.f, 240.f);
    }

    F32 min_scale()
    {
        static LLCachedControl<F32> min_scale(gSavedSettings, "FSRenderDynamicResolutionMinScale", 0.6f);
        return llclamp((F32)min_scale, SCALE_FLOOR, 1.f);
    }

    F64 change_interval()
    {
        static LLCachedControl<F32> interval(gSavedSettings, "FSRenderDynamicResolutionInterval", 3.f);
        return llclamp((F64)interval, 1.0, 30.0);
    }

    // A manual resolution setting owns the screen size, and the deferred renderer must be up.
    bool allowed()
    {
        return LLPipeline::RenderResolutionDivisor <= 1 &&
               (LLPipeline::RenderResolutionMultiplier <= 0.f || LLPipeline::RenderResolutionMultiplier == 1.f) &&
               LLPipeline::sRenderDeferred &&
               gPipeline.shadersLoaded() &&
               gPipeline.mRT == &gPipeline.mMainRT;
    }

    // Reallocates the main view's screen targets at the new scale. The shadow maps are left
    // alone: scaleMainTarget() keeps their base size at full resolution, so they early out.
    void apply_scale(F32 scale, const char* reason)
    {
        scale = llclamp(scale, SCALE_FLOOR, 1.f);
        if (scale == sScale)
        {
            return;
        }
        const F32 old_scale = sScale;
        sScale = scale;
        sLastChange = now_seconds();
        ++sGeneration;
        reset_smoothing();

        if (gPipeline.mRT != &gPipeline.mMainRT || gPipeline.mRT->width == 0 || gPipeline.mRT->height == 0)
        {
            return; // picked up by the next allocation
        }

        const U32 width = gPipeline.mRT->width;
        const U32 height = gPipeline.mRT->height;
        LLTimer timer;
        gPipeline.releaseScreenBuffers();
        gPipeline.allocateScreenBuffer(width, height);
        LL_INFOS("DynamicResolution") << "Scene scale " << old_scale << " -> " << sScale << " (" << reason << "), "
                                      << gPipeline.mRT->screen.getWidth() << "x" << gPipeline.mRT->screen.getHeight()
                                      << ", reallocated in " << timer.getElapsedTimeF32() * 1000.f << " ms" << LL_ENDL;
    }

    void run_controller(F64 now)
    {
        const F32 minimum = min_scale();
        if (sScale < minimum)
        {
            apply_scale(minimum, "minimum raised");
            return;
        }

        if (sSamples < 8 || now - sLastChange < SETTLE_SECONDS)
        {
            return;
        }

        const F64 interval = change_interval();
        if (sRaiseHoldoff < interval)
        {
            sRaiseHoldoff = interval;
        }
        else if (now - sLastChange > 10.0 * interval)
        {
            sRaiseHoldoff = interval; // stable for a long time, forget earlier oscillation
        }

        const F32 budget_ms = 1000.f / target_fps();
        const bool gpu_bound = sGPUms > 0.75f * sFrameMs && (sGPUms > 1.1f * sCPUms || sGPUms > 0.9f * sFrameMs);

        // Lower: too slow, and the GPU alone is what can't keep up
        const bool want_lower = sScale > minimum && sFrameMs > budget_ms * LOWER_MARGIN && gpu_bound &&
                                sGPUms > budget_ms * 0.9f;
        sLowerSince = want_lower ? (sLowerSince < 0.0 ? now : sLowerSince) : -1.0;
        if (want_lower && now - sLowerSince >= SUSTAIN_SECONDS && now - sLastChange >= interval)
        {
            // Pixel count proportional estimate of the scale that fits, at most two steps at once
            const F32 fit = sScale * sqrtf(budget_ms * 0.9f / llmax(sGPUms, 0.1f));
            F32 next = llclamp(fit, sScale - 2.f * SCALE_STEP, sScale - SCALE_STEP);
            next = llmax(minimum, floorf(next / SCALE_STEP + 0.01f) * SCALE_STEP);
            if (now - sLastRaise < 3.0 * interval)
            {
                sRaiseHoldoff = llmin(sRaiseHoldoff * 2.0, MAX_RAISE_HOLDOFF); // the last raise didn't hold
            }
            apply_scale(next, "GPU bound");
            return;
        }

        if (sScale >= 1.f)
        {
            return;
        }

        // Raise: the GPU will still have headroom one step up
        const F32 next = llmin(1.f, sScale + SCALE_STEP);
        const F32 ratio = next / sScale;
        const bool headroom = sGPUms * ratio * ratio < budget_ms * RAISE_BUDGET;
        sRaiseSince = headroom ? (sRaiseSince < 0.0 ? now : sRaiseSince) : -1.0;

        // A CPU-bound frame hides the GPU's real cost (its span follows the CPU), so a long
        // CPU-bound stretch also earns a raise, under the backoff.
        sCPUBoundSince = !gpu_bound ? (sCPUBoundSince < 0.0 ? now : sCPUBoundSince) : -1.0;
        const bool cpu_probe = !gpu_bound && now - sCPUBoundSince >= 4.0 * sRaiseHoldoff;

        if (now - sLastChange >= sRaiseHoldoff &&
            ((headroom && now - sRaiseSince >= SUSTAIN_SECONDS) || cpu_probe))
        {
            sLastRaise = now;
            apply_scale(next, headroom ? "GPU headroom" : "CPU bound");
        }
    }

    // NVIDIA Image Scaling constants: NVScalerUpdateConfig() from NIS_Config.h (NIS SDK v1.0.3,
    // MIT license, Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES) in SDR mode, the input
    // being tone mapped and gamma corrected. The scale and texel size terms are not needed:
    // the shader derives the source position from the present's texture coordinate.
    // coef_scale and coef_usm from NIS_Config.h, 64 phases of 6 taps: phase p is taps 0..2 at
    // [2p] and taps 3..5 at [2p + 1] of the shader's vec3 arrays. Uniforms rather than const
    // arrays in fsNISUpscaleF.glsl, which AMD drivers took about 15 s to load.
    const F32 NIS_COEF_SCALER[128 * 3] =
    {
        0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f,
        0.0029f, -0.0127f, 1.0000f, 0.0132f, -0.0034f, 0.0000f,
        0.0063f, -0.0249f, 0.9985f, 0.0269f, -0.0068f, 0.0000f,
        0.0088f, -0.0361f, 0.9956f, 0.0415f, -0.0103f, 0.0005f,
        0.0117f, -0.0474f, 0.9932f, 0.0562f, -0.0142f, 0.0005f,
        0.0142f, -0.0576f, 0.9897f, 0.0713f, -0.0181f, 0.0005f,
        0.0166f, -0.0674f, 0.9844f, 0.0874f, -0.0220f, 0.0010f,
        0.0186f, -0.0762f, 0.9785f, 0.1040f, -0.0264f, 0.0015f,
        0.0205f, -0.0850f, 0.9727f, 0.1206f, -0.0308f, 0.0020f,
        0.0225f, -0.0928f, 0.9648f, 0.1382f, -0.0352f, 0.0024f,
        0.0239f, -0.1006f, 0.9575f, 0.1558f, -0.0396f, 0.0029f,
        0.0254f, -0.1074f, 0.9487f, 0.1738f, -0.0439f, 0.0034f,
        0.0264f, -0.1138f, 0.9390f, 0.1929f, -0.0488f, 0.0044f,
        0.0278f, -0.1191f, 0.9282f, 0.2119f, -0.0537f, 0.0049f,
        0.0288f, -0.1245f, 0.9170f, 0.2310f, -0.0581f, 0.0059f,
        0.0293f, -0.1294f, 0.9058f, 0.2510f, -0.0630f, 0.0063f,
        0.0303f, -0.1333f, 0.8926f, 0.2710f, -0.0679f, 0.0073f,
        0.0308f, -0.1367f, 0.8789f, 0.2915f, -0.0728f, 0.0083f,
        0.0308f, -0.1401f, 0.8657f, 0.3120f, -0.0776f, 0.0093f,
        0.0313f, -0.1426f, 0.8506f, 0.3330f, -0.0825f, 0.0103f,
        0.0313f, -0.1445f, 0.8354f, 0.3540f, -0.0874f, 0.0112f,
        0.0313f, -0.1460f, 0.8193f, 0.3755f, -0.0923f, 0.0122f,
        0.0313f, -0.1470f, 0.8022f, 0.3965f, -0.0967f, 0.0137f,
        0.0308f, -0.1479f, 0.7856f, 0.4185f, -0.1016f, 0.0146f,
        0.0303f, -0.1479f, 0.7681f, 0.4399f, -0.1060f, 0.0156f,
        0.0298f, -0.1479f, 0.7505f, 0.4614f, -0.1104f, 0.0166f,
        0.0293f, -0.1470f, 0.7314f, 0.4829f, -0.1147f, 0.0181f,
        0.0288f, -0.1460f, 0.7119f, 0.5049f, -0.1187f, 0.0190f,
        0.0278f, -0.1445f, 0.6929f, 0.5264f, -0.1226f, 0.0200f,
        0.0273f, -0.1431f, 0.6724f, 0.5479f, -0.1260f, 0.0215f,
        0.0264f, -0.1411f, 0.6528f, 0.5693f, -0.1299f, 0.0225f,
        0.0254f, -0.1387f, 0.6323f, 0.5903f, -0.1328f, 0.0234f,
        0.0244f, -0.1357f, 0.6113f, 0.6113f, -0.1357f, 0.0244f,
        0.0234f, -0.1328f, 0.5903f, 0.6323f, -0.1387f, 0.0254f,
        0.0225f, -0.1299f, 0.5693f, 0.6528f, -0.1411f, 0.0264f,
        0.0215f, -0.1260f, 0.5479f, 0.6724f, -0.1431f, 0.0273f,
        0.0200f, -0.1226f, 0.5264f, 0.6929f, -0.1445f, 0.0278f,
        0.0190f, -0.1187f, 0.5049f, 0.7119f, -0.1460f, 0.0288f,
        0.0181f, -0.1147f, 0.4829f, 0.7314f, -0.1470f, 0.0293f,
        0.0166f, -0.1104f, 0.4614f, 0.7505f, -0.1479f, 0.0298f,
        0.0156f, -0.1060f, 0.4399f, 0.7681f, -0.1479f, 0.0303f,
        0.0146f, -0.1016f, 0.4185f, 0.7856f, -0.1479f, 0.0308f,
        0.0137f, -0.0967f, 0.3965f, 0.8022f, -0.1470f, 0.0313f,
        0.0122f, -0.0923f, 0.3755f, 0.8193f, -0.1460f, 0.0313f,
        0.0112f, -0.0874f, 0.3540f, 0.8354f, -0.1445f, 0.0313f,
        0.0103f, -0.0825f, 0.3330f, 0.8506f, -0.1426f, 0.0313f,
        0.0093f, -0.0776f, 0.3120f, 0.8657f, -0.1401f, 0.0308f,
        0.0083f, -0.0728f, 0.2915f, 0.8789f, -0.1367f, 0.0308f,
        0.0073f, -0.0679f, 0.2710f, 0.8926f, -0.1333f, 0.0303f,
        0.0063f, -0.0630f, 0.2510f, 0.9058f, -0.1294f, 0.0293f,
        0.0059f, -0.0581f, 0.2310f, 0.9170f, -0.1245f, 0.0288f,
        0.0049f, -0.0537f, 0.2119f, 0.9282f, -0.1191f, 0.0278f,
        0.0044f, -0.0488f, 0.1929f, 0.9390f, -0.1138f, 0.0264f,
        0.0034f, -0.0439f, 0.1738f, 0.9487f, -0.1074f, 0.0254f,
        0.0029f, -0.0396f, 0.1558f, 0.9575f, -0.1006f, 0.0239f,
        0.0024f, -0.0352f, 0.1382f, 0.9648f, -0.0928f, 0.0225f,
        0.0020f, -0.0308f, 0.1206f, 0.9727f, -0.0850f, 0.0205f,
        0.0015f, -0.0264f, 0.1040f, 0.9785f, -0.0762f, 0.0186f,
        0.0010f, -0.0220f, 0.0874f, 0.9844f, -0.0674f, 0.0166f,
        0.0005f, -0.0181f, 0.0713f, 0.9897f, -0.0576f, 0.0142f,
        0.0005f, -0.0142f, 0.0562f, 0.9932f, -0.0474f, 0.0117f,
        0.0005f, -0.0103f, 0.0415f, 0.9956f, -0.0361f, 0.0088f,
        0.0000f, -0.0068f, 0.0269f, 0.9985f, -0.0249f, 0.0063f,
        0.0000f, -0.0034f, 0.0132f, 1.0000f, -0.0127f, 0.0029f,
    };

    const F32 NIS_COEF_USM[128 * 3] =
    {
        0.0000f, -0.6001f, 1.2002f, -0.6001f, 0.0000f, 0.0000f,
        0.0029f, -0.6084f, 1.1987f, -0.5903f, -0.0029f, 0.0000f,
        0.0049f, -0.6147f, 1.1958f, -0.5791f, -0.0068f, 0.0005f,
        0.0073f, -0.6196f, 1.1890f, -0.5659f, -0.0103f, 0.0000f,
        0.0093f, -0.6235f, 1.1802f, -0.5513f, -0.0151f, 0.0000f,
        0.0112f, -0.6265f, 1.1699f, -0.5352f, -0.0195f, 0.0005f,
        0.0122f, -0.6270f, 1.1582f, -0.5181f, -0.0259f, 0.0005f,
        0.0142f, -0.6284f, 1.1455f, -0.5005f, -0.0317f, 0.0005f,
        0.0156f, -0.6265f, 1.1274f, -0.4790f, -0.0386f, 0.0005f,
        0.0166f, -0.6235f, 1.1089f, -0.4570f, -0.0454f, 0.0010f,
        0.0176f, -0.6187f, 1.0879f, -0.4346f, -0.0532f, 0.0010f,
        0.0181f, -0.6138f, 1.0659f, -0.4102f, -0.0615f, 0.0015f,
        0.0190f, -0.6069f, 1.0405f, -0.3843f, -0.0698f, 0.0015f,
        0.0195f, -0.6006f, 1.0161f, -0.3574f, -0.0796f, 0.0020f,
        0.0200f, -0.5928f, 0.9893f, -0.3286f, -0.0898f, 0.0024f,
        0.0200f, -0.5820f, 0.9580f, -0.2988f, -0.1001f, 0.0029f,
        0.0200f, -0.5728f, 0.9292f, -0.2690f, -0.1104f, 0.0034f,
        0.0200f, -0.5620f, 0.8975f, -0.2368f, -0.1226f, 0.0039f,
        0.0205f, -0.5498f, 0.8643f, -0.2046f, -0.1343f, 0.0044f,
        0.0200f, -0.5371f, 0.8301f, -0.1709f, -0.1465f, 0.0049f,
        0.0195f, -0.5239f, 0.7944f, -0.1367f, -0.1587f, 0.0054f,
        0.0195f, -0.5107f, 0.7598f, -0.1021f, -0.1724f, 0.0059f,
        0.0190f, -0.4966f, 0.7231f, -0.0649f, -0.1865f, 0.0063f,
        0.0186f, -0.4819f, 0.6846f, -0.0288f, -0.1997f, 0.0068f,
        0.0186f, -0.4668f, 0.6460f, 0.0093f, -0.2144f, 0.0073f,
        0.0176f, -0.4507f, 0.6055f, 0.0479f, -0.2290f, 0.0083f,
        0.0171f, -0.4370f, 0.5693f, 0.0859f, -0.2446f, 0.0088f,
        0.0161f, -0.4199f, 0.5283f, 0.1255f, -0.2598f, 0.0098f,
        0.0161f, -0.4048f, 0.4883f, 0.1655f, -0.2754f, 0.0103f,
        0.0151f, -0.3887f, 0.4497f, 0.2041f, -0.2910f, 0.0107f,
        0.0142f, -0.3711f, 0.4072f, 0.2446f, -0.3066f, 0.0117f,
        0.0137f, -0.3555f, 0.3672f, 0.2852f, -0.3228f, 0.0122f,
        0.0132f, -0.3394f, 0.3262f, 0.3262f, -0.3394f, 0.0132f,
        0.0122f, -0.3228f, 0.2852f, 0.3672f, -0.3555f, 0.0137f,
        0.0117f, -0.3066f, 0.2446f, 0.4072f, -0.3711f, 0.0142f,
        0.0107f, -0.2910f, 0.2041f, 0.4497f, -0.3887f, 0.0151f,
        0.0103f, -0.2754f, 0.1655f, 0.4883f, -0.4048f, 0.0161f,
        0.0098f, -0.2598f, 0.1255f, 0.5283f, -0.4199f, 0.0161f,
        0.0088f, -0.2446f, 0.0859f, 0.5693f, -0.4370f, 0.0171f,
        0.0083f, -0.2290f, 0.0479f, 0.6055f, -0.4507f, 0.0176f,
        0.0073f, -0.2144f, 0.0093f, 0.6460f, -0.4668f, 0.0186f,
        0.0068f, -0.1997f, -0.0288f, 0.6846f, -0.4819f, 0.0186f,
        0.0063f, -0.1865f, -0.0649f, 0.7231f, -0.4966f, 0.0190f,
        0.0059f, -0.1724f, -0.1021f, 0.7598f, -0.5107f, 0.0195f,
        0.0054f, -0.1587f, -0.1367f, 0.7944f, -0.5239f, 0.0195f,
        0.0049f, -0.1465f, -0.1709f, 0.8301f, -0.5371f, 0.0200f,
        0.0044f, -0.1343f, -0.2046f, 0.8643f, -0.5498f, 0.0205f,
        0.0039f, -0.1226f, -0.2368f, 0.8975f, -0.5620f, 0.0200f,
        0.0034f, -0.1104f, -0.2690f, 0.9292f, -0.5728f, 0.0200f,
        0.0029f, -0.1001f, -0.2988f, 0.9580f, -0.5820f, 0.0200f,
        0.0024f, -0.0898f, -0.3286f, 0.9893f, -0.5928f, 0.0200f,
        0.0020f, -0.0796f, -0.3574f, 1.0161f, -0.6006f, 0.0195f,
        0.0015f, -0.0698f, -0.3843f, 1.0405f, -0.6069f, 0.0190f,
        0.0015f, -0.0615f, -0.4102f, 1.0659f, -0.6138f, 0.0181f,
        0.0010f, -0.0532f, -0.4346f, 1.0879f, -0.6187f, 0.0176f,
        0.0010f, -0.0454f, -0.4570f, 1.1089f, -0.6235f, 0.0166f,
        0.0005f, -0.0386f, -0.4790f, 1.1274f, -0.6265f, 0.0156f,
        0.0005f, -0.0317f, -0.5005f, 1.1455f, -0.6284f, 0.0142f,
        0.0005f, -0.0259f, -0.5181f, 1.1582f, -0.6270f, 0.0122f,
        0.0005f, -0.0195f, -0.5352f, 1.1699f, -0.6265f, 0.0112f,
        0.0000f, -0.0151f, -0.5513f, 1.1802f, -0.6235f, 0.0093f,
        0.0000f, -0.0103f, -0.5659f, 1.1890f, -0.6196f, 0.0073f,
        0.0005f, -0.0068f, -0.5791f, 1.1958f, -0.6147f, 0.0049f,
        0.0000f, -0.0029f, -0.5903f, 1.1987f, -0.6084f, 0.0029f,
    };

    void setup_nis(LLGLSLShader& shader, F32 sharpness)
    {
        // adjust params based on value from sharpness slider
        sharpness = llclamp(sharpness, 0.f, 1.f);
        const F32 sharpen_slider = sharpness - 0.5f; // Map 0 to 1 to -0.5 to +0.5

        // Different range for 0 to 50% vs 50% to 100%. Sharpness 0 maps to no sharpening,
        // while 1 doesn't oversharpen.
        const F32 MaxScale = (sharpen_slider >= 0.0f) ? 1.25f : 1.75f;
        const F32 MinScale = (sharpen_slider >= 0.0f) ? 1.25f : 1.0f;
        const F32 LimitScale = (sharpen_slider >= 0.0f) ? 1.25f : 1.0f;

        const F32 kDetectRatio = 2.f * 1127.f / 1024.f;

        // Params for SDR
        const F32 kDetectThres = 64.0f / 1024.0f;
        const F32 kMinContrastRatio = 2.0f;
        const F32 kMaxContrastRatio = 10.0f;

        const F32 kSharpStartY = 0.45f;
        const F32 kSharpEndY = 0.9f;
        const F32 kSharpStrengthMin = llmax(0.0f, 0.4f + sharpen_slider * MinScale * 1.2f);
        const F32 kSharpStrengthMax = 1.6f + sharpen_slider * MaxScale * 1.8f;
        const F32 kSharpLimitMin = llmax(0.1f, 0.14f + sharpen_slider * LimitScale * 0.32f);
        const F32 kSharpLimitMax = 0.5f + sharpen_slider * LimitScale * 0.6f;

        const F32 kRatioNorm = 1.0f / (kMaxContrastRatio - kMinContrastRatio);
        const F32 kSharpScaleY = 1.0f / (kSharpEndY - kSharpStartY);
        const F32 kSharpStrengthScale = kSharpStrengthMax - kSharpStrengthMin;
        const F32 kSharpLimitScale = kSharpLimitMax - kSharpLimitMin;
        const F32 kContrastBoost = 1.0f;
        const F32 kEps = 1.0f / 255.0f;

        static LLStaticHashedString s_detect("nis_detect");
        static LLStaticHashedString s_sharp_y("nis_sharp_y");
        static LLStaticHashedString s_sharp("nis_sharp");
        shader.uniform4f(s_detect, kDetectRatio, kDetectThres, kMinContrastRatio, kRatioNorm);
        shader.uniform4f(s_sharp_y, kContrastBoost, kEps, kSharpStartY, kSharpScaleY);
        shader.uniform4f(s_sharp, kSharpStrengthMin, kSharpStrengthScale, kSharpLimitMin, kSharpLimitScale);

        // Set every present (3 KB): a reloaded program starts with them zeroed
        static LLStaticHashedString s_coef_scaler("nis_coef_scaler");
        static LLStaticHashedString s_coef_usm("nis_coef_usm");
        shader.uniform3fv(s_coef_scaler, 128, NIS_COEF_SCALER);
        shader.uniform3fv(s_coef_usm, 128, NIS_COEF_USM);
    }
}

// static
void FSDynamicResolution::beginScene(bool for_snapshot)
{
    static LLCachedControl<bool> enabled(gSavedSettings, "FSRenderDynamicResolution", false);
    if (!enabled && sScale == 1.f && !sQueriesCreated)
    {
        return; // off: nothing else runs
    }

    sOpenSlot = -1;
    if (for_snapshot || gCubeSnapshot || sSuspended > 0)
    {
        return;
    }

    if (!enabled || !allowed() || !timing_supported())
    {
        release_queries();
        if (gPipeline.mRT == &gPipeline.mMainRT)
        {
            apply_scale(1.f, "off");
        }
        return;
    }

    const F64 now = now_seconds();
    const F64 frame = now - sLastFrameStart;
    sLastFrameStart = now;

    if (!sQueriesCreated)
    {
        for (TimingSlot& slot : sSlots)
        {
            glGenQueries(2, slot.mQuery);
        }
        sQueriesCreated = true;
        sLastChange = now;
    }

    poll_queries();

    // Login, teleport and long hitches say nothing about the GPU
    if (LLStartUp::getStartupState() != STATE_STARTED || gTeleportDisplay || frame <= 0.0 || frame > 0.5)
    {
        reset_smoothing();
    }
    else
    {
        const F32 frame_ms = (F32)(frame * 1000.0);
        sFrameMs = sSamples == 0 ? frame_ms : sFrameMs + (frame_ms - sFrameMs) * 0.08f;
        run_controller(now);
    }

    // Open this frame's timing, unless every slot is still in flight
    TimingSlot& slot = sSlots[sNextSlot];
    if (!slot.mPending)
    {
        glQueryCounter(slot.mQuery[0], GL_TIMESTAMP);
        slot.mCPUStart = now_seconds();
        slot.mGeneration = sGeneration;
        sOpenSlot = sNextSlot;
    }
}

// static
void FSDynamicResolution::endScene()
{
    if (sOpenSlot < 0)
    {
        return;
    }
    TimingSlot& slot = sSlots[sOpenSlot];
    glQueryCounter(slot.mQuery[1], GL_TIMESTAMP);
    slot.mCPUSpan = now_seconds() - slot.mCPUStart;
    slot.mPending = true;
    sNextSlot = (sOpenSlot + 1) % QUERY_SLOTS;
    sOpenSlot = -1;
}

// static
void FSDynamicResolution::scaleMainTarget(U32& res_x, U32& res_y)
{
    sFullResX = res_x;
    sFullResY = res_y;
    sAppliedScale = sSuspended > 0 ? 1.f : sScale;
    if (sAppliedScale < 1.f)
    {
        res_x = llmax((U32)((F32)res_x * sAppliedScale + 0.5f), 16U);
        res_y = llmax((U32)((F32)res_y * sAppliedScale + 0.5f), 16U);
    }
}

// static
void FSDynamicResolution::shadowBaseSize(U32& res_x, U32& res_y)
{
    if (sAppliedScale < 1.f && sFullResX > 0 && sFullResY > 0)
    {
        res_x = sFullResX;
        res_y = sFullResY;
    }
}

// static
bool FSDynamicResolution::nisPresents(const LLRenderTarget* src, S32 out_width, S32 out_height)
{
    static LLCachedControl<S32> upscaler(gSavedSettings, "FSRenderUpscaler", 0);
    return upscaler == 1 && src && gFSNISUpscaleProgram.isComplete() &&
           ((S32)src->getWidth() < out_width || (S32)src->getHeight() < out_height);
}

// static
bool FSDynamicResolution::upscalerReplacesCAS(const LLRenderTarget* src)
{
    // renderFinalize() asks before the present viewport is set, so compare with the world view
    // rectangle that viewport is made from
    const LLRect& rect = gViewerWindow->getWorldViewRectRaw();
    return nisPresents(src, rect.getWidth(), rect.getHeight());
}

// static
LLGLSLShader& FSDynamicResolution::presentShader(LLGLSLShader& fallback, const LLRenderTarget* src)
{
    // NIS takes every upscaled present: dynamic resolution, RenderResolutionMultiplier below 1
    // and RenderResolutionDivisor
    if (nisPresents(src, gGLViewport[2], gGLViewport[3]))
    {
        return gFSNISUpscaleProgram;
    }
    if (sAppliedScale < 1.f && src && gFSDynResUpscaleProgram.isComplete() &&
        ((S32)src->getWidth() < gGLViewport[2] || (S32)src->getHeight() < gGLViewport[3]))
    {
        return gFSDynResUpscaleProgram;
    }
    return fallback;
}

// static
void FSDynamicResolution::setupPresent(LLGLSLShader& shader)
{
    static LLCachedControl<F32> sharpness(gSavedSettings, "FSRenderDynamicResolutionSharpness", 0.5f);
    if (&shader == &gFSNISUpscaleProgram)
    {
        setup_nis(shader, sharpness);
        return;
    }
    if (&shader != &gFSDynResUpscaleProgram)
    {
        return;
    }
    static LLStaticHashedString s_sharpness("fs_dynres_sharpness");
    shader.uniform1f(s_sharpness, llclamp((F32)sharpness, 0.f, 1.f));
}

// static
F32 FSDynamicResolution::getScale()
{
    return sAppliedScale;
}

FSDynamicResolution::SnapshotGuard::SnapshotGuard()
{
    if (sSuspended++ == 0 && sScale < 1.f)
    {
        apply_scale(1.f, "snapshot");
    }
}

FSDynamicResolution::SnapshotGuard::~SnapshotGuard()
{
    if (--sSuspended == 0)
    {
        sLastChange = now_seconds(); // a full interval at full size before lowering again
        reset_smoothing();
    }
}
