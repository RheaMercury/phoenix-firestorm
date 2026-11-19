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
uniform int fs_fit_block;

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

void main()
{
    ivec2 base = ivec2(gl_FragCoord.xy) * fs_fit_block;
    ivec2 end = min(base + ivec2(fs_fit_block), fs_fit_src_size);

    // raw depth is monotonic in distance: find the extremes first, convert two values
    float lo = 2.0;
    float hi = -1.0;
    for (int y = base.y; y < end.y; ++y)
    {
        for (int x = base.x; x < end.x; ++x)
        {
            float d = texelFetch(depthMap, ivec2(x, y), 0).r;
#ifdef REVERSE_Z
            bool sky = d <= 0.0;
#else
            bool sky = d >= 1.0;
#endif
            if (!sky)
            {
                lo = min(lo, d);
                hi = max(hi, d);
            }
        }
    }

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
    ivec2 base = ivec2(gl_FragCoord.xy) * fs_fit_block;
    ivec2 end = min(base + ivec2(fs_fit_block), fs_fit_src_size);

    vec2 range = vec2(1.0e30, 0.0);
    for (int y = base.y; y < end.y; ++y)
    {
        for (int x = base.x; x < end.x; ++x)
        {
            vec2 s = texelFetch(diffuseRect, ivec2(x, y), 0).rg;
            range.x = min(range.x, s.x);
            range.y = max(range.y, s.y);
        }
    }
    frag_color = vec4(range, 0.0, 1.0);
}

#endif
