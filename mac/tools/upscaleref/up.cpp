// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// up: the CPU reference for the Render engine's scaling filters, on the Mac (scripts/e2e_draw.py).
// xBR and NIS are Wind's Windows HLSL itself, made C++ by gen_ported.py; nearest, bilinear and the
// bicubic kernels are plain. SDR, no colour, one output pixel per thread row.
//   up <in.ppm> <out.ppm> <filter> <scale> [ox oy ow oh]
// filter: nearest bilinear catmull mitchell bspline xbr nis. Output pixel (x, y) samples the source at
// texel (ox + (x + 0.5) / scale, oy + (y + 0.5) / scale), as Wind's pass does for a view whose top-left
// source texel is (ox, oy); without a view the whole input is scaled.
// Adapted from research/wind-mac/2026-11-03-upscale-windows-reference/tool/up.cpp (2026-11-03).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
static int W, H; static float K; static std::vector<float3> img;
static float3 texel(int x, int y) { x = clamp(x, 0, W - 1); y = clamp(y, 0, H - 1); return img[y * W + x]; }
float2 SrcSize() { return float2((float)W, (float)H); }
float2 Texel() { return float2(1.f / W, 1.f / H); }
float Scale() { return K; }
float3 Tap(float2 t) { return texel((int)std::floor(t.x), (int)std::floor(t.y)); }
float3 Bilin(float2 uv) {
    float sx = uv.x * W - 0.5f, sy = uv.y * H - 0.5f; int x0 = (int)std::floor(sx), y0 = (int)std::floor(sy);
    float fx = sx - x0, fy = sy - y0;
    return lerp(lerp(texel(x0, y0), texel(x0 + 1, y0), fx), lerp(texel(x0, y0 + 1), texel(x0 + 1, y0 + 1), fx), fy);
}
float Luma(float3 c) { return dot(c, float3(0.2126f, 0.7152f, 0.0722f)); }
#include PORTED
// Mitchell-Netravali family: (B, C) = (0, 0.5) Catmull-Rom, (1/3, 1/3) Mitchell, (1, 0) cubic B-spline.
static float bc(float x, float B, float C) {
    x = std::fabs(x);
    if (x < 1) return ((12 - 9 * B - 6 * C) * x * x * x + (-18 + 12 * B + 6 * C) * x * x + (6 - 2 * B)) / 6;
    if (x < 2) return ((-B - 6 * C) * x * x * x + (6 * B + 30 * C) * x * x + (-12 * B - 48 * C) * x + (8 * B + 24 * C)) / 6;
    return 0;
}
static float3 Cubic(float2 uv, float B, float C) {
    float sx = uv.x * W - 0.5f, sy = uv.y * H - 0.5f; int x0 = (int)std::floor(sx), y0 = (int)std::floor(sy);
    float3 acc(0.f); float ws = 0;
    for (int j = -1; j <= 2; ++j) { float wy = bc(sy - (y0 + j), B, C);
        for (int i = -1; i <= 2; ++i) { float w = wy * bc(sx - (x0 + i), B, C); acc = acc + texel(x0 + i, y0 + j) * w; ws += w; } }
    return acc / ws;
}
static bool readPPM(const char* p) {
    FILE* f = fopen(p, "rb"); if (!f) return false; int mx; if (fscanf(f, "P6 %d %d %d", &W, &H, &mx) != 3) return false; fgetc(f);
    std::vector<unsigned char> b(W * H * 3); if (fread(b.data(), 1, b.size(), f) != b.size()) return false; fclose(f);
    img.resize(W * H); for (int i = 0; i < W * H; ++i) img[i] = float3(b[i*3] / 255.f, b[i*3+1] / 255.f, b[i*3+2] / 255.f); return true;
}
int main(int argc, char** argv) {
    if (argc < 5 || !readPPM(argv[1])) { fprintf(stderr, "usage: up <in.ppm> <out.ppm> <filter> <scale> [ox oy ow oh]\n"); return 1; }
    std::string flt = argv[3]; K = (float)atof(argv[4]);
    float ox = 0, oy = 0; int Wo = (int)std::lround(W * K), Ho = (int)std::lround(H * K);
    if (argc >= 9) { ox = (float)atof(argv[5]); oy = (float)atof(argv[6]); Wo = atoi(argv[7]); Ho = atoi(argv[8]); }
    std::vector<unsigned char> out(Wo * Ho * 3);
    auto run = [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) for (int x = 0; x < Wo; ++x) {
            float2 uv((ox + (x + 0.5f) / K) / W, (oy + (y + 0.5f) / K) / H); float3 c;
            if (flt == "nearest") c = Tap(floor(uv * SrcSize()));
            else if (flt == "bilinear") c = Bilin(uv);
            else if (flt == "catmull") c = Cubic(uv, 0, 0.5f);
            else if (flt == "mitchell") c = Cubic(uv, 1.f / 3, 1.f / 3);
            else if (flt == "bspline") c = Cubic(uv, 1, 0);
            else if (flt == "xbr") c = X::UpscaleXbr(uv);
            else if (flt == "nis") c = N::UpscaleNis(uv);
            else { fprintf(stderr, "bad filter %s\n", flt.c_str()); exit(1); }
            c = saturate(c); unsigned char* o = &out[(y * Wo + x) * 3];
            o[0] = (unsigned char)std::lround(c.x * 255); o[1] = (unsigned char)std::lround(c.y * 255); o[2] = (unsigned char)std::lround(c.z * 255);
        } };
    int n = std::max(1u, std::thread::hardware_concurrency()); std::vector<std::thread> th;
    for (int i = 0; i < n; ++i) th.emplace_back(run, Ho * i / n, Ho * (i + 1) / n);
    for (auto& t : th) t.join();
    FILE* f = fopen(argv[2], "wb"); fprintf(f, "P6 %d %d 255\n", Wo, Ho); fwrite(out.data(), 1, out.size(), f); fclose(f);
}
