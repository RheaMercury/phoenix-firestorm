/**
 * @file class1/interface/fsProbeBlendSHF.glsl
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

// Reflection probe refresh blend (FSRenderProbeRefreshBlend, fsprobeblend.cpp), irradiance: a probe's
// row of nine SH coefficients, mixed from the row it had before its refresh (row 0 of diffuseRect) to
// the new one (row 1). The viewport is the probe's row, so gl_FragCoord.x picks the coefficient, as
// in shProjectF.glsl. SH coefficients are linear in the light, so mixing them mixes the irradiance.

out vec4 frag_color;

uniform sampler2D diffuseRect;
uniform float fs_blend_t;   // 0 all old, 1 all new

// the shared vertex stage emits it
in vec3 vary_dir;

void main()
{
    int x = int(gl_FragCoord.x);
    vec4 a = texelFetch(diffuseRect, ivec2(x, 0), 0);
    vec4 b = texelFetch(diffuseRect, ivec2(x, 1), 0);
    frag_color = mix(a, b, clamp(fs_blend_t, 0.0, 1.0));
}
