/**
 * @file fstexturerelease.cpp
 * @brief FSTextureReleaseFaceless: video memory of textures on no face is handed back after a while
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, Trish
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

#include "fstexturerelease.h"

#include "fslighterwhilemoving.h"
#include "llframetimer.h"
#include "llviewercontrol.h"
#include "llviewerpartsim.h"
#include "llviewerpartsource.h"
#include "llviewertexture.h"

#include <unordered_set>

namespace
{
    // Memory handed back per second at most, and shrinks queued per frame at most: each shrink is a
    // small GPU copy into a new texture, so thousands at once (a region left behind) are spread over
    // a few seconds instead of landing in one frame.
    constexpr F64 RELEASE_BYTES_PER_SEC = 1024.0 * 1024.0 * 1024.0;
    constexpr U32 MAX_PER_FRAME = 16;
    constexpr F32 PARTICLE_REFRESH = 1.f;  // seconds between rebuilds of the particle source set
    constexpr F32 LOG_INTERVAL = 30.f;

    bool s_active = false;
    bool s_urgent = false;      // video memory is paging
    F64 s_budget_bytes = 0.0;
    U32 s_frame_count = 0;

    std::unordered_set<LLUUID> s_particle_textures;  // by id: sources hold their texture, but not every kind exposes it
    LLFrameTimer s_particle_timer;

    S64 s_released_bytes = 0;
    U32 s_released_count = 0;
    LLFrameTimer s_log_timer;
}

bool FSTextureRelease::enabled()
{
    static LLCachedControl<bool> release_faceless(gSavedSettings, "FSTextureReleaseFaceless", true);
    return release_faceless;
}

bool FSTextureRelease::active()
{
    return s_active;
}

F32 FSTextureRelease::delay()
{
    static LLCachedControl<F32> delay_setting(gSavedSettings, "FSTextureReleaseFacelessDelay", 20.f);
    constexpr F32 URGENT_DELAY = 5.f;
    return s_urgent ? URGENT_DELAY : llclamp((F32)delay_setting, 5.f, 300.f);
}

void FSTextureRelease::setUrgent(bool urgent)
{
    s_urgent = urgent;
}

void FSTextureRelease::updateClass()
{
    // While moving with FSRenderMovingTextureBiasHold nothing resident is shrunk (each shrink is a new
    // GL texture); faceless textures wait until the camera is still like the rest
    s_active = enabled() && (s_urgent || !FSMoving::textureBiasHold());
    s_frame_count = 0;
    if (!s_active)
    {
        s_budget_bytes = 0.0;
        s_particle_textures.clear();
        return;
    }

    const F64 dt = llclamp((F64)LLFrameTimer::getFrameDeltaTimeF32(), 0.0, 0.25);
    s_budget_bytes = llmin(s_budget_bytes + dt * RELEASE_BYTES_PER_SEC, RELEASE_BYTES_PER_SEC * 0.25);

    if (s_particle_timer.getElapsedTimeF32() > PARTICLE_REFRESH || s_particle_textures.empty())
    {
        s_particle_timer.reset();
        s_particle_textures.clear();
        if (LLViewerPartSim::instanceExists())
        {
            const LLViewerPartSim::source_list_t* sources = LLViewerPartSim::getInstance()->getParticleSystemList();
            for (const LLPointer<LLViewerPartSource>& source : *sources)
            {
                if (source.notNull() && !source->isDead())
                {
                    const LLUUID id = source->getImageUUID();
                    if (id.notNull())
                    {
                        s_particle_textures.insert(id);
                    }
                }
            }
        }
    }

    if (s_log_timer.getElapsedTimeF32() > LOG_INTERVAL)
    {
        s_log_timer.reset();
        if (s_released_count > 0)
        {
            LL_INFOS("FSTextureBias") << llformat("Released %.0f MB of video memory from %u textures on no face for %.0f s over the last %.0f s",
                                                  (F64)s_released_bytes / (1024.0 * 1024.0), s_released_count, delay(), LOG_INTERVAL)
                                      << LL_ENDL;
        }
        s_released_bytes = 0;
        s_released_count = 0;
    }
}

bool FSTextureRelease::usedByParticles(const LLViewerTexture* tex)
{
    return tex && s_particle_textures.count(tex->getID()) > 0;
}

bool FSTextureRelease::takeBudget(S64 bytes)
{
    if (!s_active || s_frame_count >= MAX_PER_FRAME || s_budget_bytes <= 0.0)
    {
        return false;
    }
    ++s_frame_count;
    s_budget_bytes -= (F64)llmax(bytes, (S64)0);
    return true;
}

void FSTextureRelease::noteReleased(S64 bytes)
{
    if (bytes > 0)
    {
        s_released_bytes += bytes;
        ++s_released_count;
    }
}
