// W3, the denoiser's shared GLSL (xgpu_vulkan_denoise.c; ~/dev/rt2-realtime-design.md section 1.5): its uniforms, the
// pre-pass's inputs and guides read (xgpu_spirv.h's C1), and the reprojection to the frame before.

// (xgpu.h, xgpu_spirv.h)
#define INPUT_PRESENT 0x400u
#define MOTION_NONE 0u

// control.y's flags (xgpu_vulkan_denoise.c's)
#define HISTORY 1
#define GRADIENTS 2
#define FIRST 4
#define CAMERA_ONLY 8
// (checking, the negative control: an exponential average from the start, the history's count held at the cap)
#define EMA 16

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

// where p was in the frame before, in its pixels (continuous: a pixel's middle at +0.5), from the cameras (E1's own
// reprojection, lt/e1.frag), or the guides' motion where they carry it; false off its view
bool reproject(vec3 p, ivec2 pixel, uint motion, out vec2 at)
{
    if (motion != MOTION_NONE && (pass.control.y & CAMERA_ONLY) == 0)
    {
        // (C1's motion: pixels from where the point was the frame before)
        at = vec2(pixel) + 0.5 - unpackHalf2x16(motion);
    }
    else
    {
        vec3 w = p - pass.previous[0].xyz, pf = pass.previous[1].xyz, pr = pass.previous[2].xyz;
        vec3 pu = pass.previous[3].xyz;
        float along = dot(w, pf) / dot(pf, pf);
        if (along <= 0.0)
            return false;
        vec2 was = vec2(dot(w, pr) / dot(pr, pr), dot(w, pu) / dot(pu, pu)) / along;
        at = vec2(pass.previous_corners.xy) + (was + 1.0) * 0.5 * vec2(pass.previous_corners.zw - pass.previous_corners.xy);
    }
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
