/**
 * @file class1/deferred/fsShadowScreenFilterF.glsl
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
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
 * $/LicenseInfo$
 */

// Screen-space filtering of the sun shadow (FSRenderShadowScreenFilter, fsshadowscreenfilter.cpp).
// With it, the PCSS sun program keeps its blocker search but looks the shadow up with a narrow
// four tap kernel, and writes the width of the penumbra it found, as a radius in screen pixels,
// to a second target (fs_shadow_penumbra). The two passes of the light map blur (blurLightF.glsl,
// built with this file) then blur the sun channel along their direction over that radius, with
// weights that drop for pixels off this pixel's plane or facing another way, so the shadow of a
// wall doesn't bleed onto the floor or onto an avatar standing in front of it. Pixels without a
// penumbra keep the usual blur. Optionally the second pass blends the result with the last
// frame's, found by reprojection, where the history saw the same surface (FSRenderShadowScreenFilterTemporal).

uniform sampler2D lightMap;             // this pass's input
uniform vec2 delta;                     // this pass's direction, (1, 0) or (0, 1)
uniform vec2 screen_res;

uniform int fs_screen_filter;           // 1 while the sun pass wrote penumbra radii this frame
uniform sampler2D fs_shadow_penumbra;   // r: penumbra radius in screen pixels, 0 for none
uniform sampler2D fs_shadow_history;    // last frame's result: r the sun shadow, g its view depth
uniform float fs_shadow_history_weight; // weight of the history, 0 when there is none (and in the first pass)
uniform mat4 fs_shadow_to_prev_view;    // this frame's camera space to last frame's
uniform mat4 fs_shadow_proj;            // the projection (last frame's is taken to be the same)

float getDepth(vec2 pos_screen);
bool isFarDepth(float d);
vec4 getNorm(vec2 pos_screen);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);

#define FS_SF_TAPS 6    // per side

// The sun shadow of the pixel at tc (view space pos, normal norm) for this pass: blurred over the
// penumbra radius, or the usual blur's result (blurred) where there is no penumbra. range returns
// the lowest and highest value that went into it, for the history clamp.
float fsScreenFilterShadow(float blurred, vec2 tc, vec3 pos, vec3 norm, out vec2 range)
{
    range = vec2(blurred);
    if (fs_screen_filter == 0)
    {
        return blurred;
    }
    float radius = texture(fs_shadow_penumbra, tc).r;
    if (radius < 1.0)
    {
        return blurred;
    }

    float c = texture(lightMap, tc).r;
    float sum = c;
    float wsum = 1.0;
    float lo = c;
    float hi = c;

    vec2 px = delta / screen_res;   // one pixel along this pass
    float spacing = radius / float(FS_SF_TAPS);
    // interleaved gradient noise (Jimenez 2014): shifts the taps per pixel, so the gaps between
    // them don't show as bands where the radius is wide
    float jitter = fract(52.9829189 * fract(0.06711056 * gl_FragCoord.x + 0.00583715 * gl_FragCoord.y));
    float inv_two_sigma2 = 2.0 / (radius * radius);     // sigma = radius / 2
    float tol = 0.01 + 0.01 * abs(pos.z);               // metres off this pixel's plane

    for (int i = 0; i < FS_SF_TAPS; ++i)
    {
        float o = (float(i) + jitter) * spacing + 0.5;
        float gauss = exp(-o * o * inv_two_sigma2);
        for (int s = 0; s < 2; ++s)
        {
            vec2 stc = tc + px * (s == 0 ? o : -o);
            if (any(lessThan(stc, vec2(0.0))) || any(greaterThan(stc, vec2(1.0))))
            {
                continue;
            }
            float d = getDepth(stc);
            if (isFarDepth(d))
            {
                continue;
            }
            vec3 p = getPositionWithDepth(stc, d).xyz;
            float plane = abs(dot(norm, p - pos));
            float w = gauss * (1.0 - smoothstep(tol, 2.0 * tol, plane));
            if (w <= 0.0)
            {
                continue;
            }
            w *= smoothstep(0.5, 0.9, dot(getNorm(stc).xyz, norm));
            if (w <= 0.0)
            {
                continue;
            }
            float v = texture(lightMap, stc).r;
            sum += v * w;
            wsum += w;
            if (w > 0.2)
            {
                lo = min(lo, v);
                hi = max(hi, v);
            }
        }
    }

    range = vec2(lo, hi);
    return sum / wsum;
}

// shadow blended with the last frame's at the point pos, kept within range (widened a little) so a
// shadow that moved doesn't leave a trail, and dropped where the history saw another surface
float fsScreenFilterTemporal(float shadow, vec2 range, vec3 pos)
{
    if (fs_screen_filter == 0 || fs_shadow_history_weight <= 0.0)
    {
        return shadow;
    }
    vec4 prev_view = fs_shadow_to_prev_view * vec4(pos, 1.0);
    vec4 prev = fs_shadow_proj * prev_view;
    if (prev.w <= 0.0)
    {
        return shadow;
    }
    vec2 prev_tc = prev.xy / prev.w * 0.5 + 0.5;
    if (any(lessThan(prev_tc, vec2(0.0))) || any(greaterThan(prev_tc, vec2(1.0))))
    {
        return shadow;
    }
    vec2 h = texture(fs_shadow_history, prev_tc).rg;
    float prev_z = -prev_view.z;
    float rel = abs(h.g - prev_z) / max(prev_z, 0.1);
    float w = h.g > 0.0 ? fs_shadow_history_weight * (1.0 - smoothstep(0.02, 0.08, rel)) : 0.0;
    float hist = clamp(h.r, range.x - 0.08, range.y + 0.08);
    return mix(shadow, hist, w);
}
