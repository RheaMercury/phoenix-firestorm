// Water ripple obstacles (FSRenderWaterRippleObstacles): which cells of the ripple grid are solid
// at the water surface (fswaterobstacles.cpp, fsWaterRippleSimF.glsl).
//
// Seen straight down, the walls of a post or a hull are edge on and draw nothing, so instead of
// looking for geometry at the surface this asks, for each cell, what the nearest surface straight
// above or below a test plane (just above the water) is. Inside a solid every surface faces away
// from you: above the plane its normal points up, below it down. From open water the nearest
// surface faces toward you. The blend keeps the minimum of each channel, so per cell:
//   r distance to the nearest surface facing away (inside something)
//   g distance to the nearest surface facing toward the plane (outside)
//   b depth of the water over the terrain, for the soft fade in shallow water
//   a 0 where a surface crosses the test plane (fsWaterObstacleG.glsl's water line strips)
// and the cell is solid when r < g or a is 0. Terrain is a height field, not a closed solid: above
// the water it is land (r), below it only gives the depth (b). Cleared to FS_OBST_FAR.

#define FS_OBST_FAR 60000.0

out vec4 frag_color;

uniform vec4 fs_obst_water; // y test plane above the water (m), z 1 for terrain, w deepest water recorded for the fade (m)

in float vary_height;
flat in float vary_slice; // 1 on a water line strip

void main()
{
    if (vary_slice > 0.5)
    {
        frag_color = vec4(FS_OBST_FAR, FS_OBST_FAR, FS_OBST_FAR, 0.0);
        return;
    }
    float above = vary_height - fs_obst_water.y;
    float dist = min(abs(above), FS_OBST_FAR * 0.5);
    vec4 result = vec4(FS_OBST_FAR);
    if (fs_obst_water.z > 0.5)
    {
        if (above > 0.0)
        {
            result.r = dist; // land
        }
        else if (-vary_height < fs_obst_water.w)
        {
            result.b = max(-vary_height, 0.0);
        }
    }
    else if (gl_FrontFacing == (above > 0.0))
    {
        result.r = dist; // faces away from the plane: the plane is inside
    }
    else
    {
        result.g = dist;
    }
    frag_color = result;
}
