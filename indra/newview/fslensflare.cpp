/**
 * @file fslensflare.cpp
 * @brief High quality lens flare (FSRenderLensFlareHQ), see fslensflare.h.
 *
 * The sun measurement follows the approach of Alchemy Viewer's generateLensFlareState
 * (pipeline.cpp, Copyright (C) 2026, Alchemy Viewer Project, LGPL 2.1): the probe radius from the
 * body's angular size through the field of view and the snapshot zoom, a soft fade over a margin
 * past the frame's edge, a history that is started over when the sun hands over to the moon, and
 * a history that is held, not cleared, through a snapshot without post effects. The code is our own.
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

#include "llviewerprecompiledheaders.h"

#include "fslensflare.h"

#include "llenvironment.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llsettingssky.h"
#include "llshadermgr.h"
#include "llvertexbuffer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "pipeline.h"

LLGLSLShader gFSLensFlareStateProgram;
LLGLSLShader gFSLensFlarePickProgram;
LLGLSLShader gFSLensFlareSelectProgram;

extern bool gSnapshot;
extern bool gSnapshotNoPost;

namespace
{
    constexpr U32 TILES = 32;           // fsLensFlarePickF: 8 x 4
    constexpr U32 KEEP = 8;             // fsLensFlareSelectF
    constexpr F32 RISE_SECONDS = 0.12f; // time constant of the drive coming up
    constexpr F32 FALL_SECONDS = 0.08f; // and going down: an occluder hides the sun a little faster
    constexpr F32 EDGE_MARGIN = 0.2f;   // the flare fades out over this much past the frame's edge
    // Tangent of the sun disc's half angle at a sun scale of 1: the sky draws it on a quad of half
    // size HEAVENLY_BODY_FACTOR * the disc radius (0.1 * 0.5), and sunDiscF fills 1 / 1.1 of it
    constexpr F32 SUN_HALF_TAN = 0.045f;

    // Heap allocated: a static LLRenderTarget would release GL objects after the context is gone
    LLRenderTarget* sState[2] = { nullptr, nullptr };
    U32 sStateIndex = 0;            // the one written last
    bool sStateValid = false;       // the history holds a drive for the body now in the sky
    U32 sStateFrame = 0;            // frame the drive was measured for
    bool sStateSun = true;          // the body the history is for
    LLRenderTarget* sPicks = nullptr;
    LLRenderTarget* sLights = nullptr;
    U32 sLightsFrame = 0;           // frame the lights were picked for

    struct SunScreen
    {
        F32 mU = 0.5f;
        F32 mV = 0.5f;
        F32 mRadius = 0.f;          // disc radius in screen heights
        F32 mFade = 0.f;            // 0 behind the camera or well outside the frame
        bool mSun = true;           // the sun, else the moon
        bool mUp = false;           // either is in the sky
    };

    SunScreen sun_screen()
    {
        SunScreen s;
        LLEnvironment& environment = LLEnvironment::instance();
        const bool sun_up = environment.getIsSunUp();
        s.mSun = sun_up;
        s.mUp = sun_up || environment.getIsMoonUp();
        if (!s.mUp)
        {
            return s;
        }

        // As renderLensEffects places it. Divided by clip w, which reverse-Z leaves alone.
        const LLVector4& dir = sun_up ? gPipeline.mTransformedSunDir : gPipeline.mTransformedMoonDir;
        const glm::vec4 clip = get_current_projection() * glm::vec4(dir.mV[VX], dir.mV[VY], dir.mV[VZ], 0.f);
        if (clip.w <= 1e-4f)
        {
            return s;
        }
        s.mU = clip.x / clip.w * 0.5f + 0.5f;
        s.mV = clip.y / clip.w * 0.5f + 0.5f;
        s.mFade = llclamp((s.mU + EDGE_MARGIN) / EDGE_MARGIN, 0.f, 1.f) *
                  llclamp((1.f + EDGE_MARGIN - s.mU) / EDGE_MARGIN, 0.f, 1.f) *
                  llclamp((s.mV + EDGE_MARGIN) / EDGE_MARGIN, 0.f, 1.f) *
                  llclamp((1.f + EDGE_MARGIN - s.mV) / EDGE_MARGIN, 0.f, 1.f);
        // Keep the measuring taps on the frame; the fade has taken the flare out by then
        s.mU = llclamp(s.mU, -EDGE_MARGIN, 1.f + EDGE_MARGIN);
        s.mV = llclamp(s.mV, -EDGE_MARGIN, 1.f + EDGE_MARGIN);

        // The disc's size on screen follows the sky's sun (or moon) scale, the field of view and a
        // tiled snapshot's zoom, which narrows the projection without changing the field of view.
        // Capped, so a huge sun at a narrow field of view cannot measure half the frame.
        LLSettingsSky::ptr_t psky = environment.getCurrentSky();
        const F32 body_scale = psky ? llclamp(sun_up ? psky->getSunScale() : psky->getMoonScale(), 0.25f, 20.f) : 1.f;
        const LLViewerCamera* camera = LLViewerCamera::getInstance();
        const F32 fov = llclamp(camera->getView(), 0.01f, F_PI - 0.01f);
        s.mRadius = llmin(0.5f * SUN_HALF_TAN * body_scale / tanf(fov * 0.5f) * llmax(camera->getZoomFactor(), 1.f), 0.3f);
        return s;
    }

    // A tiny float target, cleared once when made: some drivers (AMD) hand out new targets holding
    // whatever was there before, and the state is read back as its own history
    bool ensure_target(LLRenderTarget*& target, U32 width, U32 height, bool& made)
    {
        made = false;
        if (!target)
        {
            target = new LLRenderTarget();
        }
        if (target->isComplete() && target->getWidth() == width && target->getHeight() == height)
        {
            return true;
        }
        target->release();
        if (!target->allocate(width, height, GL_RGBA16F))
        {
            return false;
        }
        target->bindTarget();
        glClearColor(0.f, 0.f, 0.f, 0.f);
        target->clear(GL_COLOR_BUFFER_BIT);
        target->flush();
        made = true;
        return true;
    }

    void free_target(LLRenderTarget*& target)
    {
        if (target)
        {
            target->release();
            delete target;
            target = nullptr;
        }
    }

    void draw_triangle(LLVertexBuffer* tri)
    {
        tri->setBuffer();
        tri->drawArrays(LLRender::TRIANGLES, 0, 3);
    }

    LLStaticHashedString sFlareSun("fs_flare_sun");
    LLStaticHashedString sFlareRate("fs_flare_rate");
    LLStaticHashedString sPickArea("fs_pick_area");
    LLStaticHashedString sExposure("exposure");
    LLStaticHashedString sFlareHQ("flare_hq");
    LLStaticHashedString sFlareHQParams("flare_hq_params");
    LLStaticHashedString sFlareHQSun("flare_hq_sun");
}

bool FSLensFlare::enabled()
{
    static LLCachedControl<bool> hq(gSavedSettings, "FSRenderLensFlareHQ", false);
    static LLCachedControl<F32> strength(gSavedSettings, "FSRenderLensFlare", 0.f);
    return hq && strength > 0.f;
}

void FSLensFlare::pickLights(LLRenderTarget* levels, U32 count, LLRenderTarget& depth, LLVertexBuffer* tri)
{
    static LLCachedControl<F32> lights(gSavedSettings, "FSRenderLensFlareLights", 0.5f);

    sLightsFrame = 0;
    if (!enabled() || lights <= 0.f || count == 0 || !tri ||
        !gFSLensFlarePickProgram.isComplete() || !gFSLensFlareSelectProgram.isComplete())
    {
        free_target(sPicks);
        free_target(sLights);
        return;
    }

    bool made = false;
    if (!ensure_target(sPicks, TILES, 2, made) || !ensure_target(sLights, KEEP, 2, made))
    {
        return;
    }

    LL_PROFILE_GPU_ZONE("lens flare lights");

    // Scan a level about 1/32 of the screen across, place the light more exactly two levels finer
    // and then in the half resolution first level
    const U32 coarse = llmin(count - 1, 4u);
    const U32 mid = coarse >= 2 ? coarse - 2 : 0;
    const F32 texel = (F32)(1u << (coarse + 1)); // full resolution pixels across one coarse texel
    const SunScreen sun = sun_screen();

    LLGLDepthTest depth_test(GL_FALSE, GL_FALSE);
    LLGLDisable blend(GL_BLEND);
    LLGLDisable cull(GL_CULL_FACE);

    sPicks->bindTarget();
    gFSLensFlarePickProgram.bind();
    gFSLensFlarePickProgram.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, &levels[coarse], false, LLTexUnit::TFO_POINT);
    gFSLensFlarePickProgram.bindTexture(LLShaderMgr::DEFERRED_SPECULAR, &levels[mid], false, LLTexUnit::TFO_POINT);
    gFSLensFlarePickProgram.bindTexture(LLShaderMgr::DEFERRED_EMISSIVE, &levels[0], false, LLTexUnit::TFO_POINT);
    gFSLensFlarePickProgram.bindTexture(LLShaderMgr::DEFERRED_DEPTH, &depth, true);
    gFSLensFlarePickProgram.uniform4f(sFlareSun, sun.mU, sun.mV, sun.mRadius, sun.mFade);
    gFSLensFlarePickProgram.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (F32)depth.getWidth(), (F32)depth.getHeight());
    gFSLensFlarePickProgram.uniform1f(sPickArea, texel * texel);
    draw_triangle(tri);
    gFSLensFlarePickProgram.unbind();
    sPicks->flush();

    sLights->bindTarget();
    gFSLensFlareSelectProgram.bind();
    gFSLensFlareSelectProgram.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, sPicks, false, LLTexUnit::TFO_POINT);
    draw_triangle(tri);
    gFSLensFlareSelectProgram.unbind();
    sLights->flush();

    sLightsFrame = LLFrameTimer::getFrameCount();
}

void FSLensFlare::measureSun(LLRenderTarget& screen, LLRenderTarget& depth, LLRenderTarget& exposure, LLVertexBuffer* tri)
{
    static LLCachedControl<F32> render_exposure(gSavedSettings, "RenderExposure", 1.f);

    sStateFrame = 0;
    if (!enabled() || !tri || !gFSLensFlareStateProgram.isComplete())
    {
        free_target(sState[0]);
        free_target(sState[1]);
        sStateValid = false;
        return;
    }
    if (gSnapshotNoPost)
    {
        // A snapshot without post effects draws no flare: keep the history as it is, so the live
        // view's flare does not start over after it
        return;
    }

    const SunScreen sun = sun_screen();
    if (!sun.mUp)
    {
        sStateValid = false;
        return;
    }
    if (sun.mSun != sStateSun)
    {
        // A sun's drive fading out on the moon would be wrong: start over
        sStateSun = sun.mSun;
        sStateValid = false;
    }

    bool made0 = false;
    bool made1 = false;
    if (!ensure_target(sState[0], 1, 1, made0) || !ensure_target(sState[1], 1, 1, made1))
    {
        sStateValid = false;
        return;
    }
    if (made0 || made1)
    {
        sStateValid = false;
    }

    LL_PROFILE_GPU_ZONE("lens flare sun");

    // Moved toward this frame's measure at a rate set by the frame time, so the fade takes as long
    // at any frame rate. A snapshot or a fresh history takes it at once.
    const F32 dt = llclamp(LLFrameTimer::getFrameDeltaTimeF32(), 0.f, 0.25f);
    F32 rise = 1.f - expf(-dt / RISE_SECONDS);
    F32 fall = 1.f - expf(-dt / FALL_SECONDS);
    if (!sStateValid || gSnapshot)
    {
        rise = 1.f;
        fall = 1.f;
    }

    LLRenderTarget* prev = sState[sStateIndex];
    LLRenderTarget* next = sState[sStateIndex ^ 1];

    LLGLDepthTest depth_test(GL_FALSE, GL_FALSE);
    LLGLDisable blend(GL_BLEND);
    LLGLDisable cull(GL_CULL_FACE);

    next->bindTarget();
    gFSLensFlareStateProgram.bind();
    gFSLensFlareStateProgram.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, &screen, false, LLTexUnit::TFO_BILINEAR);
    gFSLensFlareStateProgram.bindTexture(LLShaderMgr::DEFERRED_DEPTH, &depth, true);
    gFSLensFlareStateProgram.bindTexture(LLShaderMgr::EXPOSURE_MAP, &exposure);
    gFSLensFlareStateProgram.bindTexture(LLShaderMgr::SCENE_MAP, prev, false, LLTexUnit::TFO_POINT);
    gFSLensFlareStateProgram.uniform1f(sExposure, llclamp((F32)render_exposure, 0.5f, 4.f)); // as tonemap() clamps it
    gFSLensFlareStateProgram.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (F32)screen.getWidth(), (F32)screen.getHeight());
    gFSLensFlareStateProgram.uniform4f(sFlareSun, sun.mU, sun.mV, sun.mRadius, sun.mFade);
    gFSLensFlareStateProgram.uniform2f(sFlareRate, rise, fall);
    draw_triangle(tri);
    gFSLensFlareStateProgram.unbind();
    next->flush();

    sStateIndex ^= 1;
    sStateValid = true;
    sStateFrame = LLFrameTimer::getFrameCount();
}

bool FSLensFlare::setupLensPass(LLGLSLShader& shader, bool hq, LLRenderTarget* src, LLRenderTarget& exposure)
{
    static LLCachedControl<F32> render_exposure(gSavedSettings, "RenderExposure", 1.f);
    static LLCachedControl<F32> lights(gSavedSettings, "FSRenderLensFlareLights", 0.5f);
    static LLCachedControl<F32> starburst(gSavedSettings, "FSRenderLensFlareStarburst", 1.f);
    static LLCachedControl<S32> blades(gSavedSettings, "FSRenderLensFlareBlades", 6);

    shader.bindTexture(LLShaderMgr::EXPOSURE_MAP, &exposure);
    if (!hq)
    {
        // Unused, but every sampler still gets a texture of its kind
        shader.uniform1i(sFlareHQ, 0);
        shader.bindTexture(LLShaderMgr::SCENE_MAP, src, false, LLTexUnit::TFO_POINT);
        shader.bindTexture(LLShaderMgr::DEFERRED_BLOOM, src, false, LLTexUnit::TFO_POINT);
        return false;
    }

    const U32 frame = LLFrameTimer::getFrameCount();
    LLRenderTarget* state = sState[sStateIndex];
    const bool sun_ok = sStateValid && sStateFrame == frame && state && state->isComplete();
    const bool lights_ok = sLightsFrame == frame && sLights && sLights->isComplete();

    shader.uniform1i(sFlareHQ, 1);
    shader.bindTexture(LLShaderMgr::SCENE_MAP, sun_ok ? state : src, false, LLTexUnit::TFO_POINT);
    shader.bindTexture(LLShaderMgr::DEFERRED_BLOOM, lights_ok ? sLights : src, false, LLTexUnit::TFO_POINT);
    shader.uniform1f(sFlareHQSun, sun_ok ? 1.f : 0.f);
    shader.uniform1f(sExposure, llclamp((F32)render_exposure, 0.5f, 4.f));

    const SunScreen sun = sun_screen();
    const S32 blade_count = (S32)blades < 3 ? 0 : llmin((S32)blades, 12);
    shader.uniform4f(sFlareHQParams, llclamp((F32)starburst, 0.f, 4.f), lights_ok ? llclamp((F32)lights, 0.f, 4.f) : 0.f,
                     (F32)blade_count, sun.mRadius);
    return true;
}

void FSLensFlare::release()
{
    free_target(sState[0]);
    free_target(sState[1]);
    free_target(sPicks);
    free_target(sLights);
    sStateValid = false;
    sStateFrame = 0;
    sLightsFrame = 0;
}
