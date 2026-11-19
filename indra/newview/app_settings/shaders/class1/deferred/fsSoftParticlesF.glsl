// Soft particles: particles fade out where they meet walls, the ground and the water instead of
// cutting a hard line through them (GPU Gems 3, chapter 23)

// Linked as an extra fragment object into the world alpha, fullbright alpha and glow programs when
// FS_SOFT_PARTICLES is defined (FSRenderSoftParticles). FSSoftParticles::apply (fssoftparticles.cpp)
// sets the uniforms before each draw: for particles only, everything else (and every program it
// doesn't reach) reads 0 and keeps its alpha.
//
// The depth is the scene's before any alpha was drawn (the copy LLPipeline takes for name tags), so a
// particle fades against opaque surfaces, not against other particles or glass. Both depths are
// turned into distances from the camera with the projection the pass draws with, and the alpha
// falls off linearly over the fade distance in front of the surface.

// x: 1 / fade distance in meters (0: not a particle, no fade), y, z: projection [2][2] and [3][2],
// w: 1 when the draw blends with source factor one (additive), so the colour fades too
uniform vec4 fs_soft_particle;
// 1 / fade distance against the water plane, 0 when no water is drawn in this view
uniform float fs_soft_particle_water;
uniform sampler2D fs_soft_depth;
uniform vec4 waterPlane;

// distance from the camera of a window depth
float fsSoftParticleDistance(float window_depth)
{
    return fs_soft_particle.z / (window_depth * 2.0 - 1.0 + fs_soft_particle.y);
}

// Fade from the opaque scene in front of which this fragment lies, 1 = no fade
float fsSoftParticleFade()
{
    if (fs_soft_particle.x <= 0.0)
    {
        return 1.0;
    }
    float scene = texelFetch(fs_soft_depth, ivec2(gl_FragCoord.xy), 0).r;
    float gap = fsSoftParticleDistance(scene) - fsSoftParticleDistance(gl_FragCoord.z);
    return clamp(gap * fs_soft_particle.x, 0.0, 1.0);
}

// The same, with the water surface too: pos is the fragment's eye space position. Only where the
// fragment is on the camera's side of the water, the other side is clipped or fogged anyway.
float fsSoftParticleFade(vec3 pos)
{
    float fade = fsSoftParticleFade();
    if (fs_soft_particle.x > 0.0 && fs_soft_particle_water > 0.0)
    {
        float frag_side = dot(pos, waterPlane.xyz) + waterPlane.w;
        float camera_side = waterPlane.w;
        if (frag_side * camera_side > 0.0)
        {
            fade = min(fade, clamp(abs(frag_side) * fs_soft_particle_water, 0.0, 1.0));
        }
    }
    return fade;
}

// The colour to write: alpha faded, and the colour too for additive draws, whose alpha may not scale it
vec4 fsSoftParticleApply(vec4 color, float fade)
{
    color.a *= fade;
    if (fs_soft_particle.w > 0.5)
    {
        color.rgb *= fade;
    }
    return color;
}
