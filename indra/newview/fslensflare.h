/**
 * @file fslensflare.h
 * @brief High quality lens flare (FSRenderLensFlareHQ): the sun's drive and the bright lights on
 *        screen, measured on the GPU for the lens effects pass.
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

#ifndef FS_LENSFLARE_H
#define FS_LENSFLARE_H

class LLGLSLShader;
class LLRenderTarget;
class LLVertexBuffer;

extern LLGLSLShader gFSLensFlareStateProgram;
extern LLGLSLShader gFSLensFlarePickProgram;
extern LLGLSLShader gFSLensFlareSelectProgram;

// Nothing here reads back to the CPU: every pass writes a tiny target the next one samples.
//
// pickLights   from renderBloom, after its down chain: the brightest small lights in 8 x 4 tiles of
//              the bloom's bright pass, then the strongest 8 of them (fsLensFlarePickF, fsLensFlareSelectF)
// measureSun   from renderFinalize once the exposure is known, on the HDR scene after the TAA resolve:
//              how much of the sun's disc is uncovered and how bright, filtered over time (fsLensFlareStateF)
// setupLensPass from renderLensEffects: binds both results to the lens effects program, which draws the
//              flare on the finished image
//
// Main view only: the probes and mirrors never run renderFinalize.
namespace FSLensFlare
{
    // FSRenderLensFlareHQ is on and FSRenderLensFlare gives the flare a strength
    bool enabled();

    void pickLights(LLRenderTarget* levels, U32 count, LLRenderTarget& depth, LLVertexBuffer* tri);
    void measureSun(LLRenderTarget& screen, LLRenderTarget& depth, LLRenderTarget& exposure, LLVertexBuffer* tri);

    // With the lens effects program bound: binds the sun's drive and the lights, and sets the high
    // quality uniforms; with hq false, only turns the high quality path off in the program (the
    // samplers still get a texture, src). Returns whether the high quality path is on.
    bool setupLensPass(LLGLSLShader& shader, bool hq, LLRenderTarget* src, LLRenderTarget& exposure);

    void release();
}

#endif // FS_LENSFLARE_H
