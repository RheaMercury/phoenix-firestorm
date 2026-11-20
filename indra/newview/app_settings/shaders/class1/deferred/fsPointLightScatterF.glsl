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
// No march: the light the air around a point light scatters falls off with the inverse square of
// the distance, softened in a small core and windowed to zero at the light's range (see main).
// Along a straight ray, with u the distance from its closest approach to the light and h that
// closest distance, a change of variable to the angle seen from the light makes the inverse
// square constant, so the rest (window, phase function, ...) is integrated over the part of the
// ray inside the range and in front of the scene at four Gauss-Legendre points on each side of
// the closest approach. The air's density is the height fog's at the closest approach when the
// fog is on, else a constant; the light reaching the camera is dimmed by the air in between.
//
// Two looks (fs_scatter_look.x):
// - Natural (default): the light leaves a small core of its own (about a lamp's size), so the glow
//   is bright close to the light and tapers as 1 / distance on screen, the way a lamp looks in
//   mist. The light is dimmed by the air on its way out (exp(-sigma r)), so thick fog keeps halos
//   tighter. The phase function is a Henyey-Greenstein lobe of its own anisotropy blended with
//   even scattering: brighter looking toward the light, still soft from the side. A slow 3D noise
//   in world space (drifting with the region's wind when that effect is on) varies the air, so a
//   halo is never a perfect disc, and the air within a metre or two of the camera scatters less,
//   so standing in a glow does not wash the screen over.
// - Classic: the previous look, its core a fixed share of the range and with the same total light
//   as calcLegacyDistanceAttenuation's 2 (1 - r/S)^2 / (1 + falloff)^2 would give.

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform samplerBuffer fs_cl_lights;     // per light: view space centre + range, linear colour + falloff
uniform usamplerBuffer fs_cl_lists;     // header of (offset, count) entries, then light indices,
                                        // each with its glow's weight in the high 16 bits
uniform ivec4 fs_cl_dims;               // tiles across, tiles down, depth slices, first tile entry
uniform vec4 fs_cl_scale;               // tiles per unit of screen coordinate across and down

uniform int fs_scatter_max;             // lights per pixel at most (the lists hold no more)
uniform float fs_scatter_strength;      // over the strength the composite applies
uniform float fs_scatter_density;       // scattering per metre, without the height fog
uniform float vol_anisotropy;           // 0 scatters evenly, toward 1 concentrates looking into the light
uniform vec4 fs_air_fog;                // height fog: density at its base, falloff per metre, unused, 1 when on
uniform vec4 fs_air_plane;              // view space up, camera height above the fog's base
// x: 1 for the classic look, 0 for the natural one; y: the air's variation (0 none, 1 from nothing
// to twice the density); z: the share of even scattering in the phase function; w: unused
uniform vec4 fs_scatter_look;
uniform vec3 fs_scatter_noise_origin;   // the camera in noise cells, carried back against the wind, wrapped
uniform vec3 fs_scatter_noise_basis[3]; // the world's east, north and up axes in view space, over the noise cell size

// Distance stored for the sky, as in fsSpotVolumetricF
const float SKY_DIST = 65000.0;

float getDepth(vec2 pos_screen);
bool isFarDepth(float d);
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

// Classic look: the glow's soft core, as a fraction of the light's range, and the factor that
// gives the air the same total light as the surface falloff (1 - r/S)^2 did: the volume integrals
// of x^2 (1 - x)^2 and x^2 (1 - x^2)^2 c / (c + x^2) over 0..1, c = 0.02, are 1/30 and 1/140.2
const float FS_SCATTER_CORE = 0.1414214;
const float FS_SCATTER_NORM = 4.6735;

// Natural look: the core is a lamp's size (metres, plus a small share of the range so big lights
// keep a broader centre), and the light's power over its range squared. A light of range S whose
// surfaces get 2 (1 - r/S)^2 gets about P / r^2 at a quarter to a half of its range for
// P = S^2 / 8 .. S^2 / 30; S^2 / 24 puts the glow's total at about two thirds of the classic
// look's, gathered much closer to the light.
const float FS_GLOW_CORE = 0.3;
const float FS_GLOW_CORE_RANGE = 0.03;
const float FS_GLOW_POWER = 1.0 / 24.0;
// The air nearer the camera than about this (metres) scatters less: t^2 / (t^2 + d^2)
const float FS_GLOW_NEAR = 1.5;

// Hash of a noise cell, wrapped every 256 cells (as fs_scatter_noise_origin is), 0 to 1
float fsGlowHash(ivec3 cell)
{
    uvec3 c = uvec3(cell & ivec3(255));
    uint h = (c.x * 0x8da6b343u) ^ (c.y * 0xd8163841u) ^ (c.z * 0xcb1ab31fu);
    h ^= h >> 16u;
    h *= 0x7feb352du;
    h ^= h >> 15u;
    h *= 0x846ca68bu;
    h ^= h >> 16u;
    return float(h >> 8u) * (1.0 / 16777216.0);
}

// Smooth 3D value noise, 0 to 1
float fsGlowNoise(vec3 p)
{
    vec3 i = floor(p);
    vec3 f = p - i;
    vec3 w = f * f * (3.0 - 2.0 * f);
    ivec3 c = ivec3(i);
    float a = mix(fsGlowHash(c), fsGlowHash(c + ivec3(1, 0, 0)), w.x);
    float b = mix(fsGlowHash(c + ivec3(0, 1, 0)), fsGlowHash(c + ivec3(1, 1, 0)), w.x);
    float d = mix(fsGlowHash(c + ivec3(0, 0, 1)), fsGlowHash(c + ivec3(1, 0, 1)), w.x);
    float e = mix(fsGlowHash(c + ivec3(0, 1, 1)), fsGlowHash(c + ivec3(1, 1, 1)), w.x);
    return mix(mix(a, b, w.y), mix(d, e, w.y), w.z);
}

void main()
{
    vec2 tc = vary_fragcoord.xy;
    float depth = getDepth(tc);
    bool sky = isFarDepth(depth);

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
    bool classic = fs_scatter_look.x > 0.5;
    float variation = fs_scatter_look.y;
    float iso = fs_scatter_look.z;

    vec3 result = vec3(0.0);
    for (int i = 0; i < count; ++i)
    {
        // The light and its weight: the lights are chosen for the whole screen, and the last of
        // them fade out as they near the cut (FSClusteredLights::chooseGlowLights)
        uint entry_value = texelFetch(fs_cl_lists, first + i).r;
        int idx = int(entry_value & 0xFFFFu);
        float weight = float(entry_value >> 16u) * (1.0 / 65535.0);
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
        float falloff = light_col.a;
        float atten = 0.0;
        // The air at the closest approach scatters; the air on the way to the camera dims it
        float t_near = clamp(b, t0, t1);
        float sigma = airDensity(view_dir * t_near);
        float transmit = exp(-airDensity(view_dir * (0.5 * t_near)) * t_near);
        if (classic)
        {
            // The air's glow around a light falls off like light in fog does, with the inverse
            // square of the distance (softened inside a core of FS_SCATTER_CORE x the range, so it
            // stays finite at the light), times a window that reaches zero with zero slope at the
            // range:
            //     N * c / (c + (r/S)^2) * (1 - (r/S)^2)^2,   c = FS_SCATTER_CORE^2
            // So the glow gathers around the light with a long faint halo and no visible edge. N
            // keeps the air's total light the same as the surface falloff (1 - r/S)^2 gave.
            // Along the ray, u = H tan(theta) with H^2 = c S^2 + h^2 turns the inverse square into
            // a constant: integral of f(u) du / (H^2 + u^2) = (1 / H) integral of f dtheta. The
            // window and the phase function are smooth in theta, so four Gauss-Legendre points on
            // each side of the closest approach integrate them closely, also for rays passing
            // right by a light.
            float c_s2 = FS_SCATTER_CORE * FS_SCATTER_CORE * range2;
            float big_h = sqrt(c_s2 + h2);
            float th0 = atan((t0 - b) / big_h);
            float th1 = atan((t1 - b) / big_h);
            float th_near = clamp(0.0, th0, th1);
            float sum = 0.0;
            for (int side = 0; side < 2; ++side)
            {
                float s0 = side == 0 ? th0 : th_near;
                float s1 = side == 0 ? th_near : th1;
                float mid = 0.5 * (s0 + s1);
                float half_len = 0.5 * (s1 - s0);
                for (int k = 0; k < 4; ++k)
                {
                    float u = big_h * tan(mid + half_len * gauss_x[k]);
                    float r2 = u * u + h2;
                    float win = max(1.0 - r2 / range2, 0.0);
                    float r = sqrt(r2);
                    // toward the light from the point on the ray: cos = (b - t) / r
                    sum += gauss_w[k] * half_len * win * win * phase(-u / max(r, 1e-4), g);
                }
            }
            atten = sum * FS_SCATTER_NORM * c_s2 / big_h * 2.0 / ((1.0 + falloff) * (1.0 + falloff));
        }
        else
        {
            // The air's variation around the closest approach, so the glow is not a perfect disc
            if (variation > 0.0)
            {
                vec3 q = view_dir * t_near;
                vec3 cell = fs_scatter_noise_origin + vec3(dot(q, fs_scatter_noise_basis[0]), dot(q, fs_scatter_noise_basis[1]), dot(q, fs_scatter_noise_basis[2]));
                float n = fsGlowNoise(cell) * 0.7 + fsGlowNoise(cell * 2.0 + vec3(17.0, 5.0, 11.0)) * 0.3; // 2x: still repeats with the origin's wrap
                sigma *= max(1.0 + variation * (2.0 * n - 1.0), 0.0);
            }

            // Light of power P = FS_GLOW_POWER S^2 2 / (1 + falloff)^2 (the light's colour is its
            // brightness on surfaces, so its power grows with the area its range lights), spread
            // as P / (r^2 + core^2), the same window to zero at the range, dimmed by the air
            // between the light and the point (exp(-sigma r)), scattered toward the camera by the
            // blended phase function, less within FS_GLOW_NEAR of the camera. The same change of
            // variable as the classic look, with H^2 = core^2 + h^2.
            float core = FS_GLOW_CORE + FS_GLOW_CORE_RANGE * range;
            float big_h = sqrt(core * core + h2);
            float th0 = atan((t0 - b) / big_h);
            float th1 = atan((t1 - b) / big_h);
            float th_near = clamp(0.0, th0, th1);
            float near2 = FS_GLOW_NEAR * FS_GLOW_NEAR;
            float sum = 0.0;
            for (int side = 0; side < 2; ++side)
            {
                float s0 = side == 0 ? th0 : th_near;
                float s1 = side == 0 ? th_near : th1;
                float mid = 0.5 * (s0 + s1);
                float half_len = 0.5 * (s1 - s0);
                for (int k = 0; k < 4; ++k)
                {
                    float u = big_h * tan(mid + half_len * gauss_x[k]);
                    float r2 = u * u + h2;
                    float win = max(1.0 - r2 / range2, 0.0);
                    float r = sqrt(r2);
                    float t = b + u;
                    float near_fade = t * t / (t * t + near2);
                    float p = mix(phase(-u / max(r, 1e-4), g), 1.0, iso);
                    sum += gauss_w[k] * half_len * win * win * p * exp(-sigma * r) * near_fade;
                }
            }
            atten = sum * FS_GLOW_POWER * range2 / big_h * 2.0 / ((1.0 + falloff) * (1.0 + falloff));
        }
        if (atten <= 0.0)
        {
            continue;
        }

        result += light_col.rgb * (atten * sigma * transmit * weight);
    }

    frag_color = vec4(max(result * fs_scatter_strength, vec3(0.0)), scene_dist);
}
