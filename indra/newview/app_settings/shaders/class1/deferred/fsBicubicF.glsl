// Third-order filtering for the reduced resolution effects and the temporal histories
// (FSRenderBicubicUpsample, FS_BICUBIC)

// Linked as an extra fragment object into the programs that upsample a reduced resolution buffer
// or fetch a history between texels: the sun ray and projector light composites, their temporal
// resolve, the indirect light and sun shadow temporal passes, and the tone mapper's bloom fetch.
//
// Bilinear filtering is only continuous, not smooth: a reduced resolution buffer shows its texel
// grid as diamond-shaped ridges, and a history fetched bilinearly every frame blurs a little more
// each time it is reprojected. Cubic filters fix both:
//  - fsBSplineWeight: cubic B-spline weight of a texel at an offset, for the bilateral composites,
//    which weigh every texel themselves (by depth too), so they can't use the hardware trick below.
//    Smooth (C2) and exactly zero two texels out, so nothing jumps as the low resolution grid
//    moves under the pixels.
//  - fsBicubicBSpline: the same filter from 4 bilinear fetches (Sigg and Hadwiger, GPU Gems 2
//    ch. 20). Smooth and never overshoots; for the bloom, which is a blur anyway.
//  - fsCatmullRom: interpolating Catmull-Rom from 5 bilinear fetches (the 9 fetch version with
//    the 4 corners, whose weights are tiny, left out and the rest renormalised, as in Jimenez's
//    filmic SMAA). Keeps a history sharp across reprojections; it can overshoot a little, which
//    the callers' neighbourhood clamps take back.
// The textures must be bilinear filtered for the last two.

// cubic B-spline of one coordinate
float fsBSpline(float x)
{
    x = abs(x);
    if (x < 1.0)
    {
        return (4.0 - 6.0 * x * x + 3.0 * x * x * x) / 6.0;
    }
    if (x < 2.0)
    {
        float t = 2.0 - x;
        return t * t * t / 6.0;
    }
    return 0.0;
}

// weight of a texel d texels away (each axis) from the point being filtered
float fsBSplineWeight(vec2 d)
{
    return fsBSpline(d.x) * fsBSpline(d.y);
}

vec4 fsBicubicBSpline(sampler2D tex, vec2 uv)
{
    vec2 size = vec2(textureSize(tex, 0));
    vec2 pos = uv * size - 0.5;
    vec2 i = floor(pos);
    vec2 f = pos - i;
    vec2 f2 = f * f;
    vec2 f3 = f2 * f;
    vec2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
    vec2 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
    vec2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;
    vec2 w3 = f3 / 6.0;
    vec2 g0 = w0 + w1;
    vec2 g1 = w2 + w3;
    // where between texels i-1 and i (and i+1 and i+2) a bilinear fetch mixes them in that ratio
    vec2 t0 = (i - 1.0 + w1 / g0 + 0.5) / size;
    vec2 t1 = (i + 1.0 + w3 / g1 + 0.5) / size;
    return g0.y * (g0.x * textureLod(tex, vec2(t0.x, t0.y), 0.0) + g1.x * textureLod(tex, vec2(t1.x, t0.y), 0.0)) +
           g1.y * (g0.x * textureLod(tex, vec2(t0.x, t1.y), 0.0) + g1.x * textureLod(tex, vec2(t1.x, t1.y), 0.0));
}

vec4 fsCatmullRom(sampler2D tex, vec2 uv)
{
    vec2 size = vec2(textureSize(tex, 0));
    vec2 pos = uv * size;
    vec2 c1 = floor(pos - 0.5) + 0.5;  // the texel centre at or left of / below the point
    vec2 f = pos - c1;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 t0 = (c1 - 1.0) / size;
    vec2 t3 = (c1 + 2.0) / size;
    vec2 t12 = (c1 + w2 / w12) / size;

    float k0 = w12.x * w0.y;
    float k1 = w0.x * w12.y;
    float k2 = w12.x * w12.y;
    float k3 = w3.x * w12.y;
    float k4 = w12.x * w3.y;
    vec4 sum = textureLod(tex, vec2(t12.x, t0.y), 0.0) * k0 +
               textureLod(tex, vec2(t0.x, t12.y), 0.0) * k1 +
               textureLod(tex, vec2(t12.x, t12.y), 0.0) * k2 +
               textureLod(tex, vec2(t3.x, t12.y), 0.0) * k3 +
               textureLod(tex, vec2(t12.x, t3.y), 0.0) * k4;
    return sum / (k0 + k1 + k2 + k3 + k4);
}
