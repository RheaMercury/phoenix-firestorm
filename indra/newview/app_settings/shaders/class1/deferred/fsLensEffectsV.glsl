// Lens effects: the full screen triangle, and the sun's visibility for the lens flare

// postDeferredNoTCV.glsl plus the one value of the lens flare (fsLensEffectsF.glsl) that is the
// same for every pixel: how much of the sun's disc is uncovered. It takes 24 depth taps, which
// each pixel used to repeat; here the three vertices take them and hand the result on flat.

/*[EXTRA_CODE_HERE]*/

in vec3 position;

out vec2 vary_fragcoord;
flat out float vary_sun_vis;

uniform sampler2D depthMap;
uniform vec2 screen_res;
uniform vec3 sun_screen;        // xy in 0..1, z 1 when the sun is in front of the camera
uniform float flare_strength;
uniform int flare_hq;           // the high quality flare measures the sun in its own pass

// How much of the sun's disc is not covered by geometry: depth samples on two rings around it
float sunVisibility(vec2 sun)
{
    if (sun_screen.z < 0.5)
    {
        return 0.0;
    }
    float aspect = screen_res.x / screen_res.y;
    float visible = 0.0;
    const int TAPS = 12;
    for (int i = 0; i < TAPS; ++i)
    {
        float a = float(i) / float(TAPS) * 6.2831853;
        vec2 dir = vec2(cos(a) / aspect, sin(a));
        for (int ring = 1; ring <= 2; ++ring)
        {
            vec2 p = sun + dir * 0.006 * float(ring);
            if (p.x >= 0.0 && p.x <= 1.0 && p.y >= 0.0 && p.y <= 1.0)
            {
#ifdef REVERSE_Z
                visible += textureLod(depthMap, p, 0.0).r <= 0.0 ? 1.0 : 0.0; // the far plane is 0
#else
                visible += textureLod(depthMap, p, 0.0).r >= 1.0 ? 1.0 : 0.0;
#endif
            }
        }
    }
    visible /= float(TAPS * 2);

    // Fade out as the sun leaves the frame instead of cutting off at the edge
    vec2 outside = max(abs(sun - 0.5) - 0.5, vec2(0.0));
    return visible * clamp(1.0 - length(outside) * 8.0, 0.0, 1.0);
}

void main()
{
    vec4 pos = vec4(position.xyz, 1.0);
    gl_Position = pos;
    vary_fragcoord = (pos.xy*0.5+0.5);
    vary_sun_vis = (flare_strength > 0.0 && flare_hq == 0) ? sunVisibility(sun_screen.xy) : 0.0;
}
