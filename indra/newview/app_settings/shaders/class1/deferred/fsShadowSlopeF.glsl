/**
 * @file class1/deferred/fsShadowSlopeF.glsl
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

// Sun shadow bias from the depth buffer's slope (FSRenderShadowSlopeBias), for the deferred sun
// programs (sunLightF / sunLightSSAOF, permutation FS_SHADOW_DEPTH_SLOPE). The sun shadow lookup
// (shadowUtil.glsl) lowers each tap's reference depth by the receiver's depth slope in the shadow
// map, which it measures along the normal it is given, and offsets the lookup toward the light
// by the angle between that normal and the light. With the g-buffer's shading normal, normal maps
// tilt that plane per pixel: the slope comes out wrong (acne on bumpy walls, or too much bias and
// light leaking under contact points) and the offset follows the bumps. Here the normal is that
// of the surface as drawn, from the positions of the neighbouring pixels, so the per-tap bias
// matches the real receiver plane and the constant bias can be smaller (the C++ side scales it by
// FSRenderShadowSlopeBiasConstant), so shadows sit closer to their casters.

uniform vec2 screen_res;
uniform int cube_snapshot;

float getDepth(vec2 pos_screen);
bool isFarDepth(float d);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);

// view space position of the pixel at tc, and whether it is sky
vec3 fsSlopeNeighbour(vec2 tc, out bool sky)
{
    float d = getDepth(tc);
    sky = isFarDepth(d);
    return getPositionWithDepth(tc, d).xyz;
}

// Geometric normal (view space, facing the camera) of the surface at tc, view space position pos.
// Of the two neighbours along each screen axis, the one whose depth continues this pixel's better
// is used, so a pixel at an object's edge takes the slope of its own surface. A pixel with no
// such neighbour on an axis (a sliver one pixel wide, sky around) keeps fallback.
vec3 fsDepthSlopeNormal(vec2 tc, vec3 pos, vec3 fallback)
{
    if (cube_snapshot != 0)
    {
        return fallback;
    }
    vec2 px = 1.0 / screen_res;
    bool sl, sr, sd, su;
    vec3 pl = fsSlopeNeighbour(tc - vec2(px.x, 0.0), sl);
    vec3 pr = fsSlopeNeighbour(tc + vec2(px.x, 0.0), sr);
    vec3 pd = fsSlopeNeighbour(tc - vec2(0.0, px.y), sd);
    vec3 pu = fsSlopeNeighbour(tc + vec2(0.0, px.y), su);

    float el = sl ? 1.0e20 : abs(pos.z - pl.z);
    float er = sr ? 1.0e20 : abs(pr.z - pos.z);
    float ed = sd ? 1.0e20 : abs(pos.z - pd.z);
    float eu = su ? 1.0e20 : abs(pu.z - pos.z);

    // a step bigger than this along both neighbours of an axis is an edge on both sides; a
    // surface seen at a grazing angle steps by up to about this much per pixel at its distance
    float limit = 0.1 * abs(pos.z) + 0.1;
    if (min(el, er) > limit || min(ed, eu) > limit)
    {
        return fallback;
    }

    vec3 dx = er < el ? pr - pos : pos - pl;
    vec3 dy = eu < ed ? pu - pos : pos - pd;
    vec3 n = cross(dx, dy);
    float len = length(n);
    if (len < 1.0e-12)
    {
        return fallback;
    }
    n /= len;
    return dot(n, pos) > 0.0 ? -n : n;
}
