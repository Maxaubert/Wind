// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// up: the CPU reference for the Render engine's scaling filters, on the Mac (scripts/e2e_draw.py).
// xBR is Wind's Windows HLSL itself, made C++ by gen_ported.py; nearest is plain. SDR, no colour,
// one output pixel per thread row.
//   up <in.ppm> <out.ppm> <filter> <scale> [ox oy ow oh]
// filter: nearest xbr. Output pixel (x, y) samples the source at
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
#include PORTED
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
            else if (flt == "xbr") c = X::UpscaleXbr(uv);
            else { fprintf(stderr, "bad filter %s\n", flt.c_str()); exit(1); }
            c = saturate(c); unsigned char* o = &out[(y * Wo + x) * 3];
            o[0] = (unsigned char)std::lround(c.x * 255); o[1] = (unsigned char)std::lround(c.y * 255); o[2] = (unsigned char)std::lround(c.z * 255);
        } };
    int n = std::max(1u, std::thread::hardware_concurrency()); std::vector<std::thread> th;
    for (int i = 0; i < n; ++i) th.emplace_back(run, Ho * i / n, Ho * (i + 1) / n);
    for (auto& t : th) t.join();
    FILE* f = fopen(argv[2], "wb"); fprintf(f, "P6 %d %d 255\n", Wo, Ho); fwrite(out.data(), 1, out.size(), f); fclose(f);
}
