// Soft occlusion around avatars (FSRenderAvatarOcclusion): capsules on the skeletons of the nearest
// avatars darken the ambient light of what is around them (GPU Gems 2 ch. 14 in its modern form:
// capsule occlusion as in The Last of Us and Unreal's capsule shadows, Quilez's sphere occlusion).

// Linked as an extra fragment object into the soften program when FS_AVATAR_OCCLUSION is defined.
// fsavatarocclusion.cpp fills the block each frame in eye space: per avatar FS_CAP_PER_AVATAR
// capsules (start + radius, end) and a bounding sphere of everything the avatar can darken, so a
// pixel away from every avatar costs one distance test per avatar.

#define FS_CAP_AVATARS 16
#define FS_CAP_PER_AVATAR 14

layout(std140) uniform FSAvatarCapsules
{
    vec4 fs_cap_a[FS_CAP_AVATARS * FS_CAP_PER_AVATAR];  // xyz: start, w: radius (0: no capsule)
    vec4 fs_cap_b[FS_CAP_AVATARS * FS_CAP_PER_AVATAR];  // xyz: end
    vec4 fs_cap_bounds[FS_CAP_AVATARS];                 // xyz: centre, w: radius of influence
};

uniform vec4 fs_cap_params; // x: avatars in the block, y: strength 0-1, z: sky shadow strength 0-1
uniform vec3 fs_cap_sky;    // the open sky's direction (up), eye space

// Half angle of the cone of sky light the soft shadow takes away from, in radians: the ambient
// light comes from a wide part of the sky, so the shadow is broad and soft
#define FS_CAP_SKY_CONE 0.75

// Fraction of the cosine-weighted hemisphere over n at pos that a sphere covers (Quilez, with the
// Lagarde and de Rousiers correction where the sphere crosses the horizon). Points inside or right
// on the sphere get none: those are the avatar's own surface.
float fsCapSphereOcclusion(vec3 pos, vec3 n, vec3 centre, float radius)
{
    vec3 di = centre - pos;
    float l = length(di);
    float h = l / radius;
    float nl = dot(n, di / max(l, 1e-5));
    float h2 = h * h;
    float k2 = 1.0 - h2 * nl * nl;
    float res = max(0.0, nl) / h2;
    if (k2 > 0.0)
    {
        res = clamp(0.5 * (nl * h + 1.0) / h2, 0.0, 1.0);
        res = sqrt(res * res * res);
    }
    return res * smoothstep(0.9, 1.15, h);
}

// Fraction of the sky cone around dir (half angle FS_CAP_SKY_CONE) the capsule a-b of the given
// radius hides, seen from pos: the capsule's point nearest the ray along dir, as a sphere, against
// the cone by their angles (overlap of the two angular discs, scaled down by area for a sphere
// smaller than the cone)
float fsCapConeOcclusion(vec3 pos, vec3 dir, vec3 a, vec3 b, float radius)
{
    // closest points of the ray pos + s dir and the segment a + t (b - a)
    vec3 v = b - a;
    vec3 w = pos - a;
    float bv = dot(dir, v);
    float vv = max(dot(v, v), 1e-6);
    float dw = dot(dir, w);
    float vw = dot(v, w);
    float den = vv - bv * bv;
    float t = den > 1e-6 ? clamp((vw - bv * dw) / den, 0.0, 1.0) : 0.0;
    vec3 c = a + v * t - pos;
    float dist = length(c);
    if (dist <= radius || dot(c, dir) <= 0.0)
    {
        return 0.0; // inside (the avatar itself) or below the horizon of the cone's axis
    }
    float beta = asin(clamp(radius / dist, 0.0, 1.0));             // angular radius of the sphere
    float theta = acos(clamp(dot(c / dist, dir), -1.0, 1.0));      // its angle off the axis
    float cover = clamp((FS_CAP_SKY_CONE + beta - theta) / (2.0 * min(FS_CAP_SKY_CONE, beta)), 0.0, 1.0);
    // Not right next to the capsule: there the sphere term above already darkens, and that is
    // mostly the avatar's own body, which should not lose its sky light to itself
    return cover * min(1.0, (beta * beta) / (FS_CAP_SKY_CONE * FS_CAP_SKY_CONE)) * smoothstep(1.5, 3.0, dist / radius);
}

// Visibility of the ambient light at eye space pos with normal n: 1 where no avatar is near
float fsAvatarOcclusion(vec3 pos, vec3 n)
{
    int avatars = int(fs_cap_params.x);
    float strength = fs_cap_params.y;
    if (avatars <= 0 || strength <= 0.0)
    {
        return 1.0;
    }
    float sky = fs_cap_params.z;
    float vis = 1.0;
    float sky_vis = 1.0;
    for (int av = 0; av < FS_CAP_AVATARS; ++av)
    {
        if (av >= avatars)
        {
            break;
        }
        vec4 bounds = fs_cap_bounds[av];
        vec3 d = pos - bounds.xyz;
        if (dot(d, d) > bounds.w * bounds.w)
        {
            continue;
        }
        for (int c = 0; c < FS_CAP_PER_AVATAR; ++c)
        {
            int i = av * FS_CAP_PER_AVATAR + c;
            vec4 a = fs_cap_a[i];
            if (a.w <= 0.0)
            {
                continue;
            }
            vec3 b = fs_cap_b[i].xyz;
            // the capsule as the sphere at its point nearest the pixel
            vec3 ab = b - a.xyz;
            float t = clamp(dot(pos - a.xyz, ab) / max(dot(ab, ab), 1e-6), 0.0, 1.0);
            vis *= 1.0 - fsCapSphereOcclusion(pos, n, a.xyz + ab * t, a.w);
            if (sky > 0.0)
            {
                sky_vis *= 1.0 - fsCapConeOcclusion(pos, fs_cap_sky, a.xyz, b, a.w);
            }
        }
    }
    // the sky shadow only on what faces the sky at all
    float facing = clamp(dot(n, fs_cap_sky) * 0.5 + 0.5, 0.0, 1.0);
    vis *= mix(1.0, sky_vis, sky * facing);
    return mix(1.0, vis, strength);
}
