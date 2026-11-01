obj-y += serve_core.o qwen_fp.o
CFLAGS_REMOVE_qwen_fp.o += $(CC_FLAGS_NO_FPU)
CFLAGS_qwen_fp.o += $(CC_FLAGS_FPU) -mavx2 -mfma -O3 -ffp-contract=off -fno-math-errno -fno-strict-aliasing $(call cc-option,-mpreferred-stack-boundary=5) -mstackrealign
