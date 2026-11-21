// High quality lens flare (FSRenderLensFlareHQ): how strongly the sun (or moon) drives the flare
// this frame, measured once on the HDR scene and filtered over time. One texel:
//   r  the drive: the sun's overbright light over the part of its disc that is not covered,
//      compressed, times the fade at the frame's edge, filtered over time
//   g  the covered share of the disc as measured this frame (for inspection)
//   b  the raw drive before the time filter (for inspection)
// The lens effects pass reads r and draws the ghosts, halo and starburst from it.
//
// The approach follows Alchemy Viewer's lensFlareStateF.glsl (Copyright (C) 2026, Alchemy Viewer
// Project, LGPL 2.1): measure the sun once per frame on a fixed, unrotated pattern of taps rather
// than per pixel, count only taps that see the sky and are brighter than the screen can show, and
// let the result change only at a bounded rate, so a fence or foliage passing in front of the sun
// dims the flare smoothly instead of blinking it. The tap pattern, weights and filter here are
// our own and much simpler.

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

uniform sampler2D diffuseRect;  // HDR scene, linear, after the TAA resolve
uniform sampler2D depthMap;     // scene depth
uniform sampler2D exposureMap;  // this frame's exposure
uniform sampler2D sceneMap;     // last frame's state (1x1)

uniform float exposure;         // RenderExposure, as the tone mapper clamps it
uniform vec2 screen_res;
uniform vec4 fs_flare_sun;      // xy sun in uv, z disc radius in screen heights, w fade at the frame edge
uniform vec2 fs_flare_rate;     // how far toward the new value to move this frame: x rising, y falling (1 resets)

const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);
const int TAPS = 64;
const float GOLDEN_ANGLE = 2.39996323;

// Only the far plane is sky: the sky writes no depth and keeps the cleared value
float isSky(float d)
{
#ifdef REVERSE_Z
    return d <= 0.0 ? 1.0 : 0.0;
#else
    return d >= 1.0 ? 1.0 : 0.0;
#endif
}

void main()
{
    vec4 prev = texelFetch(sceneMap, ivec2(0, 0), 0);
    float scale = max(exposure * texture(exposureMap, vec2(0.5)).r, 1e-4);

    vec2 sun = fs_flare_sun.xy;
    vec2 radius = fs_flare_sun.z * vec2(screen_res.y / max(screen_res.x, 1.0), 1.0);

    // A Vogel spiral over the disc, weighted toward its centre. Fixed from frame to frame: against
    // fine foliage a moving pattern would re-roll every tap each frame and the flare would shimmer
    // with the camera standing still.
    float w_sum = 0.0;
    float sky_sum = 0.0;
    float over_sum = 0.0;
    for (int i = 0; i < TAPS; ++i)
    {
        float r = sqrt((float(i) + 0.5) / float(TAPS));
        float a = float(i) * GOLDEN_ANGLE;
        vec2 o = vec2(cos(a), sin(a)) * r;
        float w = exp(-2.5 * r * r);
        w_sum += w;

        // A tap past the frame reads the edge texel, so an occluder reaching the edge still
        // occludes and a sun just out of view keeps its glow while the edge fade takes it out
        vec2 tc = clamp(sun + o * radius, vec2(0.0), vec2(1.0));
        float sky = isSky(textureLod(depthMap, tc, 0.0).r);
        if (sky > 0.0)
        {
            // Exposed brightness past what the screen shows: a bright cloud or haze near the sun
            // that tone mapping still holds below white does not count
            float lum = dot(textureLod(diffuseRect, tc, 0.0).rgb, LUMA) * scale;
            lum = (lum == lum) ? min(lum, 65000.0) : 0.0; // a NaN from the scene would stick in the history
            sky_sum += w;
            over_sum += w * max(lum - 1.0, 0.0);
        }
    }

    float visible = sky_sum / max(w_sum, 1e-4);
    float over = over_sum / max(w_sum, 1e-4);

    // A smooth limit on the drive: about 1 for a clear midday sun under normal exposure, at most 2
    // when the camera is exposed up for a dark interior with the sun in a window
    float d = over / 8.0;
    float target = 2.0 * d / (1.0 + d) * fs_flare_sun.w;

    float rate = target > prev.r ? fs_flare_rate.x : fs_flare_rate.y;
    float drive = mix(prev.r, target, clamp(rate, 0.0, 1.0));
    // A half float decays toward a denormal rather than to zero; the pass reading this needs zero
    drive = (target <= 0.0 && drive < 1e-3) ? 0.0 : drive;

    frag_color = vec4(drive, visible, target, 1.0);
}
