// PORT: SDR->HDR grade ported from Special K (basic_hdr_shader_ps.hlsl main path +
// common_defs.hlsl helpers, main path only, TONEMAP_NONE / non-visualized).
// Input: composed SDR frame (sRGB-encoded). Output: the graded scRGB frame re-encoded as
// gamma 2.2 relative to paper white (see encode_out); the engine's HDR encode decodes it.

struct Params {
    eotf: f32,        // 2.2 = power law, -2.2 = sRGB piecewise curve
    pb_enable: u32,   // perceptual boost / color boost on
    pb: vec4<f32>,    // maxPQValue, PQToLinear p1, LinearToPQ multiplier, divisor
    color_boost: f32, // 0 = luma-only boost, 1 = per-channel
    saturation: f32,
    gamut: f32,       // expandGamut amount, 0 disables
    luma_scale: f32,  // paper_white_nits / 80
    debug_stage: u32, // 0 = full; 1..5 = return after that stage
};

struct VertexOutput {
    @builtin(position) pos: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var scene_tex: texture_2d<f32>;
@group(0) @binding(2) var scene_samp: sampler;

const FLOAT_MAX = 65504.0;          // float_MAX in Special K
const FP16_MIN = 0.0000000596046448; // asfloat(0x33C00000)

// PQ constants (ParamsPQ PQ in common_defs.hlsl)
const PQ_N = 0.1593017578125;
const PQ_RCP_N = 6.277394618035784;
const PQ_M = 78.84375;
const PQ_RCP_M = 0.012683313565541848;
const PQ_C1 = 0.8359375;
const PQ_C2 = 18.8515625;
const PQ_C3 = 18.6875;
// PQToLinear divides by C2 - C3*y, which reaches zero for inputs past the PQ range (overbright
// unclipped surfaces) and would turn into NaN; keep it positive so they saturate instead.
const PQ_DEN_MIN = 0.0001;

// HLSL row-major matrices stored as WGSL columns (= transpose), so HLSL mul(M, v) is v * M here.
const REC709_TO_XYZ = mat3x3<f32>(
    vec3(0.412390798330307, 0.357584327459335, 0.180480793118477),
    vec3(0.212639003992081, 0.715168654918671, 0.072192318737507),
    vec3(0.019330818206072, 0.119194783270359, 0.950532138347626),
);
const XYZ_TO_REC709 = mat3x3<f32>(
    vec3(3.240969896316528, -1.537383198738098, -0.498610764741898),
    vec3(-0.969243645668030, 1.875967502593994, 0.041555058211088),
    vec3(0.055630080401897, -0.203976958990097, 1.056971549987793),
);
const SRGB_2_AP1_D65 = mat3x3<f32>(
    vec3(0.616850994009129, 0.334062934274955, 0.049086071715917),
    vec3(0.069866393979171, 0.917416678964894, 0.012716927055935),
    vec3(0.020549066815885, 0.107642210710817, 0.871808722473298),
);
const AP1_D65_2_SRGB = mat3x3<f32>(
    vec3(1.692679398492150, -0.606218057156000, -0.086461341336150),
    vec3(-0.128573980072268, 1.137933633392290, -0.009359653320017),
    vec3(-0.024022465092119, -0.126211717940702, 1.150234183032820),
);
const AP1_D65_2_XYZ = mat3x3<f32>(
    vec3(0.647292657846805, 0.134403399178057, 0.168471065430319),
    vec3(0.265998245089921, 0.676089826168407, 0.057911928741672),
    vec3(-0.005447063039384, 0.004072830278123, 1.089797204502370),
);
const WIDE_2_AP1_D65 = mat3x3<f32>(
    vec3(0.834516905462339, 0.160259589549493, 0.005223504988168),
    vec3(0.025545193577855, 0.973101531866070, 0.001353274556075),
    vec3(0.001925828854283, 0.030372797012442, 0.967701374133275),
);
const AP1_2_SRGB = mat3x3<f32>(
    vec3(1.70505, -0.62179, -0.08326),
    vec3(-0.13026, 1.14080, -0.01055),
    vec3(-0.02400, -0.12897, 1.15297),
);

// This tint predates the copysign builtin; NaN in s takes the + branch.
fn copy_sign(v: f32, s: f32) -> f32 {
    return select(v, -v, s < 0.0);
}

fn copy_sign3(v: vec3<f32>, s: vec3<f32>) -> vec3<f32> {
    return select(v, -v, s < vec3(0.0));
}

// SanitizeFP: NaN -> 0, +Inf -> +float_MAX, -Inf -> -float_MAX (componentwise;
// WGSL bitcast is scalar-only).
fn sanit(x: f32) -> f32 {
    let mag = bitcast<u32>(x) & 0x7fffffffu;
    if (mag < 0x7f800000u) {
        return x;
    }
    if (mag == 0x7f800000u) {
        return copy_sign(FLOAT_MAX, x);
    }
    return 0.0;
}

fn sanitize3(c: vec3<f32>) -> vec3<f32> {
    return vec3(sanit(c.x), sanit(c.y), sanit(c.z));
}

fn rec709_to_xyz(c: vec3<f32>) -> vec3<f32> {
    return c * REC709_TO_XYZ;
}

fn xyz_to_rec709(c: vec3<f32>) -> vec3<f32> {
    return c * XYZ_TO_REC709;
}

fn luminance(c: vec3<f32>) -> f32 {
    return max(0.0, dot(c, vec3(0.2627, 0.678, 0.0593)));
}

// Saturation (vibrance) from common_defs.hlsl:490.
fn saturation(c: vec3<f32>, sat: f32) -> vec3<f32> {
    let luma = luminance(c);
    return vec3(luma) + sat * (c - vec3(luma));
}

fn linear_to_pq3(x: vec3<f32>, max_pq: f32) -> vec3<f32> {
    let y = pow(max(x, vec3(0.0)) / max_pq, vec3(PQ_N));
    return pow((PQ_C1 + PQ_C2 * y) / (1.0 + PQ_C3 * y), vec3(PQ_M));
}

fn linear_to_pq4(x: vec4<f32>, max_pq: f32) -> vec4<f32> {
    let y = pow(max(x, vec4(0.0)) / max_pq, vec4(PQ_N));
    return pow((PQ_C1 + PQ_C2 * y) / (1.0 + PQ_C3 * y), vec4(PQ_M));
}

fn pq_to_linear3(x: vec3<f32>, max_pq: f32) -> vec3<f32> {
    let y = pow(max(x, vec3(0.0)), vec3(PQ_RCP_M));
    let nd = max(y - PQ_C1, vec3(0.0)) / max(PQ_C2 - PQ_C3 * y, vec3(PQ_DEN_MIN));
    return pow(nd, vec3(PQ_RCP_N)) * max_pq;
}

fn pq_to_linear4(x: vec4<f32>, max_pq: f32) -> vec4<f32> {
    let y = pow(max(x, vec4(0.0)), vec4(PQ_RCP_M));
    let nd = max(y - PQ_C1, vec4(0.0)) / max(PQ_C2 - PQ_C3 * y, vec4(PQ_DEN_MIN));
    return pow(nd, vec4(PQ_RCP_N)) * max_pq;
}

// SK_ProcessColor4 xRGB_to_Linear: sign-preserving power/sRGB EOTF.
fn eotf_decode(c: vec3<f32>) -> vec3<f32> {
    let a = abs(c);
    if (params.eotf < 0.0) {
        // RemoveSRGBCurve (sRGB piecewise)
        let lin = select(pow((a + vec3(0.055)) / vec3(1.055), vec3(2.4)),
                         a / vec3(12.92), a <= vec3(0.04045));
        return copy_sign3(lin, c);
    }
    return copy_sign3(pow(a, vec3(params.eotf)), c);
}

// expandGamut from common_defs.hlsl:2046 (ExpandMat = Wide_2_AP1 * AP1_2_sRGB,
// applied here as chained multiplies).
fn expand_gamut(v: vec3<f32>, g: f32) -> vec3<f32> {
    if (g <= 0.0) {
        return v;
    }
    let color_ap1 = v * SRGB_2_AP1_D65;
    let luma_ap1 = rec709_to_xyz(xyz_to_rec709(color_ap1 * AP1_D65_2_XYZ)).y;
    var chroma = color_ap1 / max(luma_ap1, 0.000001);
    var chroma_dist = max(abs(dot(chroma - vec3(1.0), chroma - vec3(1.0))), 0.000001);
    let expand_amount = (1.0 - exp2(-4.0 * chroma_dist)) *
        (1.0 - exp2(-4.0 * g * luma_ap1 * luma_ap1));
    let color_expand = (color_ap1 * AP1_D65_2_SRGB) * WIDE_2_AP1_D65;
    return mix(color_ap1, color_expand, expand_amount) * AP1_2_SRGB;
}

// Write the frame in the engine's SDR-gamma convention (it decodes sign*|c|^2.2 * paper/80),
// so HDR values round-trip exactly while anything copying the frame as SDR (menu backdrops)
// just clips highlights instead of misreading linear data.
fn encode_out(c: vec3<f32>, a: f32) -> vec4<f32> {
    return vec4(copy_sign3(pow(abs(c) / params.luma_scale, vec3(1.0 / 2.2)), c), a);
}

var<private> corners: array<vec2<f32>, 3> = array<vec2<f32>, 3>(
    vec2(-1.0, 1.0),
    vec2(-1.0, -3.0),
    vec2(3.0, 1.0),
);
var<private> uvs: array<vec2<f32>, 3> = array<vec2<f32>, 3>(
    vec2(0.0, 0.0),
    vec2(0.0, 2.0),
    vec2(2.0, 0.0),
);

@vertex
fn vs_main(@builtin(vertex_index) vtx_idx: u32) -> VertexOutput {
    var out: VertexOutput;
    out.pos = vec4(corners[vtx_idx], 0.0, 1.0);
    out.uv = uvs[vtx_idx];
    return out;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
    let sample = textureSample(scene_tex, scene_samp, in.uv);
    let alpha = clamp(sample.a, 0.0, 1.0);
    if (params.debug_stage == 6u) {
        // Input check: green = NaN, yellow = Inf, red = negative, blue = above 1, else gray.
        let mag = vec3(bitcast<u32>(sample.r), bitcast<u32>(sample.g), bitcast<u32>(sample.b)) &
            vec3(0x7fffffffu);
        var flag = vec3(dot(abs(sample.rgb), vec3(0.2126, 0.7152, 0.0722)));
        if (any(mag > vec3(0x7f800000u))) {
            flag = vec3(0.0, 1.0, 0.0);
        } else if (any(mag == vec3(0x7f800000u))) {
            flag = vec3(1.0, 1.0, 0.0);
        } else if (any(sample.rgb < vec3(0.0))) {
            flag = vec3(1.0, 0.0, 0.0);
        } else if (any(sample.rgb > vec3(1.0))) {
            flag = vec3(0.0, 0.0, 1.0);
        }
        return encode_out(clamp(flag, vec3(0.0), vec3(1.0)) * params.luma_scale, 1.0);
    }

    // SK main: orig_color = abs(color); sanitize; normalize past 1.0 through the EOTF.
    let orig_color = abs(sample.rgb);
    var c = sanitize3(sample.rgb);

    let max_rgb = max(1.0, max(abs(c.x), max(abs(c.y), abs(c.z))));
    c = eotf_decode(c / max_rgb) * max_rgb;
    if (params.debug_stage == 1u) {
        return encode_out(c * params.luma_scale, 1.0);
    }

    // Perceptual boost / color boost (SK pqBoostParams "optimization for both" path).
    if (params.pb_enable != 0u && params.pb.x > 0.1) {
        // The boost curve is only sane for SDR-range input: far above it each XYZ channel blows up
        // differently (purple/black). Scale overbright input (additive blends, lighting gain) back
        // under 2.0, boost, and scale up again; SDR-range input is untouched.
        let xyz_in = max(rec709_to_xyz(c), vec3(0.0));
        let s = max(1.0, max(xyz_in.x, max(xyz_in.y, xyz_in.z)) * 0.5);
        let xyz = xyz_in / s;
        let luma = xyz.y;
        if (luma > 0.0) {
            let boosted = pq_to_linear4(
                linear_to_pq4(vec4(xyz, luma), params.pb.x) * params.pb.z,
                params.pb.y) / params.pb.w;
            c = xyz_to_rec709(mix(xyz * boosted.www / luma, boosted.rgb, params.color_boost)) * s;
        }
    }

    if (params.debug_stage == 2u) {
        return encode_out(c * params.luma_scale, 1.0);
    }

    c = saturation(c, params.saturation);
    c = c * params.luma_scale;
    if (params.debug_stage == 3u) {
        return encode_out(c, 1.0);
    }

    c = expand_gamut(c, params.gamut);
    if (params.debug_stage == 4u) {
        return encode_out(c, 1.0);
    }

    // Clamp_scRGB + keep pure black pixels (FP16 precision guard).
    c = clamp(sanitize3(c), vec3(-FLOAT_MAX), vec3(FLOAT_MAX));
    c = c * select(vec3(0.0), vec3(1.0), dot(orig_color, vec3(1.0)) > FP16_MIN);
    if (params.debug_stage == 5u) {
        return encode_out(c, 1.0);
    }

    return encode_out(c, alpha);
}
