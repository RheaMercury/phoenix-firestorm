// High quality lens flare (FSRenderLensFlareHQ): find the brightest small light in each of 8 x 4
// tiles of the screen, from the bloom chain's bright pass, so the lens effects pass can give each
// a small flare of its own. Club lights, lamps, projector bulbs and bright glints all qualify;
// lit walls do not, because only a peak brighter than its surroundings counts.
//
// Drawn into a 32 x 2 target, one column per tile:
//   row 0  xy where the light is (uv), z its strength (scene brightness times its area in full
//          resolution pixels), w how much it stands out from its surroundings
//   row 1  rgb its colour, largest channel 1
// The select pass then keeps the strongest few.

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

uniform sampler2D diffuseRect;  // the coarse bloom level the tiles are scanned in
uniform sampler2D specularRect; // a finer level, to place the light more exactly
uniform sampler2D emissiveRect; // the first level, half resolution
uniform sampler2D depthMap;     // scene depth: the sky (and the sun in it) is never a light here

uniform vec4 fs_flare_sun;      // xy sun in uv, z disc radius in screen heights, w on screen (0 none)
uniform vec2 screen_res;
uniform float fs_pick_area;     // full resolution pixels in one texel of the coarse level

const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);
const int TILES_X = 8;
const int TILES_Y = 4;

float isSky(float d)
{
#ifdef REVERSE_Z
    return d <= 0.0 ? 1.0 : 0.0;
#else
    return d >= 1.0 ? 1.0 : 0.0;
#endif
}

float lumAt(sampler2D tex, ivec2 p, ivec2 size)
{
    vec3 c = texelFetch(tex, clamp(p, ivec2(0), size - 1), 0).rgb;
    float l = dot(c, LUMA);
    return (l == l) ? clamp(l, 0.0, 65000.0) : 0.0;
}

// The brightest texel within r of the given uv, then the brightness weighted centre of its 3 x 3
// neighbourhood, in uv: places the light to a fraction of this level's texel
vec2 refine(sampler2D tex, vec2 uv, int r)
{
    ivec2 size = textureSize(tex, 0);
    ivec2 c = ivec2(uv * vec2(size));
    ivec2 best = c;
    float best_l = -1.0;
    for (int y = -r; y <= r; ++y)
    {
        for (int x = -r; x <= r; ++x)
        {
            float l = lumAt(tex, c + ivec2(x, y), size);
            if (l > best_l)
            {
                best_l = l;
                best = c + ivec2(x, y);
            }
        }
    }
    vec2 sum = vec2(0.0);
    float w_sum = 0.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            ivec2 p = clamp(best + ivec2(x, y), ivec2(0), size - 1);
            float l = lumAt(tex, p, size);
            float w = l * l;
            sum += (vec2(p) + 0.5) * w;
            w_sum += w;
        }
    }
    return w_sum > 0.0 ? sum / w_sum / vec2(size) : (vec2(best) + 0.5) / vec2(size);
}

void main()
{
    int col = int(gl_FragCoord.x);
    int row = int(gl_FragCoord.y);
    ivec2 tile = ivec2(col % TILES_X, col / TILES_X);

    ivec2 size = textureSize(diffuseRect, 0);
    ivec2 lo = tile * size / ivec2(TILES_X, TILES_Y);
    ivec2 hi = (tile + 1) * size / ivec2(TILES_X, TILES_Y);

    // The texel that is brightest and stands out most from the texels two out on each side:
    // a lamp is a peak, a lit wall a plateau
    ivec2 best = lo;
    float best_score = 0.0;
    float best_peak = 0.0;
    for (int y = lo.y; y < hi.y; ++y)
    {
        for (int x = lo.x; x < hi.x; ++x)
        {
            ivec2 p = ivec2(x, y);
            float l = lumAt(diffuseRect, p, size);
            if (l <= 0.0)
            {
                continue;
            }
            float ring = (lumAt(diffuseRect, p + ivec2(2, 0), size) + lumAt(diffuseRect, p - ivec2(2, 0), size) +
                          lumAt(diffuseRect, p + ivec2(0, 2), size) + lumAt(diffuseRect, p - ivec2(0, 2), size)) * 0.25;
            float peak = clamp(1.0 - ring / l, 0.0, 1.0);
            float score = l * peak * peak;
            if (score > best_score)
            {
                best_score = score;
                best = p;
                best_peak = peak;
            }
        }
    }

    vec4 pick = vec4(0.0);
    vec3 color = vec3(1.0);
    if (best_score > 0.0)
    {
        vec2 uv = (vec2(best) + 0.5) / vec2(size);
        uv = refine(diffuseRect, uv, 1);
        uv = refine(specularRect, uv, 2);
        uv = refine(emissiveRect, uv, 3);

        vec3 c = texelFetch(diffuseRect, best, 0).rgb;
        color = c / max(max(c.r, max(c.g, c.b)), 1e-4);
        float energy = lumAt(diffuseRect, best, size) * fs_pick_area;

        // Not the sky (the sun has its own flare), and not the glow the sun spreads onto whatever
        // stands in front of it
        float aspect = screen_res.x / max(screen_res.y, 1.0);
        float sun_dist = length((uv - fs_flare_sun.xy) * vec2(aspect, 1.0));
        bool near_sun = fs_flare_sun.w > 0.0 && sun_dist < fs_flare_sun.z * 3.0 + 0.05;
        if (isSky(textureLod(depthMap, uv, 0.0).r) > 0.0 || near_sun)
        {
            energy = 0.0;
        }
        pick = vec4(uv, energy, best_peak);
    }

    frag_color = row == 0 ? pick : vec4(color, 1.0);
}
