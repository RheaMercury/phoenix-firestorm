// Interactive water ripples (FSRenderWaterRipples): one step of the wave simulation, one fragment
// per grid cell (fswaterripples.cpp). The grid follows the camera in whole cells; the shift says
// where each cell was in the previous map, and cells that come in from outside start flat.
//
// The sources (avatars and moving objects crossing the water plane) are capsules from where they
// were at the last step to where they are, so a fast mover leaves a continuous wake at any frame
// rate. They push the surface as an impulse, and a damped wave equation carries it outward.
//
// With the region wind (FSRenderWindEffects), each step also pushes a faint noise into the
// surface: fine noise under larger gust patches, world anchored and carried downwind, which the
// wave equation turns into moving catspaw ripples. The push is sized to the damping, so it
// settles instead of building up. The surface current that carries everything downwind is a
// moving frame (fswaterripples.cpp), not a resample; only the foam goes a little faster.

#define FS_RIPPLE_MAX_SOURCES 64 // MAX_SOURCES in fswaterripples.cpp

out vec4 frag_color;

uniform sampler2D fs_ripple_prev; // r height (m), g height a step earlier, b foam
uniform vec2 fs_ripple_shift;     // whole cells the grid moved since fs_ripple_prev was written
uniform vec4 fs_ripple_sim;       // x (speed * step / cell)^2, y kept per step, z foam kept per step, w foam gain (0 = none)
uniform vec2 fs_ripple_cell;      // x cell size in metres, y cells across
uniform int fs_ripple_count;      // sources splatted in this step
uniform vec4 fs_ripple_seg[FS_RIPPLE_MAX_SOURCES]; // capsule axis from xy to zw, in cells
uniform vec4 fs_ripple_src[FS_RIPPLE_MAX_SOURCES]; // x radius in cells, y height added at the centre (m)
uniform vec4 fs_ripple_wind;      // x wind push per step (m, 0 = no wind), y gusts (0 to 1), zw foam's extra drift (cells per step)
uniform vec4 fs_ripple_wind_o1;   // noise coordinates at the grid's corner: xy fine layer 1, zw fine layer 2
uniform vec4 fs_ripple_wind_o2;   // xy gust patch noise coordinates at the corner, z fine layer 1 weight, w fine layer 2 weight
uniform vec3 fs_ripple_wind_s;    // noise cells per grid cell: fine layer 1, fine layer 2, gust patches

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

// Gradient noise, about -0.7 to 0.7, repeating every 256 cells (wrapNoise in fswaterripples.cpp).
// Little of it is slow and broad, so the push hardly lifts or sinks whole areas.
vec2 fsRippleWindGrad(ivec2 i)
{
    uvec2 u = uvec2(i & ivec2(255));
    uint h = (u.x * 0x8da6b343u) ^ (u.y * 0xd8163841u);
    h ^= h >> 16u;
    h *= 0x7feb352du;
    h ^= h >> 15u;
    h *= 0x846ca68bu;
    h ^= h >> 16u;
    return vec2(float(h & 0xffffu), float(h >> 16u)) * (1.0 / 32768.0) - 1.0;
}

float fsRippleWindNoise(vec2 p)
{
    vec2 c = floor(p);
    vec2 f = p - c;
    ivec2 i = ivec2(c);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = dot(fsRippleWindGrad(i), f);
    float b = dot(fsRippleWindGrad(i + ivec2(1, 0)), f - vec2(1.0, 0.0));
    float d = dot(fsRippleWindGrad(i + ivec2(0, 1)), f - vec2(0.0, 1.0));
    float e = dot(fsRippleWindGrad(i + ivec2(1, 1)), f - vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(d, e, u.x), u.y);
}

// The wind's push at a cell centre p (in cells), before its strength: fine ripples, strongest in
// the gust patches, which cover more of the water in a gust
float fsRippleWind(vec2 p)
{
    float fine = 0.0;
    if (fs_ripple_wind_o2.z > 0.0)
    {
        fine += fs_ripple_wind_o2.z * fsRippleWindNoise(fs_ripple_wind_o1.xy + p * fs_ripple_wind_s.x);
    }
    if (fs_ripple_wind_o2.w > 0.0)
    {
        fine += fs_ripple_wind_o2.w * fsRippleWindNoise(fs_ripple_wind_o1.zw + p * fs_ripple_wind_s.y);
    }
    float gust_patch = fsRippleWindNoise(fs_ripple_wind_o2.xy + p * fs_ripple_wind_s.z) * 0.5 + 0.5;
    float lo = 0.55 - 0.3 * clamp(fs_ripple_wind.y, 0.0, 1.0);
    return fine * (0.15 + 0.85 * smoothstep(lo, lo + 0.3, gust_patch));
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

    // The wind: pushed the same way, every step
    if (fs_ripple_wind.x > 0.0)
    {
        h += fs_ripple_wind.x * fsRippleWind(p);
    }

    // Damped wave equation. Near the grid's edges the waves are absorbed (a sponge), so they
    // don't bounce back off the edge into view.
    float edge = float(min(min(cell.x, cell.y), min(res - 1 - cell.x, res - 1 - cell.y)));
    float keep = fs_ripple_sim.y * mix(0.9, 1.0, smoothstep(0.0, float(res) * 0.06, edge));
    float lap = hl + hr + hd + hu - 4.0 * h;
    float h_new = fsRippleSafe((2.0 * h - h_prev + fs_ripple_sim.x * lap) * keep);

    // Churned water: where the surface is steep it turns white, and the foam lingers and fades
    float foam_was = here.b;
    if (fs_ripple_sim.w > 0.0 && (fs_ripple_wind.z != 0.0 || fs_ripple_wind.w != 0.0))
    {
        // Foam rides on top and the wind pushes it on a little: read it from a fraction of a
        // cell upwind, blending the four cells around
        vec2 from = vec2(at) - fs_ripple_wind.zw;
        vec2 base = floor(from);
        vec2 f = from - base;
        ivec2 b = ivec2(base);
        float f00 = fsRippleSafe(fsRippleCell(b, res).b);
        float f10 = fsRippleSafe(fsRippleCell(b + ivec2(1, 0), res).b);
        float f01 = fsRippleSafe(fsRippleCell(b + ivec2(0, 1), res).b);
        float f11 = fsRippleSafe(fsRippleCell(b + ivec2(1, 1), res).b);
        foam_was = mix(mix(f00, f10, f.x), mix(f01, f11, f.x), f.y);
    }
    float foam = fsRippleSafe(foam_was) * fs_ripple_sim.z;
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
