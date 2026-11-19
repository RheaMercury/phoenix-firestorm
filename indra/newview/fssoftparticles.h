/**
 * @file fssoftparticles.h
 * @brief Soft particles: particles fade where they meet the scene (FSRenderSoftParticles)
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

#ifndef FS_SOFTPARTICLES_H
#define FS_SOFTPARTICLES_H

class LLGLSLShader;

// Particles fade out over a short distance in front of opaque surfaces and the water instead of
// cutting hard lines into them (GPU Gems 3 ch. 23). The world alpha, fullbright alpha and glow
// programs get fsSoftParticlesF.glsl (FS_SOFT_PARTICLES) when the setting is on; the alpha pool
// sets its uniforms before each draw of a particle group. The scene depth is the pre-alpha copy
// LLPipeline takes for name tags (fs_take_tag_depth), which it then also takes for this.
// Main thread only, like the rest of the render pipeline.
namespace FSSoftParticles
{
    // FSRenderSoftParticles: whether the programs get the permutation and the depth copy is wanted
    bool enabled();

    // The pre-alpha depth copy (LLPipeline::mFSTagDepth) was just taken for the main view
    void noteDepthTaken();

    // At the start of an alpha pass: true when particles drawn in it can fade (main view, depth taken
    // this frame, not HUDs, impostors, probes or mirrors). Takes the pass's projection.
    bool beginPass();

    // Before a draw in a pass beginPass accepted: the fade for a particle, none for anything else.
    // A no-op for programs without the permutation.
    void apply(LLGLSLShader& shader, bool particle, bool additive);
}

#endif // FS_SOFTPARTICLES_H
