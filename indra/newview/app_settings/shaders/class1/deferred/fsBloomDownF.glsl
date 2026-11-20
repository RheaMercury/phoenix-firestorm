// Bloom: halve the resolution of the previous level, blurring as it goes

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform sampler2D diffuseRect;  // previous, larger level
uniform vec2 src_texel;         // one texel of diffuseRect
uniform int fs_bloom_hq;        // FSRenderBloomHQFilter: 13 tap kernel instead of 5

void main()
{
    vec2 tc = vary_fragcoord.xy;

    if (fs_bloom_hq != 0)
    {
        // Jimenez 2014 (Call of Duty: Advanced Warfare): 13 bilinear taps, the centre box at
        // half the weight and four overlapping boxes two texels out at a quarter together. Every
        // source texel is covered with a smooth falloff, so a thin bright edge moving by one texel
        // no longer pulses in and out of the kernel as it does with the 5 tap one below.
        vec3 a = texture(diffuseRect, tc + vec2(-2.0,  2.0) * src_texel).rgb;
        vec3 b = texture(diffuseRect, tc + vec2( 0.0,  2.0) * src_texel).rgb;
        vec3 c = texture(diffuseRect, tc + vec2( 2.0,  2.0) * src_texel).rgb;
        vec3 d = texture(diffuseRect, tc + vec2(-2.0,  0.0) * src_texel).rgb;
        vec3 e = texture(diffuseRect, tc).rgb;
        vec3 f = texture(diffuseRect, tc + vec2( 2.0,  0.0) * src_texel).rgb;
        vec3 g = texture(diffuseRect, tc + vec2(-2.0, -2.0) * src_texel).rgb;
        vec3 h = texture(diffuseRect, tc + vec2( 0.0, -2.0) * src_texel).rgb;
        vec3 i = texture(diffuseRect, tc + vec2( 2.0, -2.0) * src_texel).rgb;
        vec3 j = texture(diffuseRect, tc + vec2(-1.0,  1.0) * src_texel).rgb;
        vec3 k = texture(diffuseRect, tc + vec2( 1.0,  1.0) * src_texel).rgb;
        vec3 l = texture(diffuseRect, tc + vec2(-1.0, -1.0) * src_texel).rgb;
        vec3 m = texture(diffuseRect, tc + vec2( 1.0, -1.0) * src_texel).rgb;

        vec3 hq = e * 0.125;
        hq += (a + c + g + i) * 0.03125;
        hq += (b + d + f + h) * 0.0625;
        hq += (j + k + l + m) * 0.125;
        frag_color = vec4(hq, 1.0);
        return;
    }

    // Centre plus four diagonal bilinear taps: a 5 tap kernel reaching about two source texels
    // each way, cheap enough to run on every level
    vec3 c = texture(diffuseRect, tc).rgb * 4.0;
    c += texture(diffuseRect, tc + vec2(-1.0, -1.0) * src_texel).rgb;
    c += texture(diffuseRect, tc + vec2( 1.0, -1.0) * src_texel).rgb;
    c += texture(diffuseRect, tc + vec2(-1.0,  1.0) * src_texel).rgb;
    c += texture(diffuseRect, tc + vec2( 1.0,  1.0) * src_texel).rgb;

    frag_color = vec4(c * 0.125, 1.0);
}
