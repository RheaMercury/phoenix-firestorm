// Interactive water ripples (FSRenderWaterRipples): one step of the wave simulation, one fragment
// per grid cell (fswaterripples.cpp). The grid follows the camera in whole cells; the shift says
// where each cell was in the previous map, and cells that come in from outside start flat.
//
// The sources (avatars and moving objects crossing the water plane) are capsules from where they
// were at the last step to where they are, so a fast mover leaves a continuous wake at any frame
// rate. They push the surface as an impulse, and a damped wave equation carries it outward.

#define FS_RIPPLE_MAX_SOURCES 64 // MAX_SOURCES in fswaterripples.cpp

out vec4 frag_color;

uniform sampler2D fs_ripple_prev; // r height (m), g height a step earlier, b foam
uniform vec2 fs_ripple_shift;     // whole cells the grid moved since fs_ripple_prev was written
uniform vec4 fs_ripple_sim;       // x (speed * step / cell)^2, y kept per step, z foam kept per step, w foam gain (0 = none)
uniform vec2 fs_ripple_cell;      // x cell size in metres, y cells across
uniform int fs_ripple_count;      // sources splatted in this step
uniform vec4 fs_ripple_seg[FS_RIPPLE_MAX_SOURCES]; // capsule axis from xy to zw, in cells
uniform vec4 fs_ripple_src[FS_RIPPLE_MAX_SOURCES]; // x radius in cells, y height added at the centre (m)

// Whatever the driver or a bad frame left there, the simulation never carries a NaN or a runaway
float fsRippleSafe(float v)
{
    return (isnan(v) || isinf(v)) ? 0.0 : clamp(v, -4.0, 4.0);
}

vec4 fsRippleCell(ivec2 c, int res)
{
    if (any(lessThan(c, ivec2(0))) || any(greaterThanEqual(c, ivec2(res))))
    {
        return vec4(0.0); // outside the previous grid: flat, still water
    }
    return texelFetch(fs_ripple_prev, c, 0);
}

void main()
{
    int res = int(fs_ripple_cell.y + 0.5);
    ivec2 cell = ivec2(gl_FragCoord.xy);
    ivec2 at = cell + ivec2(fs_ripple_shift);

    vec4 here = fsRippleCell(at, res);
    float h = fsRippleSafe(here.r);
    float h_prev = fsRippleSafe(here.g);
    float hl = fsRippleSafe(fsRippleCell(at - ivec2(1, 0), res).r);
    float hr = fsRippleSafe(fsRippleCell(at + ivec2(1, 0), res).r);
    float hd = fsRippleSafe(fsRippleCell(at - ivec2(0, 1), res).r);
    float hu = fsRippleSafe(fsRippleCell(at + ivec2(0, 1), res).r);

    // Sources: a smooth bump along each capsule, added to the height but not to the height a step
    // earlier, so it moves the surface (an impulse) and the wave equation spreads it as rings
    vec2 p = vec2(cell) + 0.5;
    int count = min(fs_ripple_count, FS_RIPPLE_MAX_SOURCES);
    for (int i = 0; i < count; ++i)
    {
        vec4 seg = fs_ripple_seg[i];
        vec4 src = fs_ripple_src[i];
        vec2 ab = seg.zw - seg.xy;
        float t = clamp(dot(p - seg.xy, ab) / max(dot(ab, ab), 1e-4), 0.0, 1.0);
        float d = length(p - seg.xy - ab * t) / max(src.x, 1.0);
        if (d < 1.0)
        {
            float w = 1.0 - d * d;
            h += src.y * w * w;
        }
    }

    // Damped wave equation. Near the grid's edges the waves are absorbed (a sponge), so they
    // don't bounce back off the edge into view.
    float edge = float(min(min(cell.x, cell.y), min(res - 1 - cell.x, res - 1 - cell.y)));
    float keep = fs_ripple_sim.y * mix(0.9, 1.0, smoothstep(0.0, float(res) * 0.06, edge));
    float lap = hl + hr + hd + hu - 4.0 * h;
    float h_new = fsRippleSafe((2.0 * h - h_prev + fs_ripple_sim.x * lap) * keep);

    // Churned water: where the surface is steep it turns white, and the foam lingers and fades
    float foam = fsRippleSafe(here.b) * fs_ripple_sim.z;
    if (fs_ripple_sim.w > 0.0)
    {
        vec2 grad = vec2(hr - hl, hu - hd) / (2.0 * max(fs_ripple_cell.x, 1e-3));
        float churn = length(grad) * fs_ripple_sim.w;
        foam = max(foam, smoothstep(0.5, 1.0, churn));
    }
    else
    {
        foam = 0.0;
    }

    frag_color = vec4(h_new, h, clamp(foam, 0.0, 1.0), 0.0);
}
