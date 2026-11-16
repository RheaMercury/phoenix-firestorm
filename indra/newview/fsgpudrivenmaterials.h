/**
 * @file fsgpudrivenmaterials.h
 * @brief GPU-driven draws for the deferred legacy material passes (FSPerfGPUDrivenMaterials)
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

#ifndef FS_GPUDRIVENMATERIALS_H
#define FS_GPUDRIVENMATERIALS_H

#include "stdtypes.h"

class LLGLSLShader;

// The non blended gDeferredMaterialProgram permutations built with FS_GDD_MATERIAL: matrices and material from
// storage buffers, the diffuse, normal and specular maps as bindless handles. Built only while
// FSPerfGPUDrivenMaterials is on, the driver has gGLManager.mFSHasGPUDrivenDraws and bindless textures work
// (NVIDIA, fsGDDBindlessSetup); the rigged ones only while fsGDDRiggedWanted() also holds and the shaders read
// the palette buffer (FSSkinPaletteBuffer::active). After the stock programs are set up (via fsGDDLoadShaders,
// after fsGDDPBRLoadShaders).
void fsGDDMaterialsLoadShaders();
void fsGDDMaterialsUnloadShaders();
// Frees the arenas and the per draw buffers.
void fsGDDMaterialsReleaseGL();

// FSPerfGPUDrivenRigged and what it builds on are on (one of FSPerfGPUDrivenMaterials, FSPerfGPUDrivenPBR,
// FSPerfGPUDrivenLegacy and FSPerfGPUDrivenAlpha, FSPerfRiggedPaletteBuffer, the driver's multi-draw support): LLViewerShaderMgr::
// loadBasicShaders builds the skinning object the rigged programs link (LLShaderMgr::FS_GDD_SKIN_OBJECT).
bool fsGDDRiggedWanted();
// The rigged multi-draw programs may be built now (shader load): fsGDDRiggedWanted, the shaders read the palette
// buffer (FSSkinPaletteBuffer::active), the skinning object was built, and vs_blocks (the driver's vertex shader
// storage blocks) has room for the palettes' as a third.
bool fsGDDRiggedLoadable(S32 vs_blocks);
// The rigged multi-draws may run now (draw time): FSPerfGPUDrivenRigged on, the shaders read the palette
// buffer, not an impostor (those stay stock).
bool fsGDDRiggedEnabled();

// LLDrawPoolMaterials::renderDeferred, in place of its draw loop, with stock (one of gDeferredMaterialProgram)
// bound; rigged passes only with FSPerfGPUDrivenRigged. Draws the pass's batches in their order: runs that can come from the arena
// as multi-draws, the rest the stock way. False, with nothing drawn, when the feature is off or can't run
// here; the caller then draws the pass the stock way. stock is bound again on return.
bool fsGDDDrawMaterials(U32 type, LLGLSLShader& stock);

#endif // FS_GPUDRIVENMATERIALS_H
