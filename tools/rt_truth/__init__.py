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
