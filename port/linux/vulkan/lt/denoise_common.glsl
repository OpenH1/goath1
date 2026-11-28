// W3, the denoiser's shared GLSL (xgpu_vulkan_denoise.c; ~/dev/rt2-realtime-design.md section 1.5): its uniforms, the
// pre-pass's inputs and guides read (xgpu_spirv.h's C1), and the reprojection to the frame before.

// (xgpu.h, xgpu_spirv.h)
#define INPUT_PRESENT 0x400u
#define MOTION_NONE 0u
#define MOTION_FLIP 0x80008000u
#define MOTION_RANGE 0.25

// control.y's flags (xgpu_vulkan_denoise.c's)
#define HISTORY 1
#define GRADIENTS 2
#define FIRST 4
#define CAMERA_ONLY 8
// (checking, the negative control: an exponential average from the start, the history's count held at the cap)
#define EMA 16
// (the specular channel, xgpu_vulkan_denoise_specular_draw: the raw's a its hit distance, less than 0 where the pixel
// has no specular; its history reprojected from the reflection's virtual point, the count capped by roughness; the
// a-trous passes stopped at pixels without one)
#define SPECULAR 32
// (checking: the temporal stage writes the guides' motion less the cameras' reprojection, in pixels; with
// MOTION_RAW too the motion itself)
#define MOTION_VIEW 128
#define MOTION_RAW 256

// the draw set's uniforms: struct xgpu_world_ao_pass, then (at 1024) xgpu_vulkan_denoise.c's struct denoise_params
layout(set = 0, binding = 1, std140) uniform Pass
{
    vec4 camera[4];
    ivec4 corners;
    vec4 cascades[4];
    vec4 previous[4];
    ivec4 previous_corners;
    vec4 history;
    // x, y the images' size; z, w the gradients'
    layout(offset = 1024) vec4 size;
    // x the count's cap, y the flags, z the a-trous step (pixels)
    ivec4 control;
    // x the previous frame's number (its history.y: its gradient pixels' and seed's), y the luminance's edge-stop
    // (sigma, in standard deviations), z the depth's (a share of the distance), w the shaded normal's (1 - cos)
    vec4 tuning;
    // (the guide's FOLD, lt/denoise_guide.frag) x, y the bounce's size (blocks), z its phase this frame, w its flags
    vec4 bounce;
    // (lt/denoise_pair.glsl's FINAL) x the suns, then each sun's forward and power (lt/e1.frag's light table rows)
    ivec4 suns;
    vec4 sun[16];
    // (lt/denoise_gradient.frag's deterministic half) x, y the light table's rows this frame and the frame before, z
    // the frame before's first row, w the flashlight's cookie's first texel in the cells
    ivec4 lights;
} pass;

const float COS_25 = 0.906;
const float DEPTH_SHARE = 0.03;

vec3 unfold(vec2 e)
{
    vec2 a = abs(e);
    float z = 1.0 - a.x - a.y;
    float t = max(-z, 0.0);
    vec2 f = e + vec2(e.x >= 0.0 ? -t : t, e.y >= 0.0 ? -t : t);
    return normalize(vec3(f, z));
}

float luminance(vec3 c)
{
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

// a pixel's world point from this frame's camera and its distance
vec3 world_point(ivec2 pixel, float distance)
{
    vec2 low = vec2(pass.corners.xy), size = vec2(pass.corners.zw) - low;
    vec2 screen = (vec2(pixel) + 0.5 - low) * 2.0 / size - 1.0;
    vec3 ray = normalize(pass.camera[1].xyz + pass.camera[2].xyz * screen.x + pass.camera[3].xyz * screen.y);
    return pass.camera[0].xyz + ray * distance;
}

// a pixel's world point from the previous frame's camera and its distance then
vec3 previous_point(ivec2 pixel, float distance)
{
    vec2 low = vec2(pass.previous_corners.xy), size = vec2(pass.previous_corners.zw) - low;
    vec2 screen = (vec2(pixel) + 0.5 - low) * 2.0 / size - 1.0;
    vec3 ray = normalize(pass.previous[1].xyz + pass.previous[2].xyz * screen.x + pass.previous[3].xyz * screen.y);
    return pass.previous[0].xyz + ray * distance;
}

// where p is in this frame's pixels (continuous); false off its view
bool project(vec3 p, out vec2 at)
{
    vec3 w = p - pass.camera[0].xyz, f = pass.camera[1].xyz, r = pass.camera[2].xyz, u = pass.camera[3].xyz;
    float along = dot(w, f) / dot(f, f);
    if (along <= 0.0)
        return false;
    vec2 screen = vec2(dot(w, r) / dot(r, r), dot(w, u) / dot(u, u)) / along;
    at = vec2(pass.corners.xy) + (screen + 1.0) * 0.5 * vec2(pass.corners.zw - pass.corners.xy);
    return all(greaterThanEqual(at, vec2(pass.corners.xy))) && all(lessThan(at, vec2(pass.corners.zw)));
}

// where p was in the frame before, in its pixels (continuous: a pixel's middle at +0.5): the guides' motion where they
// carry it (a moving object's, a skinned character's, the camera's), else the cameras' (E1's own reprojection,
// lt/e1.frag); false off its view
bool reproject_camera(vec3 p, out vec2 at)
{
    vec3 w = p - pass.previous[0].xyz, pf = pass.previous[1].xyz, pr = pass.previous[2].xyz;
    vec3 pu = pass.previous[3].xyz;
    float along = dot(w, pf) / dot(pf, pf);
    at = vec2(-1.0);
    if (along <= 0.0)
        return false;
    vec2 was = vec2(dot(w, pr) / dot(pr, pr), dot(w, pu) / dot(pu, pu)) / along;
    at = vec2(pass.previous_corners.xy) + (was + 1.0) * 0.5 * vec2(pass.previous_corners.zw - pass.previous_corners.xy);
    return true;
}

// (C1's motion: where the point is less where it was the frame before, a fraction of the window, x right and y
// down, in fixed point; the jitter left out, as the cameras' reprojection leaves it out)
vec2 motion_fraction(uint motion)
{
    return unpackSnorm2x16(motion ^ MOTION_FLIP) * MOTION_RANGE;
}

vec2 reproject_motion(ivec2 pixel, uint motion)
{
    return vec2(pixel) + 0.5 - motion_fraction(motion) * vec2(pass.corners.zw - pass.corners.xy);
}

bool reproject(vec3 p, ivec2 pixel, uint motion, out vec2 at)
{
    if (motion != MOTION_NONE && (pass.control.y & CAMERA_ONLY) == 0)
        at = reproject_motion(pixel, motion);
    else if (!reproject_camera(p, at))
        return false;
    return all(greaterThanEqual(at, vec2(pass.previous_corners.xy))) &&
        all(lessThan(at, vec2(pass.previous_corners.zw)));
}

// whether the previous frame's pixel q (its filter guide: before, lt/denoise_guide.frag) is the same surface as this
// one's (p, its geometric normal n, owner): drawn, the same owner, the distance from the previous camera within 3%,
// the normal within 25 degrees
bool same_surface(uvec2 before, vec3 p, vec3 n, uint owner)
{
    float was_distance = unpackHalf2x16(before.y).x, now_distance = length(p - pass.previous[0].xyz);
    return (before.y & 0xFFFFu) != 0u && (before.y >> 16) == (owner & 0xFFFFu) &&
        abs(was_distance - now_distance) <= DEPTH_SHARE * now_distance &&
        dot(unfold(unpackSnorm4x8(before.x).zw), n) >= COS_25;
}

// (SPECULAR) whether the previous frame's pixel q (its filter guide) is on this pixel's surface: drawn, the same owner,
// its point on this one's plane within 3% of the distance, the normal within 25 degrees (the virtual point's pixel
// is another of the same surface, at another distance)
bool same_plane(uvec2 before, ivec2 q, vec3 p, vec3 n, uint owner)
{
    vec3 was = previous_point(q, unpackHalf2x16(before.y).x);
    return (before.y & 0xFFFFu) != 0u && (before.y >> 16) == (owner & 0xFFFFu) &&
        abs(dot(was - p, n)) <= DEPTH_SHARE * length(p - pass.previous[0].xyz) &&
        dot(unfold(unpackSnorm4x8(before.x).zw), n) >= COS_25;
}
