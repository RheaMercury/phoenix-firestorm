// Bloom: pick out the bright parts of the HDR scene at half resolution

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform sampler2D diffuseRect;  // full resolution HDR scene, linear
uniform sampler2D exposureMap;
uniform vec2 src_texel;         // one texel of diffuseRect

uniform float exposure;
uniform float bloom_threshold;  // in exposed (on screen) brightness
uniform float bloom_knee;       // width of the soft transition below the threshold
uniform int fs_bloom_hq;        // FSRenderBloomHQFilter: the firefly weights only push down taps that stand out

float luminance(vec3 c)
{
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

// What of this colour passes the threshold, easing in over the knee rather than switching on
vec3 brightPart(vec3 c)
{
    float brightness = max(c.r, max(c.g, c.b));
    float soft = clamp(brightness - bloom_threshold + bloom_knee, 0.0, 2.0 * bloom_knee);
    soft = soft * soft / (4.0 * bloom_knee + 1e-4);
    float contribution = max(soft, brightness - bloom_threshold) / max(brightness, 1e-4);
    return c * contribution;
}

vec3 tap(vec2 tc, float scale, inout float weight_sum)
{
    vec3 c = texture(diffuseRect, tc).rgb * scale;
    // A NaN or Inf texel would poison the weights and spread over the whole bloom
    c = mix(c, vec3(0.0), isnan(c));
    c = min(c, vec3(65000.0));
    c = brightPart(c);
    // A single very bright pixel would otherwise turn into a flickering blob: weigh each tap
    // down by its own brightness, so the average leans toward the typical value
    float w = 1.0 / (1.0 + luminance(c));
    weight_sum += w;
    return c * w;
}

// One tap's bright part, without a weight (FSRenderBloomHQFilter)
vec3 sceneTap(vec2 tc, float scale)
{
    vec3 c = texture(diffuseRect, tc).rgb * scale;
    c = mix(c, vec3(0.0), isnan(c));
    c = min(c, vec3(65000.0));
    return brightPart(c);
}

// The largest channel: a saturated red or blue firefly counts in full, as luminance would not
float peakOf(vec3 c)
{
    return max(c.r, max(c.g, c.b));
}

void main()
{
    vec2 tc = vary_fragcoord.xy;
    float scale = max(exposure * texture(exposureMap, vec2(0.5)).r, 1e-4);

    vec3 c;
    if (fs_bloom_hq != 0)
    {
        // The firefly weight below also pushes down every tap of an evenly bright area against
        // the darker taps at its edge, so a small bright window or the sun's disc loses up to half
        // its light at every edge and blooms as a shrunken, rounded blob. Here a tap is weighed
        // down only by how much brighter it is than the brightest of the other three: an even
        // area keeps a plain average and its shape, a lone bright pixel is still crushed. The
        // outlier form follows Alchemy Viewer's bloomDownsampleF.glsl (Copyright (C) 2026,
        // Alchemy Viewer Project, LGPL 2.1).
        vec3 a = sceneTap(tc + vec2(-1.0, -1.0) * src_texel, scale);
        vec3 b = sceneTap(tc + vec2( 1.0, -1.0) * src_texel, scale);
        vec3 d = sceneTap(tc + vec2(-1.0,  1.0) * src_texel, scale);
        vec3 e = sceneTap(tc + vec2( 1.0,  1.0) * src_texel, scale);
        float la = peakOf(a);
        float lb = peakOf(b);
        float ld = peakOf(d);
        float le = peakOf(e);
        float wa = 1.0 / (1.0 + max(la - max(max(lb, ld), le), 0.0));
        float wb = 1.0 / (1.0 + max(lb - max(max(la, ld), le), 0.0));
        float wd = 1.0 / (1.0 + max(ld - max(max(la, lb), le), 0.0));
        float we = 1.0 / (1.0 + max(le - max(max(la, lb), ld), 0.0));
        c = (a * wa + b * wb + d * wd + e * we) / (wa + wb + wd + we);
    }
    else
    {
        // Four bilinear taps cover the 4x4 source texels around this half resolution texel
        float weight_sum = 0.0;
        c = tap(tc + vec2(-1.0, -1.0) * src_texel, scale, weight_sum);
        c += tap(tc + vec2( 1.0, -1.0) * src_texel, scale, weight_sum);
        c += tap(tc + vec2(-1.0,  1.0) * src_texel, scale, weight_sum);
        c += tap(tc + vec2( 1.0,  1.0) * src_texel, scale, weight_sum);
        c /= max(weight_sum, 1e-4);
    }

    // Back to scene units, it is added before exposure is applied
    frag_color = vec4(c / scale, 1.0);
}
