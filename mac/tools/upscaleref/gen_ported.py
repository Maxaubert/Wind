# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
"""Turns the Windows render engine's xBR and NIS pixel shaders (src/render/upscale_shaders.h and
upscale_nis.h, HLSL) into C++ for up.cpp (ported.inc), so the Mac's Metal pass is checked against the
Windows source itself. usage: gen_ported.py <repo root> <out ported.inc>"""
import re, sys
root, out = sys.argv[1], sys.argv[2]
src = open(root + '/src/render/upscale_shaders.h').read(); nis = open(root + '/src/render/upscale_nis.h').read()
lit = lambda t, n: re.search(n + r' = R"\((.*?)\)";', t, re.S).group(1)
xbr, coef, nisb = lit(src, 'kUpSrcXbr'), lit(nis, 'kUpNisCoef'), lit(nis, 'kUpSrcNis')
xbr = xbr.replace('return float4(A < B);', 'return lt4(A, B);').replace('return float4(A != B);', 'return ne4(A, B);')
for s in ['yzwx', 'wxyz', 'zwxy', 'yxwz']:
    xbr = re.sub(r'(\w+)\.' + s + r'\b', lambda m: f'SW({m.group(1)},{",".join(str("xyzw".index(c)) for c in s)})', xbr)
xbr = xbr.replace('float3 Upscale(float2 uv)', 'float3 UpscaleXbr(float2 uv)')
nisb = nisb.replace('[unroll] ', '').replace('float3 Upscale(float2 uv)', 'float3 UpscaleNis(float2 uv)').replace('return 0;', 'return float4(0.f);')
nisb = nisb.replace('static float P[6][6];', 'static thread_local float P[6][6];')
open(out, 'w').write('namespace X {\n' + xbr + '\n}\nnamespace N {\n' + coef + nisb + '\n}\n')
