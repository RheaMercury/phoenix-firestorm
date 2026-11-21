// Interactive water ripples (FSRenderWaterRipples), sampled by the water shaders (waterF.glsl,
// underWaterF.glsl) when built with FS_WATER_RIPPLES. The height field is simulated on a grid
// around the camera (fswaterripples.cpp, fsWaterRippleSimF.glsl).
//
// No vertex displacement: the water is a flat plane drawn with a handful of vertices, far too few
// to carry ripples. The ripples tilt the surface normal instead, which drives the reflection, the
// refraction distortion, the specular and the glints, so the plane reads as displaced.
//
// The water line (FSRenderWaterRippleWaterline): the simulation keeps each cell's obstacle flag in
// alpha (1 inside a post, a hull or the shore, 0 in open water), and filtered that falls from 1 to
// 0 across an obstacle's edge. A thin, faint touch of white just outside it marks where the water
// meets the object, and it flashes up where a lap climbs the edge (the laps come at random,
// fsWaterRippleSimF.glsl, so the flicker does too). The shoreline foam (FS_WATER_FOAM) already
// draws that edge from the scene depth, so with it on this adds nothing.

uniform sampler2D fs_ripple_map; // r height (m), g height a step earlier, b churned-water foam, a 1 in obstacles
uniform vec4 fs_ripple_grid;     // xy agent-space corner of the grid, z 1 / its size in metres, w 1 / cells across
uniform vec4 fs_ripple_params;   // x slope strength (0 = off), y 1 / (2 * cell size), z foam strength, w animation phase
uniform vec2 fs_ripple_line;     // x water line touch at its brightest (0 = off), y 1 / the distance it is gone by (m)
uniform vec3 eyeVec;             // camera position, agent space

#ifdef FS_WATER_RIPPLE_FOAM
float fsFoamNoise(vec2 p); // fsWaterFoamF.glsl
#endif

// Slope of the ripples (dh/dx, dh/dy, agent space) at a water fragment, scaled by their strength
// and faded out toward the grid's edges and where a pixel covers several cells.
// view_offset agent space offset of the fragment from the camera (waterV.glsl's view.xy)
// foam        churned-water foam and water line cover in [0, 1]
vec2 fsWaterRipples(vec2 view_offset, out float foam)
{
    foam = 0.0;
    vec2 w = view_offset + eyeVec.xy;
    vec2 uv = (w - fs_ripple_grid.xy) * fs_ripple_grid.z;

    // Before any branch: derivatives are only defined in uniform control flow
    vec2 footprint = fwidth(uv) / max(fs_ripple_grid.w, 1e-6); // cells per pixel

    if (fs_ripple_params.x <= 0.0)
    {
        return vec2(0.0);
    }
    vec2 edges = min(uv, 1.0 - uv);
    float edge = min(edges.x, edges.y);
    if (edge <= 0.0)
    {
        return vec2(0.0); // beyond the grid the water is as before
    }
    float fade = smoothstep(0.0, 0.15, edge);
    // Ripples smaller than a pixel would only shimmer
    fade *= 1.0 - smoothstep(0.75, 2.5, max(footprint.x, footprint.y));
    if (fade <= 0.0)
    {
        return vec2(0.0);
    }

    float t = fs_ripple_grid.w;
    vec4 cl = textureLod(fs_ripple_map, uv - vec2(t, 0.0), 0.0);
    vec4 cr = textureLod(fs_ripple_map, uv + vec2(t, 0.0), 0.0);
    vec4 cd = textureLod(fs_ripple_map, uv - vec2(0.0, t), 0.0);
    vec4 cu = textureLod(fs_ripple_map, uv + vec2(0.0, t), 0.0);
    vec2 slope = vec2(cr.r - cl.r, cu.r - cd.r) * fs_ripple_params.y * fs_ripple_params.x * fade;

#ifdef FS_WATER_RIPPLE_FOAM
    bool need_centre = true;
#else
    bool need_centre = false;
#endif
#ifndef FS_WATER_FOAM
    need_centre = need_centre || fs_ripple_line.x > 0.0;
#endif
    vec4 centre = need_centre ? textureLod(fs_ripple_map, uv, 0.0) : vec4(0.0);

#ifdef FS_WATER_RIPPLE_FOAM
    float churn = centre.b * fade;
    if (churn > 0.0 && fs_ripple_params.z > 0.0)
    {
        // Frayed by the shoreline foam's noise, finer and drifting faster, so wakes break up
        float phase = fs_ripple_params.w;
        float n = fsFoamNoise(w * 3.1 + vec2(0.07, 0.11) * phase) * 0.6
                + fsFoamNoise(w * 7.9 - vec2(0.13, 0.05) * phase) * 0.4;
        foam = smoothstep(0.2, 0.7, churn + (n - 0.5) * 0.8) * min(churn * 2.0, 1.0);
        foam = clamp(foam * fs_ripple_params.z, 0.0, 1.0);
    }
#endif

#ifndef FS_WATER_FOAM
    if (fs_ripple_line.x > 0.0)
    {
        // The obstacle flag blurred over a cell and a half each way: about a half at the edge, 0 a
        // cell and a half out in open water, 1 inside. The line sits on the open side, where the
        // object doesn't hide it, about a cell wide.
        float inside = (cl.a + cr.a + cd.a + cu.a + 2.0 * centre.a) * (1.0 / 6.0);
        inside = isnan(inside) ? 0.0 : inside;
        float line = smoothstep(0.03, 0.3, inside) * (1.0 - smoothstep(0.4, 0.75, inside));
        // Barely there between laps, bright where a lap climbs the edge (a lap is a few
        // millimetres to a couple of centimetres high there), gone again as it falls back
        float climb = smoothstep(0.002, 0.015, centre.r);
        line *= (0.2 + 0.8 * climb) * fade;
        line *= 1.0 - clamp(length(view_offset) * fs_ripple_line.y, 0.0, 1.0);
        foam = max(foam, clamp(line * fs_ripple_line.x, 0.0, 1.0));
    }
#endif

    return clamp(slope, vec2(-2.0), vec2(2.0));
}
