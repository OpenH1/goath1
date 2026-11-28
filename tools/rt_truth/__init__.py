"""rt_truth: the truth renderer of the rt2 scene format. Mitsuba's variant is chosen here, before the
plugins are defined: RT_TRUTH_VARIANT (cuda_ad_rgb by default, llvm_ad_rgb when there is no GPU)."""

import os

import mitsuba as mi

VARIANT = os.environ.get("RT_TRUTH_VARIANT", "cuda_ad_rgb")
try:
    mi.set_variant(VARIANT)
except Exception:
    VARIANT = "llvm_ad_rgb"
    mi.set_variant(VARIANT)

# Evaluated (wavefront) loops and calls: a kernel per plugin instance, each small and cached, instead of one
# megakernel holding every material's code (whose compile takes over ten minutes on a 160-shader map).
# RT_TRUTH_MEGAKERNEL=1 keeps the symbolic megakernel.
import drjit as _dr
if os.environ.get("RT_TRUTH_MEGAKERNEL") != "1":
    _dr.set_flag(_dr.JitFlag.SymbolicLoops, False)
    _dr.set_flag(_dr.JitFlag.SymbolicCalls, False)
