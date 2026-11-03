# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
"""Turns the Windows render engine's xBR pixel shader (src/render/upscale_shaders.h, HLSL) into C++ for
up.cpp (ported.inc), so the Mac's Metal pass is checked against the Windows source itself.
usage: gen_ported.py <repo root> <out ported.inc>"""
import re, sys
root, out = sys.argv[1], sys.argv[2]
src = open(root + '/src/render/upscale_shaders.h').read()
xbr = re.search(r'kUpSrcXbr = R"\((.*?)\)";', src, re.S).group(1)
xbr = xbr.replace('return float4(A < B);', 'return lt4(A, B);').replace('return float4(A != B);', 'return ne4(A, B);')
for s in ['yzwx', 'wxyz', 'zwxy', 'yxwz']:
    xbr = re.sub(r'(\w+)\.' + s + r'\b', lambda m: f'SW({m.group(1)},{",".join(str("xyzw".index(c)) for c in s)})', xbr)
xbr = xbr.replace('float3 Upscale(float2 uv)', 'float3 UpscaleXbr(float2 uv)')
open(out, 'w').write('namespace X {\n' + xbr + '\n}\n')
