// Bloom: spread a smaller level back up onto the next larger one

/*[EXTRA_CODE_HERE]*/

// Drawn with additive blending onto the larger level, which still holds its own downsampled
// image, so every level's blur ends up summed into the top one.

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform sampler2D diffuseRect;  // smaller level
uniform vec2 src_texel;         // one texel of diffuseRect
uniform int fs_bloom_hq;        // FSRenderBloomHQFilter: 9 tap tent instead of the ring below

// Tap spacing of the tent, in source texels. sqrt(8/3) gives it the same variance as the ring
// kernel below, so the bloom keeps its size and only loses the ring's hollow middle.
#define FS_BLOOM_TENT_RADIUS 1.633

void main()
{
    vec2 tc = vary_fragcoord.xy;

    if (fs_bloom_hq != 0)
    {
        // A 3x3 tent (1 2 1 / 2 4 2 / 1 2 1) / 16 with its centre tap: a smooth bump with no hole
        vec2 r = src_texel * FS_BLOOM_TENT_RADIUS;
        vec3 t = texture(diffuseRect, tc).rgb * 4.0;
        t += (texture(diffuseRect, tc + vec2(-r.x, 0.0)).rgb + texture(diffuseRect, tc + vec2(r.x, 0.0)).rgb +
              texture(diffuseRect, tc + vec2(0.0, -r.y)).rgb + texture(diffuseRect, tc + vec2(0.0, r.y)).rgb) * 2.0;
        t += texture(diffuseRect, tc + vec2(-r.x, -r.y)).rgb + texture(diffuseRect, tc + vec2(r.x, -r.y)).rgb +
             texture(diffuseRect, tc + vec2(-r.x,  r.y)).rgb + texture(diffuseRect, tc + vec2(r.x,  r.y)).rgb;
        frag_color = vec4(t * 0.0625, 1.0);
        return;
    }

    // A ring: four edge taps two texels out, four diagonal taps one texel out at double weight
    vec3 c = texture(diffuseRect, tc + vec2(-2.0,  0.0) * src_texel).rgb;
    c += texture(diffuseRect, tc + vec2( 2.0,  0.0) * src_texel).rgb;
    c += texture(diffuseRect, tc + vec2( 0.0, -2.0) * src_texel).rgb;
    c += texture(diffuseRect, tc + vec2( 0.0,  2.0) * src_texel).rgb;
    c += texture(diffuseRect, tc + vec2(-1.0, -1.0) * src_texel).rgb * 2.0;
    c += texture(diffuseRect, tc + vec2( 1.0, -1.0) * src_texel).rgb * 2.0;
    c += texture(diffuseRect, tc + vec2(-1.0,  1.0) * src_texel).rgb * 2.0;
    c += texture(diffuseRect, tc + vec2( 1.0,  1.0) * src_texel).rgb * 2.0;

    frag_color = vec4(c / 12.0, 1.0);
}
