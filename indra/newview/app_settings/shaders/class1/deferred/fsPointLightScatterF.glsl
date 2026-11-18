/**
 * @file class1/deferred/fsPointLightScatterF.glsl
 * @brief Local point lights glowing in the air (FSRenderPointLightScatter)
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

// Light from the local point lights scattered toward the camera by the air along each view ray.
// Drawn first into the projector volumetric map (fsSpotVolumetricF.glsl's), at its reduced
// resolution: rgb is the scattered light, a the distance this pixel sees, which the projectors
// drawn after it leave alone. The map's temporal resolve and composite then treat both alike.
//
// No march: a point light lights the air with the same falloff it lights surfaces with,
// calcLegacyDistanceAttenuation's 2 (1 - r/S)^2 / (1 + falloff)^2 inside its range S, and along a
// straight ray through a homogeneous medium that integrates in closed form. With u the distance
// along the ray from its closest approach to the light, h that closest distance, and
// r = sqrt(u^2 + h^2):
//     integral of (1 - r/S)^2 du = u - (u r + h^2 asinh(u / h)) / S + (u^3 / 3 + h^2 u) / S^2
// taken over the part of the ray inside the range and in front of the scene. The phase function
// (Henyey-Greenstein, as the projector cones use it) changes along the ray, so it is averaged over
// that segment, weighted by the falloff, at four Gauss-Legendre points on each side of the
// closest approach, where the angle to the light turns fastest: within about 1% of the exact
// average for 99% of rays, up to about 15% for rays grazing a light with strong forward
// scattering. The air's density is the height fog's at the closest approach when the fog is on,
// else a constant; the light reaching the camera is dimmed by the air in between.

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform samplerBuffer fs_cl_lights;     // per light: view space centre + range, linear colour + falloff
uniform usamplerBuffer fs_cl_lists;     // header of (offset, count) entries, then light indices
uniform ivec4 fs_cl_dims;               // tiles across, tiles down, depth slices, first tile entry
uniform vec4 fs_cl_scale;               // tiles per unit of screen coordinate across and down

uniform int fs_scatter_max;             // lights per pixel at most (the nearest)
uniform float fs_scatter_strength;      // over the strength the composite applies
uniform float fs_scatter_density;       // scattering per metre, without the height fog
uniform float vol_anisotropy;           // 0 scatters evenly, toward 1 concentrates looking into the light
uniform vec4 fs_air_fog;                // height fog: density at its base, falloff per metre, unused, 1 when on
uniform vec4 fs_air_plane;              // view space up, camera height above the fog's base

// Distance stored for the sky, as in fsSpotVolumetricF
const float SKY_DIST = 65000.0;

float getDepth(vec2 pos_screen);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);

// Henyey-Greenstein, scaled so that even scattering is 1 (as in fsSpotVolumetricF)
float phase(float cos_theta, float g)
{
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cos_theta;
    // Capped: near g = 1 the peak grows without bound and blows out bloom
    return min((1.0 - g2) / (denom * sqrt(denom)), 16.0);
}

// The air's scattering per metre at a view space point
float airDensity(vec3 p)
{
    if (fs_air_fog.w > 0.5)
    {
        float h = dot(p, fs_air_plane.xyz) + fs_air_plane.w;
        return fs_air_fog.x * exp(-fs_air_fog.y * max(h, 0.0));
    }
    return fs_scatter_density;
}

// asinh without the cancellation of log(x + sqrt(x^2 + 1)) for large negative x
float fsAsinh(float x)
{
    float a = abs(x);
    return sign(x) * log(a + sqrt(a * a + 1.0));
}

// Antiderivative of (1 - r/S)^2 along the ray, u from the closest approach, h its distance
float falloffPrimitive(float u, float h, float range)
{
    float r = sqrt(u * u + h * h);
    float int_r = 0.5 * (u * r + h * h * fsAsinh(u / h));
    float int_r2 = u * u * u / 3.0 + h * h * u;
    return u - 2.0 * int_r / range + int_r2 / (range * range);
}

void main()
{
    vec2 tc = vary_fragcoord.xy;
    float depth = getDepth(tc);
    bool sky = depth >= 1.0;

    vec3 pos = getPositionWithDepth(tc, sky ? 0.5 : depth).xyz;
    vec3 view_dir = normalize(pos);
    float scene_dist = sky ? SKY_DIST : min(length(pos), SKY_DIST);

    ivec2 tile = clamp(ivec2(tc * fs_cl_scale.xy), ivec2(0), fs_cl_dims.xy - ivec2(1));
    int entry = fs_cl_dims.w + tile.y * fs_cl_dims.x + tile.x;
    int first = int(texelFetch(fs_cl_lists, entry * 2).r);
    int count = min(int(texelFetch(fs_cl_lists, entry * 2 + 1).r), fs_scatter_max);

    const vec4 gauss_x = vec4(-0.8611363, -0.3399810, 0.3399810, 0.8611363);
    const vec4 gauss_w = vec4(0.3478548, 0.6521452, 0.6521452, 0.3478548);
    float g = vol_anisotropy;

    vec3 result = vec3(0.0);
    for (int i = 0; i < count; ++i)
    {
        int idx = int(texelFetch(fs_cl_lists, first + i).r);
        vec4 light = texelFetch(fs_cl_lights, idx * 2);
        vec4 light_col = texelFetch(fs_cl_lights, idx * 2 + 1);
        vec3 center = light.xyz;
        float range = light.w;

        // The part of the view ray inside the light's range, in front of what the pixel sees
        float b = dot(view_dir, center);
        float h2 = max(dot(center, center) - b * b, 0.0);
        float range2 = range * range;
        if (range <= 0.0 || h2 >= range2)
        {
            continue;
        }
        float half_chord = sqrt(range2 - h2);
        float t0 = max(b - half_chord, 0.0);
        float t1 = min(b + half_chord, scene_dist);
        if (t1 <= t0)
        {
            continue;
        }
        float h = max(sqrt(h2), 1e-4);

        // Falloff integrated over the segment, with calcLegacyDistanceAttenuation's scale
        float falloff = light_col.a;
        float atten = (falloffPrimitive(t1 - b, h, range) - falloffPrimitive(t0 - b, h, range)) * 2.0 / ((1.0 + falloff) * (1.0 + falloff));
        if (atten <= 0.0)
        {
            continue;
        }

        // The phase function averaged over the segment, weighted by the falloff, split at the
        // closest approach (one side is empty when the segment does not reach it)
        float t_near = clamp(b, t0, t1);
        float phase_sum = 0.0;
        float weight_sum = 0.0;
        for (int side = 0; side < 2; ++side)
        {
            float s0 = side == 0 ? t0 : t_near;
            float s1 = side == 0 ? t_near : t1;
            float mid = 0.5 * (s0 + s1);
            float half_len = 0.5 * (s1 - s0);
            for (int k = 0; k < 4; ++k)
            {
                float u = mid + half_len * gauss_x[k] - b;
                float r = sqrt(u * u + h2);
                float a = max(1.0 - r / range, 0.0);
                float w = gauss_w[k] * half_len * a * a;
                // toward the light from the point on the ray: cos = (b - t) / r
                phase_sum += w * phase(-u / max(r, 1e-4), g);
                weight_sum += w;
            }
        }
        float mean_phase = weight_sum > 1e-12 ? phase_sum / weight_sum : phase(0.0, g);

        // The air at the closest approach scatters; the air on the way to the camera dims it
        float sigma = airDensity(view_dir * t_near);
        float transmit = exp(-airDensity(view_dir * (0.5 * t_near)) * t_near);

        result += light_col.rgb * (atten * mean_phase * sigma * transmit);
    }

    frag_color = vec4(max(result * fs_scatter_strength, vec3(0.0)), scene_dist);
}
