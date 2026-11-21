// Water ripple obstacles (FSRenderWaterRippleObstacles): passes each triangle through for the
// facing test in fsWaterObstacleF.glsl and, for the geometry of objects, also draws where the
// triangle crosses the test plane just above the water: a strip as wide as a cell and a half
// along that line (fswaterobstacles.cpp).
//
// The facing test alone needs closed solids: a post or hull whose top and bottom were left out
// (common in mesh, where nobody sees them) or with a deck or roof over it reads as open water.
// Where a surface crosses the water line is solid whatever the rest of the object looks like, so
// every wall, post and hull side marks its own outline.

layout(triangles) in;
layout(triangle_strip, max_vertices = 7) out;

uniform vec4 fs_obst_view;   // xy agent-space corner of the mask, z 2 / its size in metres, w half width of a water line strip (m)
uniform vec4 fs_obst_water;  // x water height (agent z), y test plane above the water (m), z 1 for terrain

in vec3 fs_obst_agent[];     // agent-space positions (fsWaterObstacleV.glsl)

out float vary_height;       // height above the water surface, metres
flat out float vary_slice;   // 1 on the water line strips

vec4 fsObstProject(vec2 xy)
{
    // Orthographic, x east and y north: a triangle wound counter-clockwise seen from above faces up
    return vec4((xy - fs_obst_view.xy) * fs_obst_view.z - 1.0, 0.5, 1.0);
}

void main()
{
    for (int i = 0; i < 3; ++i)
    {
        gl_Position = fsObstProject(fs_obst_agent[i].xy);
        vary_height = fs_obst_agent[i].z - fs_obst_water.x;
        vary_slice = 0.0;
        EmitVertex();
    }
    EndPrimitive();

    if (fs_obst_water.z > 0.5)
    {
        return; // terrain: above the water it is land already
    }

    // Where the edges cross the test plane
    float h[3];
    for (int i = 0; i < 3; ++i)
    {
        h[i] = fs_obst_agent[i].z - fs_obst_water.x - fs_obst_water.y;
    }
    vec2 p[2];
    int count = 0;
    for (int i = 0; i < 3; ++i)
    {
        int j = (i + 1) % 3;
        if ((h[i] > 0.0) != (h[j] > 0.0) && count < 2)
        {
            float t = h[i] / (h[i] - h[j]);
            p[count] = mix(fs_obst_agent[i].xy, fs_obst_agent[j].xy, t);
            ++count;
        }
    }
    if (count < 2)
    {
        return;
    }

    // A strip along the line, reaching past its ends, wide enough that no cell centre it passes
    // near is missed
    vec2 d = p[1] - p[0];
    float len = length(d);
    vec2 along = (len > 1e-5 ? d / len : vec2(1.0, 0.0)) * fs_obst_view.w;
    vec2 side = vec2(-along.y, along.x);
    vec2 a = p[0] - along;
    vec2 b = p[1] + along;

    vary_height = 0.0;
    vary_slice = 1.0;
    gl_Position = fsObstProject(a - side);
    EmitVertex();
    vary_slice = 1.0;
    gl_Position = fsObstProject(a + side);
    EmitVertex();
    vary_slice = 1.0;
    gl_Position = fsObstProject(b - side);
    EmitVertex();
    vary_slice = 1.0;
    gl_Position = fsObstProject(b + side);
    EmitVertex();
    EndPrimitive();
}
