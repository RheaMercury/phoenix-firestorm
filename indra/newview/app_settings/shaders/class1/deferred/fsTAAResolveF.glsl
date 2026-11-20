/**
 * @file fsTAAResolveF.glsl
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

/*[EXTRA_CODE_HERE]*/

// Temporal anti-aliasing resolve (FSRenderTAA, fstaa.h). Blends this frame's jittered HDR scene
// with last frame's result, reprojected by the camera's motion from depth. Writes the result's
// colour and, in alpha, the pixel's linear view depth, which the next frame's disocclusion test
// reads back.

out vec4 frag_color;

uniform sampler2D diffuseRect;      // this frame's HDR scene, rasterised with the jitter
uniform sampler2D depthMap;         // its depth
uniform sampler2D exposureMap;      // last frame's auto exposure, 1x1
uniform sampler2D sceneMap;         // the scene before blended alpha (fs_taa_pre_alpha.x)
uniform sampler2D sceneDepth;       // the depth before alpha (fs_taa_pre_alpha.y)
uniform sampler2D fs_taa_history;   // last frame's result: rgb, a = linear view depth in metres

uniform vec2 screen_res;
uniform vec2 fs_taa_jitter;         // this frame's jitter, in pixels
uniform mat4 fs_taa_inv_proj;       // this frame's NDC, as rasterised -> view space
uniform mat4 fs_taa_reproject;      // this frame's view space -> last frame's clip space
uniform float fs_taa_blend;         // weight of this frame where the history is good
uniform float fs_taa_reactive;      // weight of this frame where blended alpha or water was drawn
uniform float fs_taa_reset;         // 1: no history
uniform vec2 fs_taa_pre_alpha;      // x: sceneMap holds this frame, y: sceneDepth does
uniform vec4 fs_taa_debug;          // x: FSRenderTAADebug view (0 off), y: this frame's jitter phase (0 to 7)

const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);

// NaN or infinity anywhere would stay in the history for good
vec3 finiteColor(vec3 c)
{
    if (any(isnan(c)) || any(isinf(c)))
    {
        return vec3(0.0);
    }
    return clamp(c, vec3(0.0), vec3(65000.0));
}

// Colours are blended compressed by 1 / (1 + exposed luminance): about what the tone mapper
// will do to them, so a bright highlight can't outweigh everything around it
vec3 compress(vec3 c, float e)
{
    return c / (1.0 + dot(c, LUMA) * e);
}

vec3 uncompress(vec3 c, float e)
{
    return c / max(1.0 - dot(c, LUMA) * e, 1e-3);
}

vec3 toYCoCg(vec3 c)
{
    return vec3(dot(c, vec3(0.25, 0.5, 0.25)), dot(c, vec3(0.5, 0.0, -0.5)), dot(c, vec3(-0.25, 0.5, -0.25)));
}

vec3 fromYCoCg(vec3 c)
{
    return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

// The point of the box nearest to q along the line to its centre (clipping, not clamping, keeps
// the hue of the history)
vec3 clipToBox(vec3 q, vec3 lo, vec3 hi)
{
    vec3 centre = 0.5 * (hi + lo);
    vec3 extent = 0.5 * (hi - lo) + 1e-5;
    vec3 v = q - centre;
    vec3 a = abs(v / extent);
    float m = max(a.x, max(a.y, a.z));
    return m > 1.0 ? centre + v / m : q;
}

bool closer(float a, float b)
{
#ifdef REVERSE_Z
    return a > b;
#else
    return a < b;
#endif
}

vec3 viewPosition(vec2 pixel_centre, float depth)
{
#ifdef REVERSE_Z
    float z = depth;                // glClipControl(GL_ZERO_TO_ONE): window depth is NDC depth
#else
    float z = depth * 2.0 - 1.0;
#endif
    vec4 ndc = vec4(pixel_centre / screen_res * 2.0 - 1.0, z, 1.0);
    vec4 v = fs_taa_inv_proj * ndc;
    return v.xyz / v.w;
}

// Catmull-Rom from five bilinear taps (the four corners weigh next to nothing)
vec3 historyCatmullRom(vec2 uv)
{
    vec2 sample_pos = uv * screen_res;
    vec2 tc1 = floor(sample_pos - 0.5) + 0.5;
    vec2 f = sample_pos - tc1;

    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;

    vec2 tc0 = (tc1 - 1.0) / screen_res;
    vec2 tc3 = (tc1 + 2.0) / screen_res;
    vec2 tc12 = (tc1 + w2 / w12) / screen_res;

    vec3 result = texture(fs_taa_history, vec2(tc12.x, tc0.y)).rgb * (w12.x * w0.y);
    result += texture(fs_taa_history, vec2(tc0.x, tc12.y)).rgb * (w0.x * w12.y);
    result += texture(fs_taa_history, tc12).rgb * (w12.x * w12.y);
    result += texture(fs_taa_history, vec2(tc3.x, tc12.y)).rgb * (w3.x * w12.y);
    result += texture(fs_taa_history, vec2(tc12.x, tc3.y)).rgb * (w12.x * w3.y);
    float weight = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return result / max(weight, 1e-4);
}

// Linear view depth of the texel p of this frame (clamped to the screen)
float linearDepthAt(ivec2 p, ivec2 size)
{
    ivec2 q = clamp(p, ivec2(0), size - 1);
    return max(-viewPosition(vec2(q) + 0.5, texelFetch(depthMap, q, 0).r).z, 0.0);
}

// FSRenderTAADebug 4: this frame's jitter, drawn in the top left corner over the dimmed scene. A
// row of 8 cells, the lit one is this frame's phase (it must step through all 8 in a video or
// across screenshots), and a box that stands for one pixel with a dot where the jitter put the
// sample this frame (its centre is the pixel's centre; the dot must stay inside the box and land
// somewhere else every frame).
vec3 debugJitter(vec2 frag, vec3 scene)
{
    vec2 p = vec2(frag.x, screen_res.y - frag.y);  // from the top left corner, y down
    vec3 col = scene;
    for (int i = 0; i < 8; ++i)
    {
        vec2 lo = vec2(16.0 + float(i) * 28.0, 16.0);
        if (all(greaterThanEqual(p, lo)) && all(lessThan(p, lo + vec2(24.0))))
        {
            col = (abs(float(i) - fs_taa_debug.y) < 0.5) ? vec3(1.0, 1.0, 0.2) : vec3(0.08);
        }
    }
    vec2 box_lo = vec2(16.0, 56.0);
    vec2 box_hi = box_lo + vec2(128.0);
    if (all(greaterThanEqual(p, box_lo)) && all(lessThan(p, box_hi)))
    {
        vec2 q = p - box_lo;
        bool border = any(lessThan(q, vec2(2.0))) || any(greaterThan(q, vec2(126.0)));
        bool cross = abs(q.x - 64.0) < 0.5 || abs(q.y - 64.0) < 0.5;
        col = border ? vec3(1.0) : (cross ? vec3(0.3) : vec3(0.02));
        vec2 dot_pos = vec2(64.0) + vec2(fs_taa_jitter.x, -fs_taa_jitter.y) * 128.0;
        if (length(q - dot_pos) < 6.0)
        {
            col = vec3(1.0, 0.1, 0.1);
        }
    }
    return col;
}

void main()
{
    ivec2 size = ivec2(screen_res);
    ivec2 pix = clamp(ivec2(gl_FragCoord.xy), ivec2(0), size - 1);
    vec2 centre = vec2(pix) + 0.5;

    float e = texture(exposureMap, vec2(0.5)).r;
    if (isnan(e) || isinf(e) || e <= 0.0)
    {
        e = 1.0;
    }
    e = clamp(e, 1e-3, 1e3);

    // This frame's 3x3 neighbourhood: its colour box, its colours for the reconstruction below,
    // and the closest depth
    vec3 m1 = vec3(0.0);
    vec3 m2 = vec3(0.0);
    vec3 box_min = vec3(1e9);
    vec3 box_max = vec3(-1e9);
    vec3 samples[9];
    float centre_depth = texelFetch(depthMap, pix, 0).r;
    float best_depth = centre_depth;
    ivec2 best_pix = pix;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            ivec2 p = clamp(pix + ivec2(x, y), ivec2(0), size - 1);
            vec3 c = compress(finiteColor(texelFetch(diffuseRect, p, 0).rgb), e);
            samples[(y + 1) * 3 + (x + 1)] = c;
            vec3 ycocg = toYCoCg(c);
            m1 += ycocg;
            m2 += ycocg * ycocg;
            box_min = min(box_min, ycocg);
            box_max = max(box_max, ycocg);

            float z = texelFetch(depthMap, p, 0).r;
            if (closer(z, best_depth))
            {
                best_depth = z;
                best_pix = p;
            }
        }
    }

    // Linear depth of this pixel, kept with the result for the next frame
    vec3 centre_view = viewPosition(centre, centre_depth);
    float cur_linear = max(-centre_view.z, 0.0);

    bool use_history = fs_taa_reset < 0.5;

    // Where the closest surface around was last frame: its motion moves this pixel
    vec3 best_view = viewPosition(vec2(best_pix) + 0.5, best_depth);
    vec4 best_prev = fs_taa_reproject * vec4(best_view, 1.0);
    vec2 velocity = vec2(0.0);
    if (best_prev.w > 1e-4)
    {
        vec2 prev_uv = best_prev.xy / best_prev.w * 0.5 + 0.5;
        vec2 cur_uv = (vec2(best_pix) + 0.5 - fs_taa_jitter) / screen_res;   // that sample, unjittered
        velocity = prev_uv - cur_uv;
    }
    else
    {
        use_history = false;    // behind last frame's camera
    }
    vec2 history_uv = centre / screen_res + velocity;
    if (any(lessThan(history_uv, vec2(0.0))) || any(greaterThan(history_uv, vec2(1.0))))
    {
        use_history = false;    // off screen last frame
    }
    if (any(isnan(velocity)) || any(isinf(velocity)))
    {
        use_history = false;
        velocity = vec2(0.0);
    }

    // Motion here, in pixels: fast motion blurs the resampled history, so it gets less weight.
    // still: the pixel moved under a tenth of a pixel, the history lies on it texel for texel,
    // and any difference from this frame is the jitter's or a real change.
    float speed = length(velocity * screen_res);
    float still = 1.0 - smoothstep(0.1, 0.5, speed);

    // This pixel from this frame's samples, each weighed by its distance from the pixel's centre
    // where the jitter really put it (Gaussian fit of Blackman-Harris). Where still, a narrow one
    // (sigma 0.35 px): the history then averages the jitter's 8 positions into a box-filtered,
    // supersampled pixel, and a wide kernel on top of that only blurs (sigma 0.47 px gave a 1.2
    // px wide edge, this one 0.8 px, the same as the unfiltered samples).
    float k = mix(2.29, 4.0, still);
    vec3 filtered = vec3(0.0);
    float filter_weight = 0.0;
    for (int i = 0; i < 9; ++i)
    {
        vec2 d = vec2(float(i % 3 - 1), float(i / 3 - 1)) - fs_taa_jitter;  // the sample's distance from this pixel's centre
        float w = exp(-k * dot(d, d));
        filtered += samples[i] * w;
        filter_weight += w;
    }
    filtered /= max(filter_weight, 1e-4);

    float alpha = 1.0;
    vec3 history = filtered;

    // What decided the weight, for FSRenderTAADebug
    float dbg_disocclusion = 0.0;
    float dbg_clip = 0.0;
    float dbg_reactive = 0.0;
    float dbg_moved = 0.0;

    if (use_history)
    {
        // Disocclusion: the depth this pixel's own surface had last frame against the depths the
        // history holds around where it lands. Any of the 3x3 texels around it matching keeps it:
        // the jitter moves edges by up to a pixel either way from frame to frame.
        vec4 centre_prev = fs_taa_reproject * vec4(centre_view, 1.0);
        float expected = centre_prev.w;     // last frame's linear depth: w = -z in view space
        vec2 hist_pos = clamp(history_uv * screen_res - 0.5, vec2(0.0), screen_res - 1.0);
        ivec2 hc = ivec2(floor(hist_pos + 0.5));
        float miss = 1e9;
        for (int hy = -1; hy <= 1; ++hy)
        {
            for (int hx = -1; hx <= 1; ++hx)
            {
                ivec2 q = clamp(hc + ivec2(hx, hy), ivec2(0), size - 1);
                miss = min(miss, abs(texelFetch(fs_taa_history, q, 0).a - expected));
            }
        }
        // The tolerance also allows for the surface's own depth slope: each history texel holds
        // the depth where last frame's jitter put its sample, up to half a pixel from where this
        // frame's lands even with the 3x3 search. Ground seen at a grazing angle changes depth by
        // about d^2 * (pixel angle) / (eye height) per pixel: 15 m per pixel at 200 m, 35 m at
        // 300 m for an eye 2.5 m up at 1080p, against a tolerance of 6 and 9 m, so distant land
        // dropped its history, partly at 200 m and entirely from about 300 m, and showed the raw
        // jittered frame. The slope is the smaller of the two one-sided differences on each axis,
        // so a silhouette (a jump on one side only) still counts as a jump.
        float slope_x = min(abs(linearDepthAt(pix + ivec2(1, 0), size) - cur_linear), abs(linearDepthAt(pix - ivec2(1, 0), size) - cur_linear));
        float slope_y = min(abs(linearDepthAt(pix + ivec2(0, 1), size) - cur_linear), abs(linearDepthAt(pix - ivec2(0, 1), size) - cur_linear));
        float tolerance = 0.05 + 0.03 * max(expected, 0.0) + slope_x + slope_y;
        float disocclusion = smoothstep(1.0, 3.0, miss / tolerance);
        if (isnan(disocclusion))
        {
            disocclusion = 1.0;
        }

        history = compress(finiteColor(historyCatmullRom(history_uv)), e);

        // Variance box of this frame's neighbourhood (1 sigma), within its min and max. Where
        // still it widens to the neighbourhood's whole min and max: the jitter brings thin and
        // distant detail into some frames' samples and not others', which the variance box
        // clipped away and back from frame to frame, so it shimmered.
        vec3 mean = m1 / 9.0;
        vec3 sigma = sqrt(max(m2 / 9.0 - mean * mean, vec3(0.0)));
        vec3 lo_var = max(mean - sigma, box_min);
        vec3 hi_var = min(mean + sigma, box_max);
        vec3 hist_ycocg = toYCoCg(history);
        vec3 clipped_ycocg = clipToBox(hist_ycocg, mix(lo_var, box_min, still), mix(hi_var, box_max, still));
        history = fromYCoCg(clipped_ycocg);

        // History far outside what this frame shows here: something moved by itself (stage 1
        // knows only the camera's motion). Take more of this frame, but not all of it, so
        // edges that merely flicker in the jitter stay smooth. While the camera moves, measured
        // against the variance box. Where still, only what lies outside everything the
        // neighbourhood shows, by a tolerance of its own brightness.
        vec3 clipped_var = clipToBox(hist_ycocg, lo_var, hi_var);
        float outside = length((hist_ycocg - clipped_var) / max(hi_var - lo_var, vec3(0.01)));
        float moved_var = clamp((outside - 0.5) / 1.5, 0.0, 1.0);
        float tol = 0.01 + 0.05 * max(mean.x, 0.0);
        vec3 beyond = max(max(box_min - hist_ycocg, hist_ycocg - box_max), vec3(0.0));
        float changed = smoothstep(1.0, 4.0, length(beyond) / tol);
        vec3 beyond_var = max(max(lo_var - hist_ycocg, hist_ycocg - hi_var), vec3(0.0));
        float changed_var = smoothstep(1.0, 4.0, length(beyond_var) / tol);
        float moved = mix(moved_var, changed, still);

        // Reactive: blended alpha, particles and water drawn after the opaque scene. What only
        // changed the colour (particles, blended alpha) gets a softer response than what wrote
        // depth over the opaque scene (water, rigged alpha). Where still, only as far as the
        // colour really changed.
        float reactive = 0.0;
        bool depth_reactive = false;
        if (fs_taa_pre_alpha.x > 0.5)
        {
            vec3 before = compress(finiteColor(texelFetch(sceneMap, pix, 0).rgb), e);
            vec3 now = compress(finiteColor(texelFetch(diffuseRect, pix, 0).rgb), e);
            reactive = 0.6 * smoothstep(0.004, 0.03, length(now - before));
        }
        if (fs_taa_pre_alpha.y > 0.5 && texelFetch(sceneDepth, pix, 0).r != centre_depth)
        {
            reactive = 1.0;     // something wrote depth over the opaque scene: water, rigged alpha
            depth_reactive = true;
        }
        reactive *= mix(1.0, depth_reactive ? changed_var : changed, still);

        // Where still, a quarter of this frame's weight: an exponential average of the jitter's 8
        // positions keeps a ripple with the 8 frame cycle. On a hard edge, with the filter above,
        // that ripple is 4.3% of the edge's contrast at 0.05 and 2.1% at 0.025.
        float base = mix(fs_taa_blend, max(fs_taa_blend * 0.25, min(fs_taa_blend, 0.015)), still);
        alpha = base + 0.15 * clamp(speed / 40.0, 0.0, 1.0);
        alpha = max(alpha, 0.5 * moved);
        alpha = max(alpha, reactive * fs_taa_reactive);
        alpha = max(alpha, disocclusion);

        dbg_disocclusion = disocclusion;
        dbg_clip = smoothstep(1.0, 4.0, length(hist_ycocg - clipped_ycocg) / tol);
        dbg_reactive = reactive * fs_taa_reactive;
        dbg_moved = 0.5 * moved;
    }

    alpha = clamp(alpha, 0.0, 1.0);
    vec3 result = uncompress(mix(history, filtered, alpha), e);

    if (fs_taa_debug.x > 0.5)
    {
        // FSRenderTAADebug: false colours in place of the result (only into the debug target, the
        // history is always the result above). Divided by the exposure, so after tone mapping
        // they come out about as chosen.
        float grey = 0.15 * dot(compress(finiteColor(texelFetch(diffuseRect, pix, 0).rgb), e), LUMA);
        vec3 dbg = vec3(grey);
        int mode = int(fs_taa_debug.x + 0.5);
        if (mode == 1)
        {
            // history weight: white all history, black all this frame
            dbg = vec3(1.0 - alpha);
        }
        else if (mode == 2)
        {
            // why the history lost weight: the strongest reason, as bright as it acted
            if (!use_history)
            {
                dbg = fs_taa_reset > 0.5 ? vec3(0.0, 0.6, 1.0) : vec3(1.0, 0.0, 1.0);   // reset: cyan, off screen: magenta
            }
            else
            {
                float strongest = max(max(dbg_disocclusion, dbg_clip), max(dbg_reactive, dbg_moved));
                if (strongest > 0.02)
                {
                    vec3 c = dbg_disocclusion >= strongest ? vec3(1.0, 0.0, 0.0)     // disocclusion: red
                           : dbg_moved >= strongest ? vec3(1.0, 1.0, 0.0)           // "moved by itself": yellow
                           : dbg_reactive >= strongest ? vec3(0.0, 0.0, 1.0)        // reactive (alpha, water): blue
                           : vec3(0.0, 1.0, 0.0);                                   // colour clip changed the history: green
                    dbg = mix(dbg, c, clamp(strongest, 0.0, 1.0));
                }
            }
        }
        else if (mode == 3)
        {
            // motion: black still, blue in the still/moving transition (0.1 to 0.5 px), green to red up to 16 px
            float m = clamp(speed / 16.0, 0.0, 1.0);
            dbg = speed < 0.5 ? vec3(0.0, 0.0, 1.0 - still) : vec3(m, 1.0 - m, 0.0);
        }
        else if (mode == 4)
        {
            dbg = debugJitter(gl_FragCoord.xy, grey * vec3(3.0));
        }
        else if (mode == 5)
        {
            // how much this frame differs from the history before clipping, times 8: the jitter's
            // own differences show on edges only; flat areas lighting up are noise or changes
            dbg = clamp(abs(filtered - compress(finiteColor(historyCatmullRom(history_uv)), e)) * 8.0, 0.0, 1.0);
        }
        frag_color = vec4(dbg * 0.8 / e, 1.0);
        return;
    }

    frag_color = vec4(finiteColor(result), min(cur_linear, 65000.0));
}
