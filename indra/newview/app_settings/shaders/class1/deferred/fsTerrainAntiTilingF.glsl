// Terrain anti-tiling: breaks up the visible repetition of the four terrain textures
// (GPU Gems 1 ch. 20 "Texture Bombing" and Gems 2 ch. 12 in modern form)

// Linked as an extra fragment object into the legacy terrain program and the PBR terrain programs
// when FS_TERRAIN_ANTI_TILING is defined (FSRenderTerrainAntiTiling). Its value picks the method:
//  1: macro variation. Every layer is fetched a second time at a much larger, non-matching scale
//     and the two are blended by low frequency value noise, with a slight brightness and hue jitter.
//     One extra fetch per layer.
//  2: hex tiling (Mikkelsen 2022, "Practical Real-Time Hex-Tiling", JCGT 11(2)). The texture is
//     laid out on a grid of hexagonal cells, each with its own random offset (and rotation), and
//     three cells are blended at every pixel with weights sharpened by the colour's contrast, so
//     the seams follow the texture's features. Base colour and normal only, within a distance of
//     the camera; macro variation beyond it.
// Every fetch uses the derivatives of the original coordinate (textureGrad, rotated with the cell),
// so the mip level is the one the plain fetch would have picked and nothing seams at cell borders.
// The callers branch per fragment (terrain layers, triplanar axes, this distance), which is fine for
// that reason too. No extra textures: the noise is hashed from the texture coordinate.

// x: strength 0-1, y: distance in meters within which mode 2 hex-tiles, z: 1 rotates the hex cells
// (0 for textures with a direction, such as paving or planks), w unused
uniform vec4 fs_terrain_anti_tiling;

struct FSAntiTile
{
    vec2 uv;
    vec2 ddx;
    vec2 ddy;
    float hex;          // 1 near the camera: hex tiling, 0: macro variation, in between: both blended
#if FS_TERRAIN_ANTI_TILING == 2
    vec2 cell_uv[3];
    mat2 cell_rot[3];
    vec3 cell_w;        // barycentric weights of the three cells, before the contrast sharpening
#endif
};

// Hashes without sin(): stable for the large coordinates far out in a region
float fsATHash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

vec3 fsATHash32(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yxz + 33.33);
    return fract((p3.xxy + p3.yzz) * p3.zyx);
}

// Smooth value noise, 0-1
float fsATNoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = fsATHash12(i);
    float b = fsATHash12(i + vec2(1.0, 0.0));
    float c = fsATHash12(i + vec2(0.0, 1.0));
    float d = fsATHash12(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// The macro layer: about 1/7 of the frequency, a scale that never lines up with the original
// tiling, turned by an odd angle unless rotation is off, and shifted
const float FS_AT_MACRO_SCALE = 0.1437;

vec4 fsATMacro(sampler2D tex, FSAntiTile at, vec4 base)
{
    float strength = fs_terrain_anti_tiling.x;
    float ca = mix(1.0, 0.8, fs_terrain_anti_tiling.z);   // about 37 degrees, or none
    float sa = mix(0.0, 0.6, fs_terrain_anti_tiling.z);
    mat2 rot = mat2(ca, sa, -sa, ca);
    vec2 muv = rot * at.uv * FS_AT_MACRO_SCALE + vec2(0.37, 0.61);
    vec4 macro = textureGrad(tex, muv, rot * at.ddx * FS_AT_MACRO_SCALE, rot * at.ddy * FS_AT_MACRO_SCALE);

    // Blend noise with a period of a few tiles, and two slower ones for the jitter
    float n = fsATNoise(at.uv * 0.23);
    float nb = fsATNoise(at.uv * 0.071 + vec2(17.3, 5.1));
    float nh = fsATNoise(at.uv * 0.053 + vec2(-3.7, 29.9));
    vec4 c = mix(base, macro, smoothstep(0.2, 0.8, n) * 0.6 * strength);
    // brightness +-12%, a faint warm/cool shift: both average out to the original colour
    float b = 1.0 + (nb - 0.5) * 0.24 * strength;
    float h = (nh - 0.5) * 0.08 * strength;
    c.rgb *= b * vec3(1.0 + h, 1.0, 1.0 - h);
    return c;
}

#if FS_TERRAIN_ANTI_TILING == 2
const float FS_AT_HEX_CONTRAST = 0.6;   // how much the colour's luminance steers the cell weights
const float FS_AT_HEX_SHARPNESS = 7.0;  // exponent on the barycentric weights

vec2 fsATHexCentre(vec2 vtx)
{
    // the cell vertex back from the skewed grid to texture space
    return vec2(vtx.x + 0.5 * vtx.y, vtx.y * (1.0 / 1.15470054)) / (2.0 * sqrt(3.0));
}

mat2 fsATHexRotation(vec2 vtx)
{
    float angle = (fsATHash12(vtx + vec2(7.13, 3.71)) * 2.0 - 1.0) * 3.14159265 * fs_terrain_anti_tiling.z;
    float c = cos(angle);
    float s = sin(angle);
    return mat2(c, s, -s, c);
}
#endif

// Set up the anti-tiling for one texture coordinate. ddx, ddy: its screen derivatives, taken by the
// caller in uniform control flow.
FSAntiTile fsAntiTileBegin(vec2 uv, vec2 ddx, vec2 ddy)
{
    FSAntiTile at;
    at.uv = uv;
    at.ddx = ddx;
    at.ddy = ddy;
    at.hex = 0.0;
#if FS_TERRAIN_ANTI_TILING == 2
    float hex_dist = max(fs_terrain_anti_tiling.y, 1.0);
    // The distance along the view axis: 1/w of a perspective projection, with no input needed
    float dist = 1.0 / max(gl_FragCoord.w, 1e-6);
    at.hex = 1.0 - smoothstep(hex_dist * 0.8, hex_dist, dist);

    // The triangle grid whose vertices are the hex cell centres (Mikkelsen's TriangleGrid)
    vec2 st = uv * (2.0 * sqrt(3.0));
    vec2 skewed = vec2(st.x - 0.57735027 * st.y, 1.15470054 * st.y);
    vec2 base_id = floor(skewed);
    vec3 temp = vec3(fract(skewed), 0.0);
    temp.z = 1.0 - temp.x - temp.y;
    float s = step(0.0, -temp.z);
    float s2 = 2.0 * s - 1.0;
    at.cell_w = vec3(-temp.z * s2, s - temp.y * s2, s - temp.x * s2);
    vec2 vtx[3];
    vtx[0] = base_id + vec2(s, s);
    vtx[1] = base_id + vec2(s, 1.0 - s);
    vtx[2] = base_id + vec2(1.0 - s, s);

    // At strength 0 every cell samples the original coordinate, so the result is the plain fetch
    float strength = fs_terrain_anti_tiling.x;
    for (int i = 0; i < 3; ++i)
    {
        mat2 rot = fsATHexRotation(vtx[i]);
        rot = mat2(mix(vec2(1.0, 0.0), rot[0], strength), mix(vec2(0.0, 1.0), rot[1], strength));
        vec2 centre = fsATHexCentre(vtx[i]);
        at.cell_uv[i] = rot * (uv - centre) + centre + fsATHash32(vtx[i]).xy * 4.0 * strength;
        at.cell_rot[i] = rot;
    }
#endif
    return at;
}

// The colour of one layer. hex_w receives the cell weights fsAntiTileNormal needs for the same
// layer (unused unless mode 2).
vec4 fsAntiTileColor(sampler2D tex, FSAntiTile at, out vec3 hex_w)
{
    hex_w = vec3(1.0, 0.0, 0.0);
    vec4 plain = vec4(0.0);
    vec4 hex = vec4(0.0);
    if (at.hex < 1.0)
    {
        plain = fsATMacro(tex, at, textureGrad(tex, at.uv, at.ddx, at.ddy));
    }
#if FS_TERRAIN_ANTI_TILING == 2
    if (at.hex > 0.0)
    {
        vec4 c0 = textureGrad(tex, at.cell_uv[0], at.cell_rot[0] * at.ddx, at.cell_rot[0] * at.ddy);
        vec4 c1 = textureGrad(tex, at.cell_uv[1], at.cell_rot[1] * at.ddx, at.cell_rot[1] * at.ddy);
        vec4 c2 = textureGrad(tex, at.cell_uv[2], at.cell_rot[2] * at.ddx, at.cell_rot[2] * at.ddy);
        const vec3 lum = vec3(0.299, 0.587, 0.114);
        vec3 dw = vec3(dot(c0.rgb, lum), dot(c1.rgb, lum), dot(c2.rgb, lum));
        dw = mix(vec3(1.0), dw, FS_AT_HEX_CONTRAST);
        vec3 w = dw * pow(max(at.cell_w, vec3(0.0)), vec3(FS_AT_HEX_SHARPNESS));
        w /= max(w.x + w.y + w.z, 1e-6);
        hex_w = w;
        hex = c0 * w.x + c1 * w.y + c2 * w.z;
    }
#endif
    return mix(plain, hex, at.hex);
}

// The unpacked tangent space normal (xyz * 2 - 1) of one layer, its xy turned back with each cell's
// rotation so the lighting keeps its direction. hex_w: from fsAntiTileColor for the same layer.
vec3 fsAntiTileNormal(sampler2D tex, FSAntiTile at, vec3 hex_w)
{
    vec3 plain = vec3(0.0);
    vec3 hex = vec3(0.0);
    if (at.hex < 1.0)
    {
        plain = textureGrad(tex, at.uv, at.ddx, at.ddy).xyz * 2.0 - 1.0;
    }
#if FS_TERRAIN_ANTI_TILING == 2
    if (at.hex > 0.0)
    {
        vec3 n0 = textureGrad(tex, at.cell_uv[0], at.cell_rot[0] * at.ddx, at.cell_rot[0] * at.ddy).xyz * 2.0 - 1.0;
        vec3 n1 = textureGrad(tex, at.cell_uv[1], at.cell_rot[1] * at.ddx, at.cell_rot[1] * at.ddy).xyz * 2.0 - 1.0;
        vec3 n2 = textureGrad(tex, at.cell_uv[2], at.cell_rot[2] * at.ddx, at.cell_rot[2] * at.ddy).xyz * 2.0 - 1.0;
        // texture space to surface space: the inverse (transpose) of the cell's rotation
        n0.xy = n0.xy * at.cell_rot[0];
        n1.xy = n1.xy * at.cell_rot[1];
        n2.xy = n2.xy * at.cell_rot[2];
        hex = n0 * hex_w.x + n1 * hex_w.y + n2 * hex_w.z;
    }
#endif
    return mix(plain, hex, at.hex);
}

// What the terrain shaders call. FSAntiTile stays inside this file: NVIDIA's linker rejected the
// struct when it was also declared in the callers' separately compiled objects ("declaration of
// FSAntiTile conflicts with previous declaration"), so the callers pass plain values and the cells
// are set up again per call (a few hashes, deterministic, so the colour and the normal of a layer
// still use the same cells).
vec4 fsAntiTileSampleColor(sampler2D tex, vec2 uv, vec2 ddx, vec2 ddy, out vec3 hex_w)
{
    return fsAntiTileColor(tex, fsAntiTileBegin(uv, ddx, ddy), hex_w);
}

vec3 fsAntiTileSampleNormal(sampler2D tex, vec2 uv, vec2 ddx, vec2 ddy, vec3 hex_w)
{
    return fsAntiTileNormal(tex, fsAntiTileBegin(uv, ddx, ddy), hex_w);
}
