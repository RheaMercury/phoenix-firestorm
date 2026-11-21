/**
 * @file class1/deferred/fsShadowDepthFitF.glsl
 *
 * Reduces the main view's depth buffer to the nearest and the farthest view distance (metres,
 * along the view axis, as the sun shadow splits measure them) of the pixels that aren't sky, for
 * FSRenderShadowFitDepth (fsshadowdepthfit.cpp). Each output texel covers a block of
 * fs_fit_block x fs_fit_block texels of the level before; r is the nearest, g the farthest
 * distance. A block with only sky gives r = 1e30, g = 0, which the min and max of the next level
 * ignore, and a whole view of sky ends as r > g.
 *
 * The block is a constant (16, as fsshadowdepthfit.cpp's BLOCK), so the loops have fixed bounds and
 * the texel reads of a block can all be in flight at once; reads past the edge of the level are
 * clamped onto its last row or column, which only repeats texels of the same view (the result is
 * the extremes over the whole view, so a texel counted twice changes nothing). With GLSL 4.00 the
 * depth buffer is read four texels at a time (textureGather): a quarter of the reads.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

uniform ivec2 fs_fit_src_size; // size of the level read

#define FS_FIT_BLOCK 16

#ifdef FS_DEPTH_FIT_FIRST

uniform sampler2D depthMap;
uniform mat4 fs_fit_inv_proj;

// view distance of a raw depth: the inverse projection's z and w rows don't depend on x and y
float fsFitDistance(float d)
{
#ifdef REVERSE_Z
    float ndcz = d;
#else
    float ndcz = 2.0 * d - 1.0;
#endif
    float z = fs_fit_inv_proj[2].z * ndcz + fs_fit_inv_proj[3].z;
    float w = fs_fit_inv_proj[2].w * ndcz + fs_fit_inv_proj[3].w;
    return -z / w;
}

// sky depth reads as "nothing here": out of range for the min and the max
#ifdef REVERSE_Z
#define FS_FIT_SKY(d) ((d) <= 0.0)
#else
#define FS_FIT_SKY(d) ((d) >= 1.0)
#endif

void main()
{
    ivec2 base = ivec2(gl_FragCoord.xy) * FS_FIT_BLOCK;
    ivec2 last = fs_fit_src_size - ivec2(1);

    // raw depth is monotonic in distance: find the extremes first, convert two values
    float lo = 2.0;
    float hi = -1.0;
#if __VERSION__ >= 400
    // each gather reads the 2x2 texels around the corner it is given (wrap and filter don't matter: the
    // corner is clamped to inside the level, so the four are texels of it)
    vec2 inv_size = 1.0 / vec2(fs_fit_src_size);
    for (int y = 0; y < FS_FIT_BLOCK; y += 2)
    {
        for (int x = 0; x < FS_FIT_BLOCK; x += 2)
        {
            ivec2 p = min(base + ivec2(x, y), max(last - ivec2(1), ivec2(0)));
            vec4 d = textureGather(depthMap, (vec2(p) + vec2(1.0)) * inv_size, 0);
            for (int i = 0; i < 4; ++i)
            {
                if (!FS_FIT_SKY(d[i]))
                {
                    lo = min(lo, d[i]);
                    hi = max(hi, d[i]);
                }
            }
        }
    }
#else
    for (int y = 0; y < FS_FIT_BLOCK; ++y)
    {
        for (int x = 0; x < FS_FIT_BLOCK; ++x)
        {
            float d = texelFetch(depthMap, min(base + ivec2(x, y), last), 0).r;
            if (!FS_FIT_SKY(d))
            {
                lo = min(lo, d);
                hi = max(hi, d);
            }
        }
    }
#endif

    if (hi < lo)
    {
        frag_color = vec4(1.0e30, 0.0, 0.0, 1.0);
        return;
    }

#ifdef REVERSE_Z
    // larger is nearer
    float nearest = fsFitDistance(hi);
    float farthest = fsFitDistance(lo);
#else
    float nearest = fsFitDistance(lo);
    float farthest = fsFitDistance(hi);
#endif
    frag_color = vec4(max(nearest, 0.0), max(farthest, 0.0), 0.0, 1.0);
}

#else

uniform sampler2D diffuseRect;

void main()
{
    ivec2 base = ivec2(gl_FragCoord.xy) * FS_FIT_BLOCK;
    ivec2 last = fs_fit_src_size - ivec2(1);

    vec2 range = vec2(1.0e30, 0.0);
    for (int y = 0; y < FS_FIT_BLOCK; ++y)
    {
        for (int x = 0; x < FS_FIT_BLOCK; ++x)
        {
            vec2 s = texelFetch(diffuseRect, min(base + ivec2(x, y), last), 0).rg;
            range.x = min(range.x, s.x);
            range.y = max(range.y, s.y);
        }
    }
    frag_color = vec4(range, 0.0, 1.0);
}

#endif
