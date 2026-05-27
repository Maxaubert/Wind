#pragma once
namespace wind {

// Constant buffer: source sub-rect UV bounds, output brightness, and HDR tonemap params.
// 32 bytes (two 16-byte registers).
// hdrMode: 0 = SDR passthrough, 1 = scRGB (FP16 linear Rec.709) -> SDR.
// scRgbScale = 80 / SDR-white-nits (scRGB 1.0 = 80 nits; SDR white maps to 1.0).
struct MagCB {
    float uvMinX, uvMinY, uvMaxX, uvMaxY;        // reg 0
    float brightness, hdrMode, scRgbScale, pad0; // reg 1
};

// Fullscreen-triangle magnify shader. The VS maps the visible [0,1] screen UV into the
// source sub-rect; the PS samples the captured desktop and applies the optional HDR->SDR
// tonemap and brightness.
inline constexpr const char* kMagHLSL = R"(
cbuffer CB : register(b0) {
    float2 uvMin; float2 uvMax;
    float brightness; float hdrMode; float scRgbScale; float pad0;
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
    if (hdrMode > 0.5) {
        // FP16 scRGB source (linear Rec.709, 1.0 = 80 nits): scale so SDR white -> 1.0,
        // then sRGB-encode. Reconstructs the SDR appearance the HDR desktop shows.
        c.rgb = LinearToSrgb(max(c.rgb, 0.0) * scRgbScale);
    }
    c.rgb *= brightness;                         // optional fine-tune (default 1.0)
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

}
