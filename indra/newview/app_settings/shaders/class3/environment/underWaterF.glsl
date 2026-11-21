/**
 * @file class3\environment\underWaterF.glsl
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

out vec4 frag_color;

uniform sampler2D bumpMap;
uniform sampler2D exclusionTex;

#ifdef TRANSPARENT_WATER
uniform sampler2D screenTex;
#endif

uniform vec4 fogCol;
uniform vec3 lightDir;
uniform vec3 specular;
uniform float lightExp;
uniform vec2 fbScale;
uniform float refScale;
uniform float znear;
uniform float zfar;
uniform float kd;
uniform vec4 waterPlane;
uniform vec3 eyeVec;
uniform vec4 waterFogColor;
uniform vec3 waterFogColorLinear;
uniform float waterFogKS;
uniform vec2 screenRes;
uniform float fs_snell_window;    // strength of Snell's window, 0 is off

//bigWave is (refCoord.w, view.w);
in vec4 refCoord;
in vec4 littleWave;
in vec4 view;
in vec3 vary_position;

#ifdef FS_WATER_FOAM
in vec3 vary_normal;
float fsWaterFoam(vec2 screen_tc, vec3 pos, vec3 up, vec2 view_offset);
#endif

// <FS:Trish> Interactive ripples (fsWaterRipplesF.glsl)
#ifdef FS_WATER_RIPPLES
vec2 fsWaterRipples(vec2 view_offset, out float foam);
#endif
// </FS:Trish>

// <FS:Trish> Detailed foam (fsWaterFoamF.glsl, linked for the shoreline or the churned-water foam)
#if defined(FS_WATER_FOAM) || defined(FS_WATER_RIPPLE_FOAM)
#define FS_FOAM_DETAIL
vec4 fsWaterFoamDetail(vec2 view_offset, float cover, vec2 flow, float footprint);
#endif
// </FS:Trish>

vec4 applyWaterFogViewLinearNoClip(vec3 pos, vec4 color);

// <FS:Trish> Snell's window: soft rim, dispersion, and total reflection at grazing angles
// Snell's window: from below, light from above the water only gets through within the critical
// angle of the surface normal (48.6 degrees for water into air), a cone about 97 degrees wide
// centred straight above the eye, whatever way the camera faces. Toward its rim the surface
// reflects more and more (Fresnel from the water side), and past it the reflection is total: the
// underside of the surface mirrors the depths. The waves tilt the normal, so the rim ripples.

// Water's index for red, green and blue light: blue bends more, so its critical angle is a little
// smaller and the rim has a faint fringe, red on the outside.
const vec3 FS_SNELL_ETA = vec3(1.3310, 1.3335, 1.3400);
// How wide the rim's blur is, in radians: the many small wave slopes under one pixel smear it
const float FS_SNELL_RIM = 0.05;

// Fresnel transmission from water into air at cosine c to the normal, for each colour. At and
// past the critical angle the refracted cosine is 0, which makes both reflectances 1: no branch
// is needed.
vec3 fsSnellFresnel(vec3 c, vec3 eta)
{
    c = clamp(c, vec3(1e-4), vec3(1.0));
    vec3 ct = sqrt(max(vec3(1.0) - eta * eta * (1.0 - c * c), vec3(0.0)));
    vec3 rs = (eta * c - ct) / (eta * c + ct);
    vec3 rp = (eta * ct - c) / (eta * ct + c);
    return vec3(1.0) - 0.5 * (rs * rs + rp * rp);
}

// How much of the view from above gets through, per colour. footprint is how much the angle to
// the vertical changes across the pixel, so a distant rim stays smooth instead of stair-stepped.
vec3 fsSnellTransmission(vec3 pos, vec3 wavef, float footprint)
{
    vec3 n = normalize(waterPlane.xyz);
    vec3 t = cross(n, vec3(0.0, 0.0, 1.0));
    t = dot(t, t) > 1e-4 ? normalize(t) : vec3(1.0, 0.0, 0.0);
    vec3 b = cross(n, t);
    n = normalize(n + 0.12 * (wavef.x * t + wavef.y * b));

    float c = dot(normalize(pos), n);         // cosine of the angle to the normal, inside the water
    if (c <= 0.0)
    {
        // Grazing the surface far away, where a wave tilts away from the eye: still well past the
        // critical angle, so a mirror. Letting it through speckled the far waterline.
        return vec3(0.0);
    }
    float theta = acos(min(c, 1.0));
    float h = 0.5 * (FS_SNELL_RIM + footprint);
    vec3 crit = asin(vec3(1.0) / FS_SNELL_ETA);
    // Fresnel up to just inside the rim, then a smooth fall to total reflection across it
    vec3 inner = cos(min(vec3(theta), crit - h));
    vec3 rim = vec3(1.0) - smoothstep(crit - h, crit + h, vec3(theta));
    return fsSnellFresnel(inner, FS_SNELL_ETA) * rim;
}
// </FS:Trish>
void mirrorClip(vec3 position);

void main()
{
    mirrorClip(vary_position);
    vec2 screen_tc = (refCoord.xy/refCoord.z) * 0.5 + 0.5;
    float water_mask = texture(exclusionTex, screen_tc).r;

    vec4 color;

    //get detail normals
    vec3 wave1 = texture(bumpMap, vec2(refCoord.w, view.w)).xyz*2.0-1.0;
    vec3 wave2 = texture(bumpMap, littleWave.xy).xyz*2.0-1.0;
    vec3 wave3 = texture(bumpMap, littleWave.zw).xyz*2.0-1.0;
    vec3 wavef = normalize(wave1+wave2+wave3);

    // <FS:Trish> Interactive ripples tilt the underside too: the refraction of the world above
    // and the rim of Snell's window ripple around whatever moves through the surface
#ifdef FS_WATER_RIPPLES
    float fs_ripple_foam = 0.0;
    vec2 fs_ripple_slope = fsWaterRipples(view.xy, fs_ripple_foam);
    wavef = normalize(wavef - vec3(fs_ripple_slope * wavef.z, 0.0));
#endif
    // </FS:Trish>

    // <FS:Trish> Detailed foam: the pixel's size on the water (a derivative, so taken in uniform
    // control flow) and the tilt of the waves and ripples, which the foam rides
#ifdef FS_FOAM_DETAIL
    vec2 fs_foam_fw = fwidth(view.xy);
    float fs_foam_footprint = max(fs_foam_fw.x, fs_foam_fw.y);
    vec2 fs_foam_flow = wavef.xy * 0.05;
#endif
    // </FS:Trish>

    //figure out distortion vector (ripply)
    vec2 distort = screen_tc;
    distort = mix(distort, distort+wavef.xy*refScale, water_mask);

#ifdef TRANSPARENT_WATER
    vec4 fb = texture(screenTex, distort);
#else
    vec4 fb = vec4(waterFogColorLinear, 0.0);
#endif

    // <FS:Trish> Churned-water foam and the water line touch from the ripples share the
    // shoreline foam's look
//#ifdef FS_WATER_FOAM
#if defined(FS_WATER_FOAM) || defined(FS_WATER_RIPPLES)
    // Shoreline foam seen from below (fsWaterFoamF.glsl): it scatters the light coming down
    // through it, so it shows as a pale, washed out patch on the underside of the surface
    //float foam = fsWaterFoam(screen_tc, vary_position, normalize(vary_normal), view.xy) * water_mask;
    float foam = 0.0;
#ifdef FS_WATER_FOAM
    foam = fsWaterFoam(screen_tc, vary_position, normalize(vary_normal), view.xy);
#endif
#ifdef FS_WATER_RIPPLES
    foam = max(foam, fs_ripple_foam);
#endif
    foam *= water_mask;
    // </FS:Trish>
    if (foam > 0.0)
    {
        float luma = dot(fb.rgb, vec3(0.2126, 0.7152, 0.0722));
        // <FS:Trish> Detailed foam: the lace's opacity in place of the plain coverage
#ifdef FS_FOAM_DETAIL
        foam = fsWaterFoamDetail(view.xy, foam, fs_foam_flow, fs_foam_footprint).a;
#endif
        // </FS:Trish>
        fb.rgb = mix(fb.rgb, vec3(luma * 1.4 + 0.02), foam * 0.7);
    }
#endif

    if (fs_snell_window > 0.0)
    {
        // Outside the window the surface mirrors the deep water below it
        // <FS:Trish> Soft, slightly coloured rim: the pixel's footprint in angle to the vertical
        // (a derivative, fine here as the branch is on a uniform), from the flat surface so the
        // wave detail does not widen it
        //float transmit = fsSnellTransmission(vary_position, wavef);
        //transmit = mix(1.0, transmit, clamp(fs_snell_window, 0.0, 1.0) * water_mask);
        float fs_snell_cos = dot(normalize(vary_position), normalize(waterPlane.xyz));
        float fs_snell_fw = fwidth(acos(clamp(fs_snell_cos, -1.0, 1.0)));
        vec3 transmit = fsSnellTransmission(vary_position, wavef, min(fs_snell_fw, 0.1));
        transmit = mix(vec3(1.0), transmit, clamp(fs_snell_window, 0.0, 1.0) * water_mask);
        // </FS:Trish>
        fb.rgb = mix(waterFogColorLinear, fb.rgb, transmit);
    }

    fb = applyWaterFogViewLinearNoClip(vary_position, fb);

    frag_color = max(fb, vec4(0));
}
