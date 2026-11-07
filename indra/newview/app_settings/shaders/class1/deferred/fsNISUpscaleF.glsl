/**
 * @file fsNISUpscaleF.glsl
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
 * $/LicenseInfo$
 */

// The scaler below is a port of NVScaler from the NVIDIA Image Scaling SDK v1.0.3
// (github.com/NVIDIAGameWorks/NVIDIAImageScaling, NIS/NIS_Scaler.h and NIS/NIS_Config.h),
// which carries this notice:
//
// The MIT License(MIT)
//
// Copyright(c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of
// this software and associated documentation files(the "Software"), to deal in
// the Software without restriction, including without limitation the rights to
// use, copy, modify, merge, publish, distribute, sublicense, and / or sell copies of
// the Software, and to permit persons to whom the Software is furnished to do so,
// subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
// FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
// IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

/*[EXTRA_CODE_HERE]*/

// NIS present: replaces postDeferredNoDoFF.glsl (noise variant) when the post-processed scene
// is smaller than the window and FSRenderUpscaler selects NIS. Upstream is a compute shader
// that stages the luma of a source tile and its edge map in shared memory; this is a
// per-pixel port in which every output pixel fetches its own 6x6 source luma support and
// derives the 2x2 edge map it interpolates from the middle 4x4 of it. The algorithm, the
// coefficient tables and the constants (computed in fsdynamicresolution.cpp exactly like
// NVScalerUpdateConfig in SDR mode) are unchanged. The input is display-referred, tone mapped
// and gamma corrected, as NIS expects for SDR. Dither and depth are written like the stock
// present.

out vec4 frag_color;

uniform sampler2D diffuseRect;  // post-processed scene at the internal size
uniform sampler2D depthMap;

uniform vec2 screen_res;        // internal (source) size in texels

// NISConfig, from NVScalerUpdateConfig
uniform vec4 nis_detect;        // kDetectRatio, kDetectThres, kMinContrastRatio, kRatioNorm
uniform vec4 nis_sharp_y;       // kContrastBoost, kEps, kSharpStartY, kSharpScaleY
uniform vec4 nis_sharp;         // kSharpStrengthMin, kSharpStrengthScale, kSharpLimitMin, kSharpLimitScale

in vec2 vary_fragcoord;

uniform float fs_deband;    // > 0: triangular debanding dither replaces the stock noise
vec3 fsDeband(vec3 c);      // fsDebandF.glsl

#define kDetectRatio        nis_detect.x
#define kDetectThres        nis_detect.y
#define kMinContrastRatio   nis_detect.z
#define kRatioNorm          nis_detect.w
#define kContrastBoost      nis_sharp_y.x
#define kEps                nis_sharp_y.y
#define kSharpStartY        nis_sharp_y.z
#define kSharpScaleY        nis_sharp_y.w
#define kSharpStrengthMin   nis_sharp.x
#define kSharpStrengthScale nis_sharp.y
#define kSharpLimitMin      nis_sharp.z
#define kSharpLimitScale    nis_sharp.w

#define kPhaseCount 64

// coef_scale and coef_usm from NIS_Config.h: phase p uses taps 0..2 at [2p] and taps 3..5 at
// [2p + 1]. Set by setup_nis() in fsdynamicresolution.cpp: as const arrays here, indexed by
// the phase, AMD drivers took about 15 s to load the program, even from the shader cache.
uniform vec3 nis_coef_scaler[128];
uniform vec3 nis_coef_usm[128];
#define kNisCoefScaler nis_coef_scaler
#define kNisCoefUSM nis_coef_usm

// The 6x6 luma support, row major: P(i, j) is row i (y), column j (x), as p[i][j] upstream.
// Row and column 0 are the source texel two before floor(src).
#define P(i, j) p[(i) * 6 + (j)]

float getY(vec3 rgba)
{
    return 0.2126 * rgba.x + 0.7152 * rgba.y + 0.0722 * rgba.z;
}

// GetEdgeMap(p[4][4], i, j) upstream; q is the middle 4x4 of the support, so (i, j) picks
// the 3x3 around source texel (floor + j, floor + i).
#define Q(a, b) q[((a) + i) * 4 + (b) + j]
vec4 GetEdgeMap(float q[16], int i, int j)
{
    float g_0 = abs(Q(0, 0) + Q(0, 1) + Q(0, 2) - Q(2, 0) - Q(2, 1) - Q(2, 2));
    float g_45 = abs(Q(1, 0) + Q(0, 0) + Q(0, 1) - Q(2, 1) - Q(2, 2) - Q(1, 2));
    float g_90 = abs(Q(0, 0) + Q(1, 0) + Q(2, 0) - Q(0, 2) - Q(1, 2) - Q(2, 2));
    float g_135 = abs(Q(1, 0) + Q(2, 0) + Q(2, 1) - Q(0, 1) - Q(0, 2) - Q(1, 2));

    float g_0_90_max = max(g_0, g_90);
    float g_0_90_min = min(g_0, g_90);
    float g_45_135_max = max(g_45, g_135);
    float g_45_135_min = min(g_45, g_135);

    if (g_0_90_max + g_45_135_max == 0.0)
    {
        return vec4(0.0);
    }

    float e_0_90 = min(g_0_90_max / (g_0_90_max + g_45_135_max), 1.0);
    float e_45_135 = 1.0 - e_0_90;

    bool c_0_90 = (g_0_90_max > (g_0_90_min * kDetectRatio)) && (g_0_90_max > kDetectThres) && (g_0_90_max > g_45_135_min);
    bool c_45_135 = (g_45_135_max > (g_45_135_min * kDetectRatio)) && (g_45_135_max > kDetectThres) && (g_45_135_max > g_0_90_min);
    bool c_g_0_90 = g_0_90_max == g_0;
    bool c_g_45_135 = g_45_135_max == g_45;

    float f_e_0_90 = (c_0_90 && c_45_135) ? e_0_90 : 1.0;
    float f_e_45_135 = (c_0_90 && c_45_135) ? e_45_135 : 1.0;

    float weight_0 = (c_0_90 && c_g_0_90) ? f_e_0_90 : 0.0;
    float weight_90 = (c_0_90 && !c_g_0_90) ? f_e_0_90 : 0.0;
    float weight_45 = (c_45_135 && c_g_45_135) ? f_e_45_135 : 0.0;
    float weight_135 = (c_45_135 && !c_g_45_135) ? f_e_45_135 : 0.0;

    return vec4(weight_0, weight_90, weight_45, weight_135);
}
#undef Q

float CalcLTI(float p0, float p1, float p2, float p3, float p4, float p5, int phase_index)
{
    bool selector = (phase_index <= kPhaseCount / 2);
    float sel = selector ? p0 : p3;
    float a_min = min(min(p1, p2), sel);
    float a_max = max(max(p1, p2), sel);
    sel = selector ? p2 : p5;
    float b_min = min(min(p3, p4), sel);
    float b_max = max(max(p3, p4), sel);

    float a_cont = a_max - a_min;
    float b_cont = b_max - b_min;

    float cont_ratio = max(a_cont, b_cont) / (min(a_cont, b_cont) + kEps);
    return (1.0 - clamp((cont_ratio - kMinContrastRatio) * kRatioNorm, 0.0, 1.0)) * kContrastBoost;
}

vec4 GetInterpEdgeMap(vec4 e00, vec4 e01, vec4 e10, vec4 e11, float phase_frac_x, float phase_frac_y)
{
    vec4 h0 = mix(e00, e01, phase_frac_x);
    vec4 h1 = mix(e10, e11, phase_frac_x);
    return mix(h0, h1, phase_frac_y);
}

float EvalPoly6(float pxl[6], int phase_int)
{
    phase_int = clamp(phase_int, 0, kPhaseCount - 1);
    vec3 lo = vec3(pxl[0], pxl[1], pxl[2]);
    vec3 hi = vec3(pxl[3], pxl[4], pxl[5]);
    float y = dot(kNisCoefScaler[phase_int * 2], lo) + dot(kNisCoefScaler[phase_int * 2 + 1], hi);
    float y_usm = dot(kNisCoefUSM[phase_int * 2], lo) + dot(kNisCoefUSM[phase_int * 2 + 1], hi);

    // let's compute a piece-wise ramp based on luma
    float y_scale = 1.0 - clamp((y - kSharpStartY) * kSharpScaleY, 0.0, 1.0);

    // scale the ramp to sharpen as a function of luma
    float y_sharpness = y_scale * kSharpStrengthScale + kSharpStrengthMin;

    y_usm *= y_sharpness;

    // scale the ramp to limit USM as a function of luma
    float y_sharpness_limit = (y_scale * kSharpLimitScale + kSharpLimitMin) * y;

    y_usm = min(y_sharpness_limit, max(-y_sharpness_limit, y_usm));
    // reduce ringing
    y_usm *= CalcLTI(pxl[0], pxl[1], pxl[2], pxl[3], pxl[4], pxl[5], phase_int);

    return y + y_usm;
}

float FilterNormal(float p[36], int phase_x_frac_int, int phase_y_frac_int)
{
    vec3 cy0 = kNisCoefScaler[phase_y_frac_int * 2];
    vec3 cy1 = kNisCoefScaler[phase_y_frac_int * 2 + 1];
    vec3 cx0 = kNisCoefScaler[phase_x_frac_int * 2];
    vec3 cx1 = kNisCoefScaler[phase_x_frac_int * 2 + 1];
    float cx[6] = float[6](cx0.x, cx0.y, cx0.z, cx1.x, cx1.y, cx1.z);
    float h_acc = 0.0;
    for (int j = 0; j < 6; ++j)
    {
        float v_acc = dot(vec3(P(0, j), P(1, j), P(2, j)), cy0) + dot(vec3(P(3, j), P(4, j), P(5, j)), cy1);
        h_acc += v_acc * cx[j];
    }
    return h_acc;
}

float AddDirFilters(float p[36], float phase_x_frac, float phase_y_frac, int phase_x_frac_int, int phase_y_frac_int, vec4 w)
{
    float f = 0.0;
    if (w.x > 0.0)
    {
        // 0 deg filter
        float interp0Deg[6];
        for (int i = 0; i < 6; ++i)
        {
            interp0Deg[i] = mix(P(i, 2), P(i, 3), phase_x_frac);
        }
        f += EvalPoly6(interp0Deg, phase_y_frac_int) * w.x;
    }
    if (w.y > 0.0)
    {
        // 90 deg filter
        float interp90Deg[6];
        for (int i = 0; i < 6; ++i)
        {
            interp90Deg[i] = mix(P(2, i), P(3, i), phase_y_frac);
        }
        f += EvalPoly6(interp90Deg, phase_x_frac_int) * w.y;
    }
    if (w.z > 0.0)
    {
        // 45 deg filter
        float pphase_b45 = 0.5 + 0.5 * (phase_x_frac - phase_y_frac);

        float temp_interp45Deg[7];
        temp_interp45Deg[1] = mix(P(2, 1), P(1, 2), pphase_b45);
        temp_interp45Deg[3] = mix(P(3, 2), P(2, 3), pphase_b45);
        temp_interp45Deg[5] = mix(P(4, 3), P(3, 4), pphase_b45);
        {
            pphase_b45 = pphase_b45 - 0.5;
            float a = (pphase_b45 >= 0.0) ? P(0, 2) : P(2, 0);
            float b = (pphase_b45 >= 0.0) ? P(1, 3) : P(3, 1);
            float c = (pphase_b45 >= 0.0) ? P(2, 4) : P(4, 2);
            float d = (pphase_b45 >= 0.0) ? P(3, 5) : P(5, 3);
            temp_interp45Deg[0] = mix(P(1, 1), a, abs(pphase_b45));
            temp_interp45Deg[2] = mix(P(2, 2), b, abs(pphase_b45));
            temp_interp45Deg[4] = mix(P(3, 3), c, abs(pphase_b45));
            temp_interp45Deg[6] = mix(P(4, 4), d, abs(pphase_b45));
        }

        float interp45Deg[6];
        float pphase_p45 = phase_x_frac + phase_y_frac;
        int shift45 = 0;
        if (pphase_p45 >= 1.0)
        {
            shift45 = 1;
            pphase_p45 = pphase_p45 - 1.0;
        }
        for (int i = 0; i < 6; i++)
        {
            interp45Deg[i] = temp_interp45Deg[i + shift45];
        }

        f += EvalPoly6(interp45Deg, int(pphase_p45 * 64.0)) * w.z;
    }
    if (w.w > 0.0)
    {
        // 135 deg filter
        float pphase_b135 = 0.5 * (phase_x_frac + phase_y_frac);

        float temp_interp135Deg[7];
        temp_interp135Deg[1] = mix(P(3, 1), P(4, 2), pphase_b135);
        temp_interp135Deg[3] = mix(P(2, 2), P(3, 3), pphase_b135);
        temp_interp135Deg[5] = mix(P(1, 3), P(2, 4), pphase_b135);
        {
            pphase_b135 = pphase_b135 - 0.5;
            float a = (pphase_b135 >= 0.0) ? P(5, 2) : P(3, 0);
            float b = (pphase_b135 >= 0.0) ? P(4, 3) : P(2, 1);
            float c = (pphase_b135 >= 0.0) ? P(3, 4) : P(1, 2);
            float d = (pphase_b135 >= 0.0) ? P(2, 5) : P(0, 3);
            temp_interp135Deg[0] = mix(P(4, 1), a, abs(pphase_b135));
            temp_interp135Deg[2] = mix(P(3, 2), b, abs(pphase_b135));
            temp_interp135Deg[4] = mix(P(2, 3), c, abs(pphase_b135));
            temp_interp135Deg[6] = mix(P(1, 4), d, abs(pphase_b135));
        }

        float interp135Deg[6];
        float pphase_p135 = 1.0 + (phase_x_frac - phase_y_frac);
        int shift135 = 0;
        if (pphase_p135 >= 1.0)
        {
            shift135 = 1;
            pphase_p135 = pphase_p135 - 1.0;
        }
        for (int i = 0; i < 6; ++i)
        {
            interp135Deg[i] = temp_interp135Deg[i + shift135];
        }

        f += EvalPoly6(interp135Deg, int(pphase_p135 * 64.0)) * w.w;
    }
    return f;
}

// Dither noise as the stock present adds it
float hash(vec2 p) { return fract(1e4 * sin(17.0 * p.x + p.y * 0.1) * (0.1 + abs(sin(p.y * 13.0 + p.x)))); }

float noise(vec2 x)
{
    vec2 i = floor(x);
    vec2 f = fract(x);
    float a = hash(i);
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(a, b, u.x) + (c - a) * u.y * (1.0 - u.x) + (d - b) * u.x * u.y;
}

void main()
{
    vec2 uv = vary_fragcoord.xy;

    // Source position of this output pixel, (0.5 + dst) * kScale - 0.5 upstream
    vec2 src = uv * screen_res - 0.5;
    vec2 src_floor = floor(src);
    float fx = src.x - src_floor.x;
    float fy = src.y - src_floor.y;
    int fx_int = clamp(int(fx * float(kPhaseCount)), 0, kPhaseCount - 1);
    int fy_int = clamp(int(fy * float(kPhaseCount)), 0, kPhaseCount - 1);

    // The 6x6 luma support, texels floor - 2 .. floor + 3, clamped to the edge like the
    // linear clamp sampler upstream
    ivec2 size_max = ivec2(screen_res) - 1;
    ivec2 base = ivec2(src_floor) - 2;
    float p[36];
    for (int i = 0; i < 6; ++i)
    {
        int ty = clamp(base.y + i, 0, size_max.y);
        for (int j = 0; j < 6; ++j)
        {
            int tx = clamp(base.x + j, 0, size_max.x);
            P(i, j) = getY(texelFetch(diffuseRect, ivec2(tx, ty), 0).rgb);
        }
    }

    // Edge map of the 2x2 texels around the sample, from the middle 4x4 of the support
    float q[16];
    for (int i = 0; i < 4; ++i)
    {
        for (int j = 0; j < 4; ++j)
        {
            q[i * 4 + j] = P(i + 1, j + 1);
        }
    }
    vec4 w = GetInterpEdgeMap(GetEdgeMap(q, 0, 0), GetEdgeMap(q, 0, 1),
                              GetEdgeMap(q, 1, 0), GetEdgeMap(q, 1, 1), fx, fy);

    // weight for luma
    float baseWeight = 1.0 - w.x - w.y - w.z - w.w;

    // final luma is a weighted product of directional & normal filters
    float opY = 0.0;
    opY += FilterNormal(p, fx_int, fy_int) * baseWeight;
    opY += AddDirFilters(p, fx, fy, fx_int, fy_int, w);

    // do bilinear tap for chroma upscaling, then move it to the new luma
    vec4 op = texture(diffuseRect, uv);
    vec3 color = op.rgb + (opY - getY(op.rgb));

    if (fs_deband > 0.0)
    {
        // Triangular debanding dither (fsDebandF.glsl) in place of the stock noise
        color = fsDeband(clamp(color, vec3(0.0), vec3(1.0)));
    }
    else
    {
        vec2 tc = uv * screen_res * 4.0;
        vec3 seed = (color + vec3(1.0)) * vec3(tc.xy, tc.x + tc.y);
        color += vec3(noise(seed.rg), noise(seed.gb), noise(seed.rb)) * 0.003;
    }

    // Same guard as clampHDRRange in the stock present
    color = mix(color, vec3(0.0), isnan(color));
    frag_color = vec4(clamp(color, vec3(0.0), vec3(11.2)), op.a);

    gl_FragDepth = texture(depthMap, uv).r;
}
