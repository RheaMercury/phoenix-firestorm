// Avatars only snapshot: coverage from the avatars rendered over a black and over a white background

/*[EXTRA_CODE_HERE]*/

// Both inputs are linear HDR, taken before exposure and tone mapping, where blending is linear:
// over a background B a pixel is colour + (1 - coverage) * B. Over black that leaves the
// premultiplied colour, and the white frame differs from it by (1 - coverage) * white.
//
// Light blended additively (particles with destination factor ONE, such as glow sparkles) adds
// the same amount over black and over white, so the comparison sees no coverage for it. Where the
// frame over black holds more light than its coverage explains (a surface at most white, after
// exposure), the rest is such light: it gets coverage from its brightness, so it shows in the
// image. The output is straight (not premultiplied) alpha: the colour is the light divided by its
// coverage, tone mapped afterwards. Over black the result matches the world; over a brighter
// background additive light darkens it slightly, which straight alpha can't avoid.

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform sampler2D diffuseRect;      // avatars over black
uniform sampler2D altDiffuseMap;    // avatars over white
uniform sampler2D exposureMap;      // the scene's exposure, as the tone mapper reads it
uniform float exposure;             // RenderExposure, clamped as the tone mapper clamps it
uniform float bg_level;             // the white background's level
uniform int output_alpha;           // 1: write the coverage, 0: write the un-premultiplied colour

void main()
{
    vec2 tc = vary_fragcoord.xy;
    vec3 over_black = texture(diffuseRect, tc).rgb;
    vec3 over_white = texture(altDiffuseMap, tc).rgb;

    // How much of the background shows through, per channel. Tinted glass lets the channels
    // through unevenly, the average keeps its overall transparency.
    vec3 seen = clamp((over_white - over_black) / max(bg_level, 1e-4), 0.0, 1.0);
    float alpha = 1.0 - (seen.r + seen.g + seen.b) / 3.0;

    // Half float rounding leaves the bare background a hair off 0, and solid surfaces a hair off 1
    if (alpha < 1.0 / 512.0)
    {
        alpha = 0.0;
    }
    else if (alpha > 1.0 - 1.0 / 512.0)
    {
        alpha = 1.0;
    }

    // Additive light: the brightest channel over black, in units where 1 is white after exposure,
    // beyond what the coverage can hold, as a share of the coverage still free
    float scale = max(exposure * texture(exposureMap, vec2(0.5)).r, 1e-4);
    vec3 lit = max(over_black, vec3(0.0)) * scale;
    float peak = max(lit.r, max(lit.g, lit.b));
    float glow = alpha < 1.0 ? clamp((peak - alpha) / (1.0 - alpha), 0.0, 1.0) : 0.0;
    if (glow < 1.0 / 512.0)
    {
        glow = 0.0;
    }

    // Linear coverage the colour is divided by: the light's brightest channel comes out at white,
    // and the tone mapper brings it back down by the coverage below
    float divide = alpha + (1.0 - alpha) * glow;

    if (output_alpha != 0)
    {
        // The glow's coverage in display terms (about the tone mapper's gamma), so that over black
        // white light at this coverage looks as bright as the light did in the world
        float coverage = alpha + (1.0 - alpha) * pow(glow, 1.0 / 2.2);
        frag_color = vec4(vec3(coverage), 1.0);
        return;
    }

    vec3 color = divide > 0.0 ? over_black / divide : vec3(0.0);
    frag_color = vec4(max(color, vec3(0.0)), 0.0);
}
