// Shoreline foam: a soft, moving band of foam where the water surface meets land or anything
// standing in it. Linked into the water shaders (waterF.glsl, underWaterF.glsl) when
// FSRenderWaterFoam is on and water is transparent, since it needs the scene depth behind the
// surface; also linked for the churned-water foam of the ripples (FS_WATER_RIPPLE_FOAM). The
// pattern is procedural, so there is no texture to load.
//
// Detailed foam (FSRenderWaterFoamDetail, fs_foam_params.w): the shoreline band, the ripples'
// churned-water foam and their water line are only coverage, how much of the water foam
// covers there. fsWaterFoamDetail turns that into lace: a network of foam veins around open
// holes, finer lace and small bubbles, down to a few centimetres. Low coverage keeps only the
// veins' cores, broken into strands; high coverage closes the holes into dense white foam. So
// the coarse fields (the ripple grid is 12.5 cm a cell) look sharp close up. Off, the foam is
// the flat noise band it was before.

uniform sampler2D fs_foam_depth; // opaque scene depth, copied before any alpha was drawn (lldrawpoolwater.cpp)
uniform mat4 inv_proj;
uniform vec3 eyeVec;            // camera position, agent space
uniform vec4 fs_foam_params;    // x strength (0 = off), y band width in metres, z animation phase (s), w 1 detailed foam, 0 flat

float fsFoamHash(vec2 p)
{
    vec3 q = fract(vec3(p.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}

vec2 fsFoamHash2(vec2 p)
{
    vec3 q = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    q += dot(q, q.yzx + 33.33);
    return fract((q.xx + q.yz) * q.zy);
}

float fsFoamValueNoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = p - i;
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = fsFoamHash(i);
    float b = fsFoamHash(i + vec2(1.0, 0.0));
    float c = fsFoamHash(i + vec2(0.0, 1.0));
    float d = fsFoamHash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// Frays the ripples' churned-water foam (fsWaterRipplesF.glsl). With the detailed foam on, the
// water shaders fray all foam with fsWaterFoamDetail instead, so this gives the middle value and
// the churned water arrives there as plain coverage.
float fsFoamNoise(vec2 p)
{
    return fs_foam_params.w > 0.5 ? 0.5 : fsFoamValueNoise(p);
}

// Lace: jittered cells whose points wander slowly (t), the distance to the second nearest point
// minus the nearest, in cells. 0 along the borders between cells, which become the foam veins.
float fsFoamCells(vec2 p, float t)
{
    vec2 i = floor(p);
    vec2 f = p - i;
    float d1 = 8.0;
    float d2 = 8.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            vec2 o = vec2(float(x), float(y));
            vec2 d = o + 0.5 + 0.38 * sin(vec2(t, t * 0.83) + 6.2831 * fsFoamHash2(i + o)) - f;
            float dd = dot(d, d);
            d2 = min(d2, max(d1, dd));
            d1 = min(d1, dd);
        }
    }
    return sqrt(d2) - sqrt(d1);
}

// The same over only the four cells around the nearest corner: cheaper and a little less exact,
// which the fine lace doesn't show
float fsFoamCellsFast(vec2 p, float t)
{
    vec2 i = floor(p);
    vec2 f = p - i;
    vec2 base = step(0.5, f) - 1.0;
    float d1 = 8.0;
    float d2 = 8.0;
    for (int y = 0; y <= 1; ++y)
    {
        for (int x = 0; x <= 1; ++x)
        {
            vec2 o = base + vec2(float(x), float(y));
            vec2 d = o + 0.5 + 0.38 * sin(vec2(t, t * 0.83) + 6.2831 * fsFoamHash2(i + o)) - f;
            float dd = dot(d, d);
            d2 = min(d2, max(d1, dd));
            d1 = min(d1, dd);
        }
    }
    return sqrt(d2) - sqrt(d1);
}

// Small round bubbles, at most one per cell, each its own size: 1 at a bubble's centre, 0 outside
float fsFoamBubbles(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = p - i;
    float r = 0.16 + 0.24 * fsFoamHash(i + vec2(7.0, 3.0));
    vec2 c = 0.5 + (fsFoamHash2(i + vec2(41.0, 17.0)) - 0.5) * (1.0 - 2.0 * r);
    return 1.0 - smoothstep(0.5, 1.0, length(f - c) / r);
}

// How much of a pattern with this many features per metre to keep at this pixel size (metres):
// all of it while a feature spans 5 pixels or more, none below 2, where it would only shimmer
float fsFoamFade(float footprint, float freq)
{
    return 1.0 - smoothstep(0.2, 0.5, footprint * freq);
}

// Foam for a water surface fragment, from how much of it foam covers.
// view_offset agent space offset of the fragment from the camera (waterV.glsl's view.xy)
// cover       foam coverage in [0, 1]: the shoreline band, the ripples' churned water and line
// flow        agent space offset (m) the waves and ripples push the foam by, so it rides them
// footprint   size of the pixel on the water in metres (fwidth of the world position)
// Returns the foam's albedo in rgb, its opacity in a.
vec4 fsWaterFoamDetail(vec2 view_offset, float cover, vec2 flow, float footprint)
{
    if (fs_foam_params.w < 0.5)
    {
        return vec4(vec3(0.85), cover); // the flat look: plain paint of the coverage
    }
    if (!(cover > 0.0))
    {
        return vec4(0.0); // and NaN
    }
    cover = min(cover, 1.0);
    float fp = footprint >= 0.0 ? min(footprint, 1.0) : 1.0; // NaN at the horizon: all averaged
    float t = fs_foam_params.z;
    vec2 p = view_offset + eyeVec.xy + flow;
    vec2 r = vec2(0.8 * p.x - 0.6 * p.y, 0.6 * p.x + 0.8 * p.y); // turned, so the grids don't line up

    // Each layer fades to its average as its features get too small for the pixels
    float k1 = fsFoamFade(fp, 3.2);
    float k2 = fsFoamFade(fp, 5.0);
    float k3 = fsFoamFade(fp, 12.0);
    float k4 = fsFoamFade(fp, 23.0);
    float k5 = fsFoamFade(fp, 37.0);

    // Patches (0.3 to 0.7 m), so the foam gathers in clumps and strands with open water between
    float clump = fsFoamValueNoise(p * 1.4 + vec2(t * 0.011, 0.0)) * 0.6
                + fsFoamValueNoise(p * 3.22 + vec2(-t * 0.017, 0.5)) * 0.4;
    clump = 0.5 + (clump - 0.5) * (0.6 + 0.4 * k1);

    // Lace of 20 cm holes, its veins thick in the clumps and thin, broken strands between them
    float lace = 0.443;
    if (k2 > 0.0)
    {
        float v = 1.0 - smoothstep(0.0, 0.25 + 0.35 * clump, fsFoamCells(p * 5.0, t * 0.35));
        lace += (v - 0.443) * k2;
    }
    // Finer lace, 8 cm, wandering a little faster
    float fine = 0.408;
    if (k3 > 0.0)
    {
        float v = 1.0 - smoothstep(0.0, 0.4, fsFoamCellsFast(r * 12.0, t * 0.6));
        fine += (v - 0.408) * k3;
    }
    // Bubbles of 1 to 4 cm
    float bub = 0.15 * (1.0 - (k4 * 0.6 + k5 * 0.4));
    if (k4 > 0.0)
    {
        bub += fsFoamBubbles(p * 23.0) * k4 * 0.6;
    }
    if (k5 > 0.0)
    {
        bub += fsFoamBubbles(r * 37.0 + vec2(0.37, 0.0)) * k5 * 0.4;
    }

    // How foamy each point is: foam where this rises above the threshold for the coverage
    float h = 0.34 * clump + 0.36 * lace + 0.2 * fine - 0.16 * bub;
    // The threshold softens by how much h changes across the pixel, from the detail still drawn,
    // and by how much the faded detail would have spread it, so nothing aliases
    float grad = fp * (0.952 * k1 + 3.96 * k2 + 6.0 * k3 + 3.68 * k4 + 5.92 * k5);
    float spread = 0.25 * (0.36 * (1.0 - k2) + 0.2 * (1.0 - k3) + 0.16 * (1.0 - 0.5 * (k4 + k5)) + 0.136 * (1.0 - k1));
    float s = 0.015 + grad * 0.5 + spread;
    // h has this much of its area above the threshold for coverage c (fitted to the pattern)
    float c = cover;
    float th = 0.781 + c * (-1.576 + c * (2.325 - 1.537 * c));

    float m = smoothstep(th - s, th + s, h);
    float thick = smoothstep(th, th + 0.2 + s, h);
    // Thin lace lets the water show through it, thick foam hides it
    float alpha = m * (0.6 + 0.4 * thick);
    // Far away the lace is all averaged: the share of the pixel that is foam, at about its opacity
    float far = 1.0 - (0.36 * k2 + 0.2 * k3) * (1.0 / 0.56);
    alpha = mix(alpha, cover * 0.85, far) * smoothstep(0.0, 0.05, cover);

    // Dense foam is white, its bubbles a shade darker; thin foam a blue-grey film over the water
    float dense = smoothstep(th, th + 0.35 + s, h);
    float shade = 0.74 + 0.2 * dense - 0.12 * bub * thick;
    vec3 tint = vec3(0.8, 0.88, 0.93) + vec3(0.2, 0.12, 0.07) * thick;
    return vec4(shade * tint, clamp(alpha, 0.0, 1.0));
}

// Shoreline foam for a water surface fragment, in [0, 1]: its opacity in the flat look, its
// coverage for fsWaterFoamDetail in the detailed one.
// screen_tc   undistorted screen position of the fragment
// pos         eye space position of the fragment
// up          eye space normal of the water plane
// view_offset agent space offset of the fragment from the camera (waterV.glsl's view.xy)
float fsWaterFoam(vec2 screen_tc, vec3 pos, vec3 up, vec2 view_offset)
{
    float strength = fs_foam_params.x;
    float width = max(fs_foam_params.y, 0.01);
    float dist = length(pos);
    if (strength <= 0.0 || dist > 256.0)
    {
        return 0.0;
    }

    // Alpha-blended surfaces (invisible or see-through prims, glass) are not in this depth, so
    // they make no shoreline
    float depth = texture(fs_foam_depth, screen_tc).r;
#ifdef REVERSE_Z
    if (depth <= 0.0)
    {
        return 0.0; // sky behind the surface
    }
    vec4 scene = inv_proj * vec4(screen_tc * 2.0 - 1.0, depth, 1.0); // the depth is the NDC depth itself
#else
    if (depth >= 1.0)
    {
        return 0.0; // sky behind the surface
    }
    vec4 scene = inv_proj * vec4(vec3(screen_tc, depth) * 2.0 - 1.0, 1.0);
#endif
    scene.xyz /= scene.w;

    // Vertical gap between the surface and what lies behind it: the depth of the water from
    // above, the height of whatever rises out of it from below. Either way it closes to zero
    // along the line where something meets the surface.
    float gap = abs(dot(scene.xyz - pos, up));
    float band = 1.0 - clamp(gap / width, 0.0, 1.0);
    if (!(band > 0.0))
    {
        return 0.0;
    }

    vec2 w = view_offset + eyeVec.xy;
    float t = fs_foam_params.z;

    // Depth precision and pixel footprint cannot hold a band this thin far away
    float far_fade = 1.0 - smoothstep(128.0, 256.0, dist);

    if (fs_foam_params.w > 0.5)
    {
        // Dense close to the edge, thinning out across the band
        float cover = band * band * band * 1.1;
        // Streaks washing in toward the edge: lines along it at uneven spacing, wavy, and broken
        // into pieces along their length
        float u = band * 4.0 - t * 0.12 + (fsFoamValueNoise(w * 0.6 + vec2(3.1, 0.0)) - 0.5) * 1.6;
        float streak = smoothstep(0.5, 0.85, fsFoamValueNoise(vec2(u * 1.9, dot(w, vec2(0.31, 0.23)))));
        float pieces = smoothstep(0.3, 0.7, fsFoamValueNoise(w * 1.1 + vec2(-t * 0.02, 5.3)));
        cover += streak * pieces * 0.85 * smoothstep(0.0, 0.5, band);
        // No outline where the band ends: the coverage fades out, and the lace with it
        cover = clamp(cover * smoothstep(0.0, 0.3, band), 0.0, 1.0);
        // The strength thins the foam out, but where the water touches stays dense
        float contact = smoothstep(0.86, 0.99, band);
        cover = max(cover * sqrt(strength), contact * (0.75 + 0.25 * strength));
        return cover * far_fade;
    }

    float n = fsFoamValueNoise(w * 1.7 + vec2(0.11, 0.07) * t) * 0.6
            + fsFoamValueNoise(w * 4.3 - vec2(0.05, 0.13) * t) * 0.4;

    // Ridges that wash in toward the edge, broken up by the noise so the outline frays
    float ridge = 0.5 + 0.5 * sin(band * 9.0 - t * 1.3 + n * 5.0);
    float shape = band * (0.6 + 0.4 * ridge) + (n - 0.5) * 0.7;
    float foam = smoothstep(0.3, 0.7, shape) * sqrt(band);

    foam *= far_fade;

    return clamp(foam * strength, 0.0, 1.0);
}
