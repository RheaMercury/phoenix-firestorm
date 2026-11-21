// Water ripple obstacles (FSRenderWaterRippleObstacles): the obstacle mask, seen straight down
// over the ripple grid (fswaterobstacles.cpp). Positions only. The projection is built in
// fsWaterObstacleG.glsl from two uniforms, not taken from the viewer's matrices, so neither the
// reverse-Z remap nor the TAA jitter that those carry ever reaches this pass.

in vec3 position;

uniform mat4 fs_obst_model;  // the geometry's model matrix, to agent space

out vec3 fs_obst_agent;      // agent-space position, for fsWaterObstacleG.glsl

void main()
{
    vec4 agent = fs_obst_model * vec4(position, 1.0);
    fs_obst_agent = agent.xyz;
    // Replaced by the geometry shader; depth is not used
    gl_Position = vec4(0.0, 0.0, 0.5, 1.0);
}
