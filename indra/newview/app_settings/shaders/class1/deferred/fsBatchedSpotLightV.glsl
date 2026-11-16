/**
 * @file class1\deferred\fsBatchedSpotLightV.glsl
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

// Batched deferred spotlights without a shadow: one instance per light, drawn as its box volume

// The vertex half of gFSBatchedSpotLightProgram (FSPerfBatchedSpotLights); the fragment half is
// fsBatchedSpotLightF.glsl. fsspotlightbatch.cpp uploads every queued light once per frame and
// binds 16 KB of it at a time to the FSSpotLights block; light i of a draw starts at vec4
// fs_spot_base + i * FS_SPOT_VEC4_PER_LIGHT. The vertex buffer is an unindexed [-1,1] cube, six
// faces of six vertices in the order of fs_face_normal.
//
// Camera outside the box (fs_spot_inside 0): only the faces toward the camera are drawn, as the
// stock path's get_box_fan_indices; the others are collapsed outside the clip volume, which is
// independent of winding and of the face culling state. The camera is at least 0.2 m outside,
// so at least one face survives.
// Camera inside the box (fs_spot_inside 1): every face is drawn, depth tested GEQUAL against the
// scene with depth clamping on (see FSSpotLightBatch::flush).

uniform mat4 modelview_projection_matrix;
uniform mat4 modelview_matrix;

uniform vec3 fs_camera_origin; // agent space, like the light centres
uniform int fs_spot_base;      // first vec4 of this draw's lights in the bound range
uniform int fs_spot_inside;    // 1: the camera is inside these lights' boxes

#define FS_SPOT_VEC4_PER_LIGHT 10
#define FS_SPOT_BLOCK_VEC4 1024

layout(std140) uniform FSSpotLights
{
    // per light, see fsspotlightbatch.h:
    // [0] centre (agent space) and volume half size, [1] linear colour and falloff,
    // [2..5] screen to light projection, [6] proj_p, proj_near, [7] proj_n, proj_range,
    // [8] proj_origin, proj_ambiance, [9] proj_focus, proj_lod, proj_ambient_lod
    vec4 fs_spot_data[FS_SPOT_BLOCK_VEC4];
};

in vec3 position;

out vec4 vary_fragcoord;
flat out vec3 trans_center;
flat out int fs_spot_index;

const vec3 fs_face_normal[6] = vec3[6](vec3(-1.0, 0.0, 0.0), vec3(1.0, 0.0, 0.0),
                                       vec3(0.0, -1.0, 0.0), vec3(0.0, 1.0, 0.0),
                                       vec3(0.0, 0.0, -1.0), vec3(0.0, 0.0, 1.0));

void main()
{
    int k = fs_spot_base + gl_InstanceID * FS_SPOT_VEC4_PER_LIGHT;
    vec4 center_size = fs_spot_data[k];

    vec3 center = center_size.xyz;
    float size = center_size.w;

    // as pointLightV.glsl, the stock spotlight box path's vertex shader
    trans_center = (modelview_matrix * vec4(center, 1.0)).xyz;
    fs_spot_index = k;

    if (fs_spot_inside == 0)
    {
        // Camera in front of this face's plane: distance past the face along its outward normal.
        vec3 n = fs_face_normal[gl_VertexID / 6];
        if (dot(n, fs_camera_origin - center) <= size)
        {
            // All six vertices of the face land on the same point beyond the far plane:
            // zero area and clipped, nothing is rasterised.
            vary_fragcoord = vec4(0.0, 0.0, 2.0, 1.0);
            gl_Position = vary_fragcoord;
            return;
        }
    }

    vec3 p = position * size + center;
    vec4 pos = modelview_projection_matrix * vec4(p, 1.0);
    vary_fragcoord = pos;
    gl_Position = pos;
}
