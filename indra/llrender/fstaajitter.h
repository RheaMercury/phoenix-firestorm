/**
 * @file fstaajitter.h
 * @brief Temporal anti-aliasing (FSRenderTAA): the sub-pixel projection jitter of the main camera.
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

#ifndef FS_TAAJITTER_H
#define FS_TAAJITTER_H

#include "stdtypes.h"
#include "glm/mat4x4.hpp"

// Like reverse-Z (fsreversez.h), the jitter lives only in what reaches the GPU. The C++ side keeps
// the camera's projection unjittered, so culling, picking, occlusion and every other piece of CPU
// maths is untouched. While a jitter is set, FSReverseZ::rasterProjection (and so
// LLRender::syncMatrices and the GPU-driven draws' matrix tables) returns J * P, where J moves
// clip space x and y by the offset times w: the whole image slides by a fraction of a pixel and
// nothing else changes, depth included.
//
// The pipeline sets it only around the main camera's G-buffer and forward (alpha, water) passes,
// so shadow maps, reflection probes, mirrors, impostors, HUDs, the UI and snapshots are never
// jittered. Main thread only.
namespace FSTAAJitter
{
    // The offset in normalised device coordinates (2 / width is one pixel). Takes effect for the
    // next matrix sync of every program.
    void set(F32 ndc_x, F32 ndc_y);

    // Back to the plain projection.
    void clear();

    bool active();

    // J * proj while a jitter is set, proj otherwise.
    glm::mat4 apply(const glm::mat4& proj);

    // Texture LOD bias of the material shaders (FSRenderTAATextureBias): TAA averages the jitter's
    // samples, so textures can be sampled a little sharper than one sample per pixel asks for.
    // While FS_TAA_LOD_BIAS is compiled in (lodBiasCompiled), LLRender::syncMatrices gives every
    // program that has fs_taa_lod_bias this bias while a jitter is set and 0 otherwise, so shadow
    // maps, reflection probes, HUDs and the UI sample as before. Setting and clearing the jitter
    // reloads the projection, so every program takes the value anew.
    void setLodBias(F32 bias);
    F32 lodBias();  // what fs_taa_lod_bias gets now
    void setLodBiasCompiled(bool compiled);
    bool lodBiasCompiled();
}

#endif // FS_TAAJITTER_H
