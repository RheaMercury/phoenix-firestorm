/**
 * @file class1/interface/fsProbeBlendF.glsl
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

/*[EXTRA_CODE_HERE]*/

// Reflection probe refresh blend (FSRenderProbeRefreshBlend, fsprobeblend.cpp): one face of one mip
// of a probe's filtered cube, mixed from the maps it had before its refresh (cube fs_blend_old) to the
// new ones (cube fs_blend_new), both kept in the spare cubes at the end of the probe cube array. Drawn
// the way radianceGenF.glsl is (radianceGenV.glsl, the same quad and face rotation), so each fragment
// reads the texel it replaces.

out vec4 frag_color;

uniform samplerCubeArray reflectionProbes;
uniform int fs_blend_old;
uniform int fs_blend_new;
uniform float fs_blend_t;   // 0 all old, 1 all new
uniform float mipLevel;

in vec3 vary_dir;

void main()
{
    vec3 dir = normalize(vary_dir);
    vec4 a = textureLod(reflectionProbes, vec4(dir, float(fs_blend_old)), mipLevel);
    vec4 b = textureLod(reflectionProbes, vec4(dir, float(fs_blend_new)), mipLevel);
    frag_color = max(mix(a, b, clamp(fs_blend_t, 0.0, 1.0)), vec4(0.0));
}
