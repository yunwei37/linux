# Capture fork, never linked with MODELOS_ROLE_BOOT.
ifeq ($(MODELOS_NVIDIA_SOURCE),)
$(error MODELOS_NVIDIA_SOURCE must name the canonical capture fork)
endif
src := $(MODELOS_NVIDIA_SOURCE)/kernel-open
include $(src)/Kbuild

# Sources belong to the capture fork, outside Linux's srcroot/VPATH.
# Reuse kbuild's ordinary compile rule with the explicit source prerequisite.
$(obj)/%.o: $(src)/%.c FORCE
	$(call if_changed_rule,cc_o_c)
