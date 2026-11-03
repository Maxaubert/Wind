// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// float2/3/4 and the HLSL intrinsics the Windows shaders use, for the CPU reference (up.cpp).
#pragma once
#include <cmath>
#include <algorithm>
struct float2 { float x, y; float2() : x(0), y(0) {} float2(float v) : x(v), y(v) {} float2(float a, float b) : x(a), y(b) {} };
struct float3 { float x, y, z; float3() : x(0), y(0), z(0) {} float3(float v) : x(v), y(v), z(v) {} float3(float a, float b, float c) : x(a), y(b), z(c) {} };
struct float4 { float x, y, z, w; float4() : x(0), y(0), z(0), w(0) {} float4(float s) : x(s), y(s), z(s), w(s) {} float4(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {}
  float& operator[](int i) { return (&x)[i]; } float operator[](int i) const { return (&x)[i]; } };
#define F4OP(op) inline float4 operator op(float4 a, float4 b) { return float4(a[0] op b[0], a[1] op b[1], a[2] op b[2], a[3] op b[3]); } \
 inline float4 operator op(float a, float4 b) { return float4(a) op b; } inline float4 operator op(float4 a, float b) { return a op float4(b); }
F4OP(+) F4OP(-) F4OP(*) F4OP(/)
#define F3OP(op) inline float3 operator op(float3 a, float3 b) { return float3(a.x op b.x, a.y op b.y, a.z op b.z); } \
 inline float3 operator op(float a, float3 b) { return float3(a) op b; } inline float3 operator op(float3 a, float b) { return a op float3(b); }
F3OP(+) F3OP(-) F3OP(*) F3OP(/)
#define F2OP(op) inline float2 operator op(float2 a, float2 b) { return float2(a.x op b.x, a.y op b.y); } \
 inline float2 operator op(float a, float2 b) { return float2(a) op b; } inline float2 operator op(float2 a, float b) { return a op float2(b); }
F2OP(+) F2OP(-) F2OP(*) F2OP(/)
inline float4 operator-(float4 a) { return 0.f - a; }
inline float saturate(float a) { return std::min(1.f, std::max(0.f, a)); }
inline float4 saturate(float4 a) { return float4(saturate(a[0]), saturate(a[1]), saturate(a[2]), saturate(a[3])); }
inline float3 saturate(float3 a) { return float3(saturate(a.x), saturate(a.y), saturate(a.z)); }
inline float step(float e, float x) { return x >= e ? 1.f : 0.f; }
inline float4 step(float4 e, float4 x) { return float4(step(e[0],x[0]), step(e[1],x[1]), step(e[2],x[2]), step(e[3],x[3])); }
inline float4 lt4(float4 a, float4 b) { return float4(a[0]<b[0], a[1]<b[1], a[2]<b[2], a[3]<b[3]); }
inline float4 ne4(float4 a, float4 b) { return float4(a[0]!=b[0], a[1]!=b[1], a[2]!=b[2], a[3]!=b[3]); }
inline float4 SW(float4 a, int i, int j, int k, int l) { return float4(a[i], a[j], a[k], a[l]); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float3 lerp(float3 a, float3 b, float t) { return a + (b - a) * t; }
inline float4 lerp(float4 a, float4 b, float t) { return a + (b - a) * t; }
inline float dot(float3 a, float3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
inline float3 abs(float3 a) { return float3(std::fabs(a.x), std::fabs(a.y), std::fabs(a.z)); }
inline float max(float a, float b) { return a > b ? a : b; }
inline float min(float a, float b) { return a < b ? a : b; }
inline float4 max(float4 a, float4 b) { return float4(max(a[0],b[0]), max(a[1],b[1]), max(a[2],b[2]), max(a[3],b[3])); }
inline float3 max(float3 a, float b) { return float3(max(a.x,b), max(a.y,b), max(a.z,b)); }
inline float clamp(float a, float lo, float hi) { return std::min(hi, std::max(lo, a)); }
inline int clamp(int a, int lo, int hi) { return std::min(hi, std::max(lo, a)); }
inline float2 floor(float2 a) { return float2(std::floor(a.x), std::floor(a.y)); }
