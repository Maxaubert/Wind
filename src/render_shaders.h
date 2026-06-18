#pragma once
namespace wind {

// Constant buffer: source sub-rect UV bounds, output brightness, HDR tonemap params, sharpening
// strength, and the source texel size. 48 bytes (three 16-byte registers).
// hdrMode: 0 = SDR passthrough, 1 = scRGB (FP16 linear Rec.709) -> SDR.
// scRgbScale = 80 / SDR-white-nits (scRGB 1.0 = 80 nits; SDR white maps to 1.0).
// sharpness: 0 = off (single tap, cheapest); >0 = adaptive sharpen strength.
// texelW/texelH = 1/textureWidth, 1/textureHeight (for neighbor offsets).
struct MagCB {
    float uvMinX, uvMinY, uvMaxX, uvMaxY;             // reg 0
    float brightness, hdrMode, scRgbScale, sharpness; // reg 1
    float texelW, texelH, pad0, pad1;                 // reg 2
};

// Fullscreen-triangle magnify shader. The VS maps the visible [0,1] screen UV into the
// source sub-rect; the PS samples the captured desktop, optionally sharpens (adaptive, clamped to
// the local neighborhood so it doesn't halo), then applies the HDR->SDR tonemap and brightness.
inline constexpr const char* kMagHLSL = R"(
cbuffer CB : register(b0) {
    float2 uvMin; float2 uvMax;
    float brightness; float hdrMode; float scRgbScale; float sharpness;
    float texelW; float texelH; float2 pad;
};
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
    float2 t = float2((id << 1) & 2, id & 2);   // (0,0),(2,0),(0,2)
    VSOut o;
    o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv  = lerp(uvMin, uvMax, t);               // visible region samples [uvMin, uvMax]
    return o;
}
Texture2D tex : register(t0);
SamplerState smp : register(s0);
float3 LinearToSrgb(float3 l) {
    l = saturate(l);
    return (l <= 0.0031308) ? (l * 12.92) : (1.055 * pow(l, 1.0 / 2.4) - 0.055);
}
float4 PSMain(VSOut i) : SV_TARGET {
    float4 c = tex.Sample(smp, i.uv);
    if (sharpness > 0.0) {
        // Clamped unsharp (CAS-flavored): sharpen at output resolution using 4 cross neighbors one
        // output pixel away, then clamp to the local min/max so edges crisp up without halos.
        float2 d = (uvMax - uvMin) * float2(texelW, texelH);   // UV step of one output pixel
        float3 n = tex.Sample(smp, i.uv + float2(0.0, -d.y)).rgb;
        float3 s = tex.Sample(smp, i.uv + float2(0.0,  d.y)).rgb;
        float3 e = tex.Sample(smp, i.uv + float2( d.x, 0.0)).rgb;
        float3 w = tex.Sample(smp, i.uv + float2(-d.x, 0.0)).rgb;
        float3 sharp = c.rgb + sharpness * (4.0 * c.rgb - n - s - e - w);
        float3 mn = min(c.rgb, min(min(n, s), min(e, w)));
        float3 mx = max(c.rgb, max(max(n, s), max(e, w)));
        c.rgb = clamp(sharp, mn, mx);
    }
    if (hdrMode > 0.5) {
        // FP16 scRGB source (linear Rec.709, 1.0 = 80 nits): scale so SDR white -> 1.0,
        // then sRGB-encode. Reconstructs the SDR appearance the HDR desktop shows.
        c.rgb = LinearToSrgb(max(c.rgb, 0.0) * scRgbScale);
    }
    c.rgb *= brightness;                         // optional fine-tune (default 1.0)
    c.a = 1.0;                                   // opaque output; window opacity is set via LWA_ALPHA
    return c;
}
)";

// Cursor quad shader: a per-quad transform (top-left + size in clip space) places an
// alpha-blended textured quad. Drawn as a 4-vertex triangle strip from the vertex id.
inline constexpr const char* kCursorHLSL = R"(
cbuffer CB : register(b0) { float2 posClip; float2 sizeClip; };
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
    float2 q = float2(id & 1, (id >> 1) & 1);   // (0,0),(1,0),(0,1),(1,1)
    VSOut o;
    o.pos = float4(posClip + q * sizeClip, 0, 1);
    o.uv  = q;
    return o;
}
Texture2D tex : register(t0);
SamplerState smp : register(s0);
float4 PSMain(VSOut i) : SV_TARGET { return tex.Sample(smp, i.uv); }
)";

// Zoom edge outline as a SINGLE full-screen quad: the pixel shader colors only pixels within
// `thickness` px of a screen edge and discards the interior. One draw, no per-edge geometry - this
// replaces an earlier four-quads-in-a-loop approach that dropped individual edges on some GPUs.
// cb: rgba color + screen size (px) + thickness (px).
inline constexpr const char* kBorderHLSL = R"(
cbuffer CB : register(b0) { float4 color; float2 screen; float thickness; float pad; };
struct VSOut { float4 pos : SV_POSITION; };
VSOut VSMain(uint id : SV_VertexID) {
    float2 q = float2(id & 1, (id >> 1) & 1);   // (0,0),(1,0),(0,1),(1,1)
    VSOut o;
    o.pos = float4(q * float2(2.0, -2.0) + float2(-1.0, 1.0), 0, 1);   // full-screen quad
    return o;
}
float4 PSMain(VSOut i) : SV_TARGET {
    float2 p = i.pos.xy;   // pixel center in screen px
    if (p.x >= thickness && p.x < screen.x - thickness &&
        p.y >= thickness && p.y < screen.y - thickness) discard;   // interior -> not part of the border
    return color;
}
)";

}
