/**
 * @file alphaV.glsl
 *
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

#ifdef FS_GDD_LEGACY
#extension GL_ARB_shader_storage_buffer_object : require
#endif

#ifdef RIGGED_PRECISE_MATH
invariant gl_Position;
precise gl_Position;
#endif

#define INDEXED 1
#define NON_INDEXED 2
#define NON_INDEXED_NO_COLOR 3

#ifdef FS_GDD_LEGACY
// <FS:Trish> FSPerfGPUDrivenAlpha (fsgpudrivenalpha.cpp): runs of alpha draws drawn with
// glMultiDrawElementsIndirect out of a vertex arena, as diffuseV.glsl does for FSPerfGPUDrivenLegacy.
// Every draw command has a record, an instanced attribute its base instance selects, naming its matrices
// and its textures in the storage buffers below, which hold exactly what the stock path puts in the
// uniforms of the same names and binds on the texture units for that draw. Skinned (HAS_SKIN): the
// record's w is where the draw's joint palette starts in the storage buffer objectSkinV.glsl (built with
// FS_GDD_RIGGED) reads it from; projection_matrix stays the uniform the stock skinned branch reads (set by
// gGL.syncMatrices before the multi-draw).
layout(std430) readonly buffer FSGDDLegacyMatrices
{
    mat4 fs_gdd_matrix[];   // per model matrix: modelview, modelview projection, normal matrix; texture matrices
};

layout(std430) readonly buffer FSGDDLegacyTextures
{
    uvec2 fs_gdd_tex[];     // per draw: (minimum_alpha bits, texture count), then a bindless handle per texture
};

in uvec4 fs_gdd_draw;       // x: first of the three model matrices, y: texture matrix, z: first entry in fs_gdd_tex, w: palette (skinned)
in int texture_index;       // the unit diffuseLookup would pick (indexedTextureV.glsl)

flat out uvec2 fs_gdd_diffuse;

#ifdef HAS_SKIN
uint fsGDDSkinBase()        // objectSkinV.glsl (FS_GDD_RIGGED): the palette's first vec4
{
    return fs_gdd_draw.w;
}
#endif
// </FS:Trish>
#else
uniform mat3 normal_matrix;
uniform mat4 texture_matrix0;
uniform mat4 modelview_matrix;
uniform mat4 modelview_projection_matrix;
#endif
uniform mat4 projection_matrix;

in vec3 position;

#ifdef USE_INDEXED_TEX
void passTextureIndex();
#endif

in vec3 normal;

#ifdef USE_VERTEX_COLOR
in vec4 diffuse_color;
#endif

in vec2 texcoord0;

#ifdef HAS_SKIN
mat3x4 getSkinBlend();
vec3 skinNormal(mat3x4 b, vec3 pos, vec3 dir, mat4 m);
vec4 skinTransformH(mat3x4 b, vec3 pos, mat4 m);
#else
#ifdef IS_AVATAR_SKIN
mat4 getSkinnedTransform();
#endif
#endif

out vec3 vary_fragcoord;
out vec3 vary_position;

#ifdef USE_VERTEX_COLOR
out vec4 vertex_color;
#endif

out vec2 vary_texcoord0;
out vec3 vary_norm;

uniform float near_clip;

void main()
{
    vec4 pos;
    vec3 norm;

#ifdef FS_GDD_LEGACY
    // <FS:Trish> FSPerfGPUDrivenAlpha: the draw's matrices and the texture diffuseLookup would pick
    mat4 modelview_matrix = fs_gdd_matrix[fs_gdd_draw.x];
    mat4 modelview_projection_matrix = fs_gdd_matrix[fs_gdd_draw.x + 1u];
    mat3 normal_matrix = mat3(fs_gdd_matrix[fs_gdd_draw.x + 2u]);
    mat4 texture_matrix0 = fs_gdd_matrix[fs_gdd_draw.y];
    uvec2 fs_gdd_head = fs_gdd_tex[fs_gdd_draw.z];
    // an index past the draw's textures samples a unit left from an earlier draw on the stock path; the last one here
    uint fs_gdd_index = min(uint(max(texture_index, 0)), max(fs_gdd_head.y, 1u) - 1u); // <FS:Trish/> a count of 0 (never sent) can't wrap past the list
    fs_gdd_diffuse = fs_gdd_tex[fs_gdd_draw.z + 1u + fs_gdd_index];
    // </FS:Trish>
#endif

    //transform vertex
#ifdef HAS_SKIN
    mat3x4 skin = getSkinBlend();
    pos = skinTransformH(skin, position.xyz, modelview_matrix);
    norm = normalize(skinNormal(skin, position.xyz, normal.xyz, modelview_matrix));
    vec4 frag_pos = projection_matrix * pos;
    gl_Position = frag_pos;
#else

#ifdef IS_AVATAR_SKIN
    mat4 trans = getSkinnedTransform();
    vec4 pos_in = vec4(position.xyz, 1.0);
    pos.x = dot(trans[0], pos_in);
    pos.y = dot(trans[1], pos_in);
    pos.z = dot(trans[2], pos_in);
    pos.w = 1.0;

    norm.x = dot(trans[0].xyz, normal);
    norm.y = dot(trans[1].xyz, normal);
    norm.z = dot(trans[2].xyz, normal);
    norm = normalize(norm);

    vec4 frag_pos = projection_matrix * pos;
    gl_Position = frag_pos;
#else
    norm = normalize(normal_matrix * normal);
    vec4 vert = vec4(position.xyz, 1.0);
    pos = (modelview_matrix * vert);
    gl_Position = modelview_projection_matrix*vec4(position.xyz, 1.0);
#endif //IS_AVATAR_SKIN

#endif // HAS_SKIN

#ifdef USE_INDEXED_TEX
    passTextureIndex();
#endif

    vary_texcoord0 = (texture_matrix0 * vec4(texcoord0,0,1)).xy;

    vary_norm = norm;
    vary_position = pos.xyz;

#ifdef USE_VERTEX_COLOR
    vertex_color = diffuse_color;
#endif

#ifdef HAS_SKIN
    vary_fragcoord.xyz = frag_pos.xyz + vec3(0,0,near_clip);
#else

#ifdef IS_AVATAR_SKIN
    vary_fragcoord.xyz = pos.xyz + vec3(0,0,near_clip);
#else
    pos = modelview_projection_matrix * vert;
    vary_fragcoord.xyz = pos.xyz + vec3(0,0,near_clip);
#endif

#endif

}
