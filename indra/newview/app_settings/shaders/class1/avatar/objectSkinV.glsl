/**
 * @file objectSkinV.glsl
 * $LicenseInfo:firstyear=2007&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2007, Linden Research, Inc.
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
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

// <FS:Trish> FSPerfGPUDrivenRigged
#ifdef FS_GDD_RIGGED
#extension GL_ARB_shader_storage_buffer_object : require
#endif
// </FS:Trish>

in vec4 weight4;

#ifdef FS_GDD_RIGGED
// <FS:Trish> FSPerfGPUDrivenRigged (fsgpudrivenmaterials.cpp): this object is compiled a second time with
// FS_GDD_RIGGED (LLShaderMgr::FS_GDD_SKIN_OBJECT) for the multi-drawn rigged programs. The palette buffer of
// FSPerfRiggedPaletteBuffer is bound whole as a storage buffer, and the program's vertex shader hands over
// (fsGDDSkinBase) where the draw's palette starts in it, in vec4s: the same header and std140 mat3x4
// columns the FSSkinPalette block below reads, so every value and every operation below is the same.
layout(std430) readonly buffer FSGDDSkinPalettes
{
    vec4 fs_gdd_skin[];
};
uint fsGDDSkinBase();   // the program's vertex shader: its draw record's palette offset
// The record's top bit is skin SSS's exclusion (FS_GDD_SKIN_EXCLUDE_BIT, fsskinpalettebuffer.h), not offset
#define FS_GDD_SKIN_BASE (fsGDDSkinBase() & 0x7FFFFFFFu)

mat3x4 fsGDDSkinJoint(int i)
{
    uint at = FS_GDD_SKIN_BASE + 1u + 3u * uint(i);
    return mat3x4(fs_gdd_skin[at], fs_gdd_skin[at + 1u], fs_gdd_skin[at + 2u]);
}
#define FS_SKIN_JOINT(i) fsGDDSkinJoint(i)
#define matrixPaletteSize int(fs_gdd_skin[FS_GDD_SKIN_BASE].w)
#define skin_origin (fs_gdd_skin[FS_GDD_SKIN_BASE].xyz)
// </FS:Trish>
#elif defined(FS_SKIN_PALETTE_BUFFER)
// <FS:Trish> FSPerfRiggedPaletteBuffer: the palette is a range of one buffer written once a frame
// (fsskinpalettebuffer.cpp) instead of uniforms uploaded for every program and pass.
layout(std140) uniform FSSkinPalette
{
    vec4 fs_skin_header;    // xyz: skin_origin, w: matrixPaletteSize
    mat3x4 fs_skin_palette[MAX_JOINTS_PER_MESH_OBJECT];
};
#define matrixPalette fs_skin_palette
#define matrixPaletteSize int(fs_skin_header.w)
#define skin_origin fs_skin_header.xyz
// </FS:Trish>
#else
uniform mat3x4 matrixPalette[MAX_JOINTS_PER_MESH_OBJECT];
// Number of valid entries uploaded for this mesh.  This prevents malformed or
// partially loaded rig weights from reading stale uniform-array entries.
uniform int matrixPaletteSize;
uniform vec3 skin_origin;
#endif
// <FS:Trish> One palette entry (the uniform array, the uniform block or, FS_GDD_RIGGED, the storage buffer)
#ifndef FS_SKIN_JOINT
#define FS_SKIN_JOINT(i) matrixPalette[i]
#endif
// </FS:Trish>

mat3x4 getSkinBlend()
{
    SKIN_PRECISE vec4 w = fract(weight4);
    vec4 index = floor(weight4);

    int last_palette_index = min(max(matrixPaletteSize - 1, 0), MAX_JOINTS_PER_MESH_OBJECT - 1);
    index = min(index, vec4(float(last_palette_index)));
    index = max(index, vec4( 0.0));

    SKIN_PRECISE float weight_sum = w.x + w.y + w.z + w.w;
    if (weight_sum <= 0.00001)
    {
        // Broken/partially loaded weights must not turn the entire vertex into
        // NaNs.  Keep it attached to the root joint until valid weights arrive.
        w = vec4(1.0, 0.0, 0.0, 0.0);
        index = vec4(0.0);
        weight_sum = 1.0;
    }
    w *= 1.0 / weight_sum;

    int i1 = int(index.x);
    int i2 = int(index.y);
    int i3 = int(index.z);
    int i4 = int(index.w);

    // <FS:Trish> The four joints read once each, through FS_SKIN_JOINT (a copy changes no value)
    //SKIN_PRECISE mat3 mat = mat3(matrixPalette[i1])*w.x;
    //     mat += mat3(matrixPalette[i2])*w.y;
    //     mat += mat3(matrixPalette[i3])*w.z;
    //     mat += mat3(matrixPalette[i4])*w.w;
    //
    //SKIN_PRECISE vec3 trans = vec3(matrixPalette[i1][0].w,matrixPalette[i1][1].w,matrixPalette[i1][2].w)*w.x;
    //     trans += vec3(matrixPalette[i2][0].w,matrixPalette[i2][1].w,matrixPalette[i2][2].w)*w.y;
    //     trans += vec3(matrixPalette[i3][0].w,matrixPalette[i3][1].w,matrixPalette[i3][2].w)*w.z;
    //     trans += vec3(matrixPalette[i4][0].w,matrixPalette[i4][1].w,matrixPalette[i4][2].w)*w.w;
    mat3x4 j1 = FS_SKIN_JOINT(i1);
    mat3x4 j2 = FS_SKIN_JOINT(i2);
    mat3x4 j3 = FS_SKIN_JOINT(i3);
    mat3x4 j4 = FS_SKIN_JOINT(i4);

    SKIN_PRECISE mat3 mat = mat3(j1)*w.x;
         mat += mat3(j2)*w.y;
         mat += mat3(j3)*w.z;
         mat += mat3(j4)*w.w;

    SKIN_PRECISE vec3 trans = vec3(j1[0].w,j1[1].w,j1[2].w)*w.x;
         trans += vec3(j2[0].w,j2[1].w,j2[2].w)*w.y;
         trans += vec3(j3[0].w,j3[1].w,j3[2].w)*w.z;
         trans += vec3(j4[0].w,j4[1].w,j4[2].w)*w.w;
    // </FS:Trish>

    SKIN_PRECISE mat3x4 ret;
    ret[0] = vec4(mat[0], trans.x);
    ret[1] = vec4(mat[1], trans.y);
    ret[2] = vec4(mat[2], trans.z);

// TODO: Consider if this hack is still necessary. Any driver could potentially optimize this out if the return is before the unreachable code or if the assigned variable is unused.
#if defined(IS_AMD_CARD) && !defined(FS_GDD_RIGGED) // <FS:Trish/> no palette array to keep with FS_GDD_RIGGED
   // If it's AMD make sure the GLSL compiler sees the arrays referenced once by static index. Otherwise it seems to optimise the storage awawy which leads to unfun crashes and artifacts.
   mat3x4 dummy1 = matrixPalette[0];
   mat3x4 dummy2 = matrixPalette[MAX_JOINTS_PER_MESH_OBJECT-1];
#endif

    return ret;
}

vec3 skinDirection(mat3x4 skin, vec3 direction)
{
    SKIN_PRECISE vec3 result = mat3(skin) * direction;
    return result;
}

vec3 skinPoint(mat3x4 skin, vec3 position)
{
    SKIN_PRECISE vec3 result = mat3(skin) * position + vec3(skin[0].w, skin[1].w, skin[2].w);
    return result;
}

mat4 skinMatrix(mat3x4 skin)
{
    SKIN_PRECISE mat4 result;
    result[0] = vec4(skin[0].xyz, 0.0);
    result[1] = vec4(skin[1].xyz, 0.0);
    result[2] = vec4(skin[2].xyz, 0.0);
    result[3] = vec4(skinPoint(skin, vec3(0.0)) + skin_origin, 1.0);
    return result;
}

vec4 skinTransformH(mat3x4 skin, vec3 position, mat4 transform)
{
#ifdef RIGGED_LOCAL_ORIGIN
    // Keep origin cancellation separate from the small local vertex position.
    SKIN_PRECISE vec4 local_position = transform * vec4(skinPoint(skin, position), 0.0);
    SKIN_PRECISE vec4 origin = transform * vec4(skin_origin, 1.0);
    SKIN_PRECISE vec4 result = local_position + origin;
#else
    // Comparison path: blend agent-space translations and compose the matrices
    // before transforming the vertex, as before the local-origin experiment.
    SKIN_PRECISE mat4 matrix = transform * skinMatrix(skin);
    SKIN_PRECISE vec4 result = matrix * vec4(position, 1.0);
#endif
    return result;
}

vec3 skinNormal(mat3x4 skin, vec3 position, vec3 direction, mat4 transform)
{
#ifdef RIGGED_DIRECT_NORMALS
    SKIN_PRECISE vec3 result = mat3(transform) * skinDirection(skin, direction);
#else
    // Deliberately retain point subtraction for the normal-precision A/B test.
    SKIN_PRECISE vec3 endpoint = position + direction;
    SKIN_PRECISE vec3 result = skinTransformH(skin, endpoint, transform).xyz
        - skinTransformH(skin, position, transform).xyz;
#endif
    return result;
}

mat4 getObjectSkinnedTransform()
{
    return skinMatrix(getSkinBlend());
}
