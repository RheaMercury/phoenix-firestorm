// High quality lens flare (FSRenderLensFlareHQ): keep the strongest of the lights the pick pass
// found, one per column of an 8 x 2 target (the same layout as the pick target). A light that only
// just makes the cut fades by how far it is ahead of the first one left out, so two lights of
// about the same strength trading places do not pop their flares on and off.

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

uniform sampler2D diffuseRect;  // the pick target, 32 x 2

const int PICKS = 32;
const int KEEP = 8;

void main()
{
    int k = int(gl_FragCoord.x);
    int row = int(gl_FragCoord.y);

    float energy[PICKS];
    for (int i = 0; i < PICKS; ++i)
    {
        energy[i] = texelFetch(diffuseRect, ivec2(i, 0), 0).z;
    }

    // The rank of every pick (ties go to the lower index), the one ranked k and the first left out
    int chosen = -1;
    float first_out = 0.0;
    for (int i = 0; i < PICKS; ++i)
    {
        int rank = 0;
        for (int j = 0; j < PICKS; ++j)
        {
            rank += (energy[j] > energy[i] || (energy[j] == energy[i] && j < i)) ? 1 : 0;
        }
        if (rank == k)
        {
            chosen = i;
        }
        if (rank == KEEP)
        {
            first_out = energy[i];
        }
    }

    vec4 result = vec4(0.0);
    if (chosen >= 0 && energy[chosen] > 0.0)
    {
        result = texelFetch(diffuseRect, ivec2(chosen, row), 0);
        if (row == 0)
        {
            float lead = (energy[chosen] - first_out) / max(0.5 * energy[chosen], 1e-6);
            result.z *= clamp(lead, 0.0, 1.0);
        }
    }
    frag_color = result;
}
