"""Print the main display's transfer formula maxima (red green blue) - used by e2e.py to check the colour filter."""
import ctypes as C
CG = C.CDLL("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics")
CG.CGMainDisplayID.restype = C.c_uint32
v = [C.c_float() for _ in range(9)]
CG.CGGetDisplayTransferByFormula(CG.CGMainDisplayID(), *[C.byref(x) for x in v])
print(" ".join(f"{v[i].value:.3f}" for i in (1, 4, 7)))
